#include "caen_digitizer.h"

#include <CAEN_FELib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <epicsThread.h>
#include <epicsTime.h>
#include <errlog.h>

typedef epicsGuard<epicsMutex> Guard;
typedef epicsGuardRelease<epicsMutex> UnGuard;

using json = nlohmann::json;

static const uint64_t NO_HANDLE = -1;

// How many pending write messages can there be
static const size_t MAX_PENDING_WRITES = 1024;

// Maximum size of an individual value to be written
static const size_t MAX_PENDING_WRITE_LEN = 2048;

// Maximum time to wait for a write parameter command
static const double WAIT_FOR_WRITE_SECS = 1.0f;

// Maximum time to wait for data
static const int WAIT_FOR_DATA_MSECS = 100;

// Maximum time to wait for all threads to join (when exiting)
static const double WAIT_FOR_THEADS_SECS = 5.0f;

// Expected configuration for the device
static const std::string ACTIVE_ENDPOINT("/par/ActiveEndpoint");
static const std::string SCOPE("scope");
static const std::string DATA_FORMAT("\
    [ \
        { \"name\" : \"TIMESTAMP\", \"type\" : \"U64\" }, \
        { \"name\" : \"TRIGGER_ID\", \"type\" : \"U32\" }, \
        { \"name\" : \"WAVEFORM\", \"type\" : \"U16\", \"dim\" : 2 }, \
        { \"name\" : \"WAVEFORM_SIZE\", \"type\" : \"SIZE_T\", \"dim\" : 1 }, \
        { \"name\" : \"EVENT_SIZE\", \"type\" : \"SIZE_T\" } \
    ] \
");


struct PendingWrite {
    enum Type { Param, Command } type;
    char path[128];
    char value[MAX_PENDING_WRITE_LEN-sizeof(type)-sizeof(path)];
};

static std::string str_tolower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return s;
}

static void throw_if_err(int ec, const std::string & msg) {
    if (ec < 0) {
        char err_name[512];
        char err_desc[512];
        CAEN_FELib_GetErrorName(static_cast<CAEN_FELib_ErrorCode>(ec), err_name);
        CAEN_FELib_GetErrorDescription(static_cast<CAEN_FELib_ErrorCode>(ec), err_desc);
        char exc_desc[2048];
        snprintf(exc_desc, sizeof(exc_desc), "%s: %s. %s", msg.c_str(), err_name, err_desc);
        throw std::runtime_error(exc_desc);
    }
}

CaenDigitizerParam::CaenDigitizerParam(CaenDigitizer *parent, const std::string & path)
: parent_(parent), path_(path),
  type_(path.find("/cmd") == 0 ? CaenDigitizerParam::Type::Command : CaenDigitizerParam::Type::Parameter),
  handle_(NO_HANDLE), value_()
{
    reset();
}

IOSCANPVT CaenDigitizerParam::get_status_update() {
    return parent_->status_update;
}

void CaenDigitizerParam::reset() {
    handle_ = NO_HANDLE;
    value_.clear();
}

void CaenDigitizerParam::set(uint64_t handle, const std::string & value) {
    value_ = value;
    handle_ = handle;
}

void CaenDigitizerParam::get_value(std::string & v) {
    if (handle_ != NO_HANDLE)
        v = value_;
    else
        // TODO: improve error path
        throw std::runtime_error("INVALID HANDLE");
}

void CaenDigitizerParam::get_value(int64_t & v) {
    std::string value;
    get_value(value);
    v = std::stol(value);
}

void CaenDigitizerParam::get_value(double & v) {
    std::string value;
    get_value(value);
    v = std::stod(value);
}

void CaenDigitizerParam::get_value(bool & v) {
    std::string value;
    get_value(value);
    v = value[0]=='T' || value[0]=='t';
}

void CaenDigitizerParam::set_value(const std::string & v) {
    parent_->write_parameter(path_, v);
}

void CaenDigitizerParam::set_value(int64_t v) {
    set_value(std::to_string(v));
}

void CaenDigitizerParam::set_value(double v) {
    set_value(std::to_string(v));
}

void CaenDigitizerParam::set_value(bool v) {
    static const std::string TRUE("true");
    static const std::string FALSE("false");
    set_value(v ? TRUE : FALSE);
}

void CaenDigitizerParam::send_command() {
    parent_->send_command(path_);
}

CaenDigitizer::ParameterWriter::ParameterWriter(const std::string & name, epicsMutex *lock)
: name(name), running(false), lock(lock), handle(NO_HANDLE), pending_writes(MAX_PENDING_WRITES, MAX_PENDING_WRITE_LEN)
{}

CaenDigitizer::ParameterWriter::~ParameterWriter() {}

void CaenDigitizer::ParameterWriter::run() {
    while (running) {
        PendingWrite pw = {};
        if (pending_writes.receive(&pw, sizeof(pw), WAIT_FOR_WRITE_SECS) > 0) {
            // Drop all writes while we're not connected
            if (handle == NO_HANDLE)
                continue;

            switch (pw.type) {
                case PendingWrite::Type::Param: {
                    int ec = CAEN_FELib_SetValue(handle, pw.path, pw.value);
                    if (ec < 0) {
                        errlogPrintf(ERL_ERROR " %s: Failed to set value for path '%s'\n", name.c_str(), pw.path);
                    }
                    break;
                }
                case PendingWrite::Type::Command: {
                    int ec = CAEN_FELib_SendCommand(handle, pw.path);
                    if (ec < 0) {
                        // TODO: better error handling
                        errlogPrintf(ERL_ERROR " %s: Failed to send command '%s'\n", name.c_str(), pw.path);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

CaenDigitizer::DataReader::DataReader(const std::string & name, epicsMutex *lock)
: name(name), running(false), lock(lock), handle(NO_HANDLE)
{}

CaenDigitizer::DataReader::~DataReader() {}

void CaenDigitizer::DataReader::run() {
    while(running) {
        if (handle == NO_HANDLE) {
            epicsThreadSleep(WAIT_FOR_DATA_MSECS / 1000.0);
            continue;
        }

        // Note: this data follows the data format as set by
        // CAEN_FELib_SetReadDataFormat
        size_t max_samples = 1024*1024;
        uint64_t timestamp;
        double timestamp_us;
        uint32_t trigger_id;
        size_t event_size;
        size_t n_channels = 1;
        size_t *n_samples = (size_t*)malloc(
            n_channels*sizeof(*n_samples)
        );
        size_t *n_allocated_samples = (size_t*)malloc(
            n_channels*sizeof(*n_allocated_samples)
        );
        uint16_t **waveform = (uint16_t **)malloc(
            n_channels*sizeof(*waveform)
        );
        for (size_t i = 0; i < n_channels; ++i) {
            n_allocated_samples[i] = max_samples;
            waveform[i] = (uint16_t*)malloc(
                n_allocated_samples[i]*sizeof(*waveform[i])
            );
        }

        printf(
            "timestamp=%p "
            "trigger_id=%p "
            "waveform=%p "
            "n_samples=%p "
            "event_size=%p\n",
            &timestamp, &trigger_id,
            waveform, n_samples, &event_size
        );

        uint64_t ep_handle = NO_HANDLE;
        int ec = CAEN_FELib_GetHandle(handle, "/endpoint/scope", &ep_handle);

        ec = CAEN_FELib_HasData(ep_handle, WAIT_FOR_DATA_MSECS);
        switch (ec) {
            case CAEN_FELib_Success: printf("HAS DATA\n"); break;
            case CAEN_FELib_Timeout: printf("NO DATA\n"); continue;
            default: throw std::logic_error("TODO: FIXME");
        }

        ec = CAEN_FELib_ReadData(ep_handle, WAIT_FOR_DATA_MSECS,
            &timestamp,
            &trigger_id,
            waveform,
            n_samples,
            &event_size
        );

        switch (ec) {
            case CAEN_FELib_Success: printf("GOT DATA\n"); break;
            case CAEN_FELib_Timeout: printf("TIMEOUT\n"); break;
            case CAEN_FELib_Stop: printf("GOT STOP\n"); break;
            default: {
                //printf("Got error while trying to get data\n");
                char err_name[512];
                char err_desc[512];
                CAEN_FELib_GetErrorName(static_cast<CAEN_FELib_ErrorCode>(ec), err_name);
                CAEN_FELib_GetErrorDescription(static_cast<CAEN_FELib_ErrorCode>(ec), err_desc);
                char exc_desc[2048];
                printf("%d %s: %s. %s\n", ec, "Got error on read data", err_name, err_desc);
                //throw_if_err(ec, "Got error while trying to get data");
            }

        }

        //epicsThreadSleep(5.0);
    }
}

CaenDigitizer::CaenDigitizer(const std::string & name, const std::string & addr)
: name_(name), addr_(addr), device_tree_buffer_len_(4*1024*1024),
  device_tree_buffer_(new char[device_tree_buffer_len_]),
  running_(false),
  lock_(),
  parameter_writer_(name, &lock_),
  data_reader_(name, &lock_),
  worker_thread_(
      *this, "DT274X_Worker",
      epicsThreadGetStackSize(epicsThreadStackMedium),
      epicsThreadPriorityMedium
  ),
  parameter_writer_thread_(
      parameter_writer_, "DT274X_ParamWriter",
      epicsThreadGetStackSize(epicsThreadStackMedium),
      epicsThreadPriorityMedium
  ),
  data_reader_thread_(
      data_reader_, "DT274X_DataReader",
      epicsThreadGetStackSize(epicsThreadStackMedium),
      epicsThreadPriorityMedium
  )
{
    scanIoInit(&status_update);
    scanIoInit(&data_update);
    scanIoInit(&error_update);
}

CaenDigitizer::~CaenDigitizer() {
    delete[] device_tree_buffer_;
}

void CaenDigitizer::start() {
    if (!running_) {
        running_ = true;

        parameter_writer_.running = true;
        data_reader_.running = true;

        parameter_writer_thread_.start();
        data_reader_thread_.start();

        worker_thread_.start();
    }
}

static void wait_for_thread(const std::string & prefix, epicsThread *t, const char *n, epicsTime started_waiting_at) {
    double wait_for_secs = WAIT_FOR_THEADS_SECS - (epicsTime::getCurrent() - started_waiting_at);
    if (!t->exitWait(wait_for_secs))
        errlogPrintf(ERL_ERROR " %s: Waited for %.1f sec, but '%s' hasn't stopped...\n", prefix.c_str(), wait_for_secs, n);
}

void CaenDigitizer::stop() {
    if (running_) {
        running_ = false;
        parameter_writer_.running = false;
        data_reader_.running = false;

        epicsTime start_wait_time = epicsTime::getCurrent();
        wait_for_thread(name_, &worker_thread_,           "worker",           start_wait_time);
        wait_for_thread(name_, &parameter_writer_thread_, "parameter writer", start_wait_time);
        wait_for_thread(name_, &data_reader_thread_,      "data reader",      start_wait_time);
    }
}

void CaenDigitizer::run() {
    uint64_t handle = NO_HANDLE;
    while (running_) {
        // Clear all parameters
        for (auto it = params_.begin(); it != params_.end(); ++it)
            it->second->reset();

        int ec = CAEN_FELib_Open(addr_.c_str(), &handle);
        if (ec < 0) {
            char err_name[512];
            char err_desc[512];
            CAEN_FELib_GetErrorName(static_cast<CAEN_FELib_ErrorCode>(ec), err_name);
            CAEN_FELib_GetErrorDescription(static_cast<CAEN_FELib_ErrorCode>(ec), err_desc);
            errlogPrintf(ERL_ERROR " %s: Failed to open device: %s: %s\n", name_.c_str(), err_name, err_desc);
            epicsThreadSleep(1.0);
            continue;
        }

        // Set the current handle on the sub-threads
        parameter_writer_.handle = handle;
        data_reader_.handle = handle;

        try {
            run_with(handle);
        } catch (std::exception & ex) {
            errlogPrintf(ERL_ERROR " %s: Got exception while running: %s\n", name_.c_str(), ex.what());

            // Clear handle on sub-threads
            parameter_writer_.handle = NO_HANDLE;
            data_reader_.handle = NO_HANDLE;

            CAEN_FELib_Close(handle);

            // TODO: exponential back off
            epicsThreadSleep(1.0);
        }
    }

    // Cleanup
    if (handle != NO_HANDLE)
        CAEN_FELib_Close(handle);
}

// Ensure that the device is in 'scope' mode and configure data format
void CaenDigitizer::prepare_scope(uint64_t handle) {
    int ec = CAEN_FELib_SetValue(handle, ACTIVE_ENDPOINT.c_str(), SCOPE.c_str());
    throw_if_err(ec, "Failed to set device to 'scope' mode");

    ec = CAEN_FELib_SetReadDataFormat(handle, DATA_FORMAT.c_str());
    throw_if_err(ec, "Failed to set data format");
}

// Read the device tree from the device, parse it as JSON
// Then, for each registered parameter (from EPICS side)
// Find their corresponding value in the device tree
// and set that as the record value
void CaenDigitizer::fetch_all_params(uint64_t handle) {
    for (;;) {
        int ec = CAEN_FELib_GetDeviceTree(handle, device_tree_buffer_, device_tree_buffer_len_);
        throw_if_err(ec, "Failed to Get Device Tree");

        if (static_cast<size_t>(ec) > device_tree_buffer_len_) {
            // We don't have enough space to read the device tree,
            // resize the buffer and try again
            delete[] device_tree_buffer_;
            device_tree_buffer_len_ *= 2;
            device_tree_buffer_ = new char[device_tree_buffer_len_];
            continue;
        }
        break;
    }

    // Parse device tree contents into JSON
    auto device_tree = json::parse(device_tree_buffer_);

    // For every registered parameter, search for its corresponding value in the device tree contents
    for (auto it = params_.begin(); it != params_.end(); ++it) {
        const std::string & path = it->first;
        CaenDigitizerParam *param = it->second;

        // If the parameter is a command, skip trying to get its value (it makes no sense for a command to have a value)
        if (param->type_ == CaenDigitizerParam::Type::Command)
            continue;

        auto node = device_tree[json::json_pointer(path)];

        if (node == nullptr) {
            errlogPrintf(ERL_ERROR " %s: Failed to find parameter '%s' in device '%s'\n", name_.c_str(), path.c_str(), addr_.c_str());
            continue;
        }

        auto param_handle_node = node["handle"];
        auto param_value_node = node["value"];

        if (param_handle_node == nullptr) {
            errlogPrintf(ERL_ERROR " %s: Failed to get handle for parameter '%s'\n", name_.c_str(), path.c_str());
            continue;
        }

        if (param_value_node == nullptr) {
            errlogPrintf(ERL_ERROR " %s: Failed to get value for parameter '%s'\n", name_.c_str(), path.c_str());
            continue;
        }

        uint64_t param_handle = NO_HANDLE;
        std::string param_value;

        try {
            param_handle = param_handle_node.template get<uint64_t>();
        } catch (json::exception & ex) {
            // TODO: improve error message
            errlogPrintf(ERL_ERROR " %s: Failed to get handle for parameter '%s'\n", name_.c_str(), path.c_str());
            continue;
        }

        try {
            param_value = param_value_node.template get<std::string>();
        } catch (json::exception & ex) {
            errlogPrintf(ERL_ERROR " %s: Failed to get value for parameter '%s'\n", name_.c_str(), path.c_str());
            continue;
        }

        param->set(param_handle, param_value);
        //printf("P[%s] (%lu) = %s\n", path.c_str(), param->handle_, param->value_.c_str());
    }
}

void CaenDigitizer::write_parameter(const std::string & path, const std::string & value) {
    const std::string lpath(str_tolower(path));
    auto pw = PendingWrite {};

    pw.type = PendingWrite::Type::Param;
    strncpy(pw.path, lpath.c_str(), sizeof(pw.path)-1);
    strncpy(pw.value, value.c_str(), sizeof(pw.value)-1);

    if (parameter_writer_.pending_writes.trySend(&pw, sizeof(pw)) < 0) {
        // TODO: better error
        throw std::runtime_error("Failed to enqueue write: queue is full");
    }
}

void CaenDigitizer::send_command(const std::string & path) {
    const std::string lpath(str_tolower(path));
    auto pw = PendingWrite {};

    pw.type = PendingWrite::Type::Command;
    strncpy(pw.path, lpath.c_str(), sizeof(pw.path)-1);

    if (parameter_writer_.pending_writes.trySend(&pw, sizeof(pw)) < 0) {
        // TODO: better error
        throw std::runtime_error("Failed to enqueue command: queue is full");
    }
}

void CaenDigitizer::run_with(uint64_t handle) {
    // Force scope mode

    while (running_) {
        //epicsTime start = epicsTime::getCurrent();
        fetch_all_params(handle);
        scanIoRequest(status_update);
        //double duration = epicsTime::getCurrent() - start;

        //printf("Fetched bytes in %.3f sec\n", duration);

        // TODO: smarter throttle, configurable period
        epicsThreadSleep(1.0);
    }
}

CaenDigitizerParam *CaenDigitizer::get_parameter(const std::string & path) {
    const std::string lpath = str_tolower(path);

    // Check if we already have this parameter. If yes, return it
    auto it = params_.find(lpath);
    if (it != params_.end())
        return it->second;

    // If not, create a new one
    CaenDigitizerParam *param = new CaenDigitizerParam(this, lpath);
    params_[lpath] = param;
    return param;
}
