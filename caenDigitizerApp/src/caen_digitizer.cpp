#include "caen_digitizer.h"
#include "dbScan.h"
#include "devSup.h"

#include <CAEN_FELib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>
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

// Maximum amount of pending task commands
static const size_t MAX_PENDING_TASK_COMMANDS = 2;

// How many pending write messages can there be
static const size_t MAX_PENDING_WRITES = 1024;

// How many pending events can there be
static const size_t MAX_PENDING_EVENTS = 32;

// Maximum number of samples per waveform
static const size_t MAX_NUM_SAMPLES = 1024*1024;

// Maximum time to wait for a write parameter command
static const double WAIT_FOR_WRITE_SECS = 1.0f;

// Maximum time to wait for data
static const int WAIT_FOR_DATA_MSECS = 100;

// Maximum time to wait for all threads to join (when exiting)
static const double WAIT_FOR_THREADS_SECS = 5.0f;

struct TaskCommand {
    enum TaskCommandType {
        TASK_START,
        TASK_STOP,
    } type;
    struct {
        uint64_t handle;
        uint64_t ep_handle;
        size_t num_channels;
    } data;

    static TaskCommand create_start(uint64_t handle, uint64_t ep_handle, size_t num_channels) {
        return TaskCommand {
            .type = TASK_START,
            .data = {
                .handle = handle,
                .ep_handle = ep_handle,
                .num_channels = num_channels
            }
        };
    }

    static TaskCommand create_stop() {
        return TaskCommand {
            .type = TASK_STOP,
            .data = {}
        };
    }
};

// Stores a pending write parameter message, or pending send command message
struct PendingWrite {
    enum Type { Param, Command } type;
    std::string path;
    std::string value;

    PendingWrite(const std::string & path)
    : type(Type::Command), path(path), value()
    {}

    PendingWrite(const std::string & path, const std::string & value)
    : type(Type::Param), path(path), value(value)
    {}

    ~PendingWrite() {}
};

// Stores an acquisition event from the scope
struct Event {
    static const std::string DATA_FORMAT;
    size_t n_channels;
    uint64_t timestamp;
    uint32_t trigger_id;
    uint16_t **waveform;
    size_t *n_samples;
    size_t event_size;

    Event(size_t n_channels, size_t max_samples)
    : n_channels(n_channels), timestamp(0), trigger_id(0),
      waveform(new uint16_t*[n_channels]),
      n_samples(new size_t[n_channels]),
      event_size(0)
    {
        for (size_t ch = 0; ch < n_channels; ++ch) {
            n_samples[ch] = max_samples;
            waveform[ch] = new uint16_t[max_samples];
        }
    }

    ~Event() {
        delete[] n_samples;
        for (size_t ch = 0; ch < n_channels; ++ch)
            delete[] waveform[ch];
        delete[] waveform;
    }
};

const std::string Event::DATA_FORMAT = std::string("\
       [ \
           { \"name\" : \"TIMESTAMP\", \"type\" : \"U64\" }, \
           { \"name\" : \"TRIGGER_ID\", \"type\" : \"U32\" }, \
           { \"name\" : \"WAVEFORM\", \"type\" : \"U16\", \"dim\" : 2 }, \
           { \"name\" : \"WAVEFORM_SIZE\", \"type\" : \"SIZE_T\", \"dim\" : 1 }, \
           { \"name\" : \"EVENT_SIZE\", \"type\" : \"SIZE_T\" } \
       ] \
   ");

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

CaenDigitizer::ParameterReader::ParameterReader(const std::string & name, IOSCANPVT *scan, ParameterMap *params)
: name(name), running(false), scan(scan), params(params), device_tree_buffer_len(4*1024*1024),
  device_tree_buffer(new char[device_tree_buffer_len]),
  task_commands(MAX_PENDING_TASK_COMMANDS, sizeof(struct TaskCommand))
{}

CaenDigitizer::ParameterReader::~ParameterReader() {
    delete[] device_tree_buffer;
}

void CaenDigitizer::ParameterReader::run() {
    uint64_t handle = NO_HANDLE;

    while (running) {
        TaskCommand tc;
        if (task_commands.tryReceive(&tc, sizeof(tc)) > 0) {
            switch (tc.type) {
                case TaskCommand::TASK_START:
                    handle = tc.data.handle;
                    break;

                case TaskCommand::TASK_STOP:
                    handle = NO_HANDLE;
                    break;
            }
        }

        if (handle == NO_HANDLE) {
            epicsThreadSleep(0.1);
            continue;
        }

        try {
            fetch_all_params(handle);
            scanIoRequest(*scan);
        } catch (std::exception & ex) {
            errlogPrintf(ERL_ERROR " %s: Got exception while running: %s\n", name.c_str(), ex.what());
        }

        epicsThreadSleep(0.5);
    }
}

// Read the device tree from the device, parse it as JSON
// Then, for each registered parameter (from EPICS side)
// Find their corresponding value in the device tree
// and set that as the record value
void CaenDigitizer::ParameterReader::fetch_all_params(uint64_t handle) {
    for (;;) {
        int ec = CAEN_FELib_GetDeviceTree(handle, device_tree_buffer, device_tree_buffer_len);
        throw_if_err(ec, "Failed to Get Device Tree");

        if (static_cast<size_t>(ec) > device_tree_buffer_len) {
            // We don't have enough space to read the device tree,
            // resize the buffer and try again
            delete[] device_tree_buffer;
            device_tree_buffer_len *= 2;
            device_tree_buffer = new char[device_tree_buffer_len];
            continue;
        }
        break;
    }

    // Parse device tree contents into JSON
    auto device_tree = json::parse(device_tree_buffer);

    // For every registered parameter, search for its corresponding value in the device tree contents
    for (auto it = params->begin(); it != params->end(); ++it) {
        const std::string & path = it->first;
        CaenDigitizerParam *param = it->second;

        // If the parameter is a command, skip trying to get its value (it makes no sense for a command to have a value)
        if (param->type_ == CaenDigitizerParam::Type::Command)
            continue;

        auto node = device_tree[json::json_pointer(path)];

        if (node == nullptr) {
            errlogPrintf(ERL_ERROR " %s: Failed to find parameter '%s'\n", name.c_str(), path.c_str());
            continue;
        }

        auto param_handle_node = node["handle"];
        auto param_value_node = node["value"];

        if (param_handle_node == nullptr) {
            errlogPrintf(ERL_ERROR " %s: Failed to get handle for parameter '%s'\n", name.c_str(), path.c_str());
            continue;
        }

        if (param_value_node == nullptr) {
            errlogPrintf(ERL_ERROR " %s: Failed to get value for parameter '%s'\n", name.c_str(), path.c_str());
            continue;
        }

        uint64_t param_handle = NO_HANDLE;
        std::string param_value;

        try {
            param_handle = param_handle_node.template get<uint64_t>();
        } catch (json::exception & ex) {
            // TODO: improve error message
            errlogPrintf(ERL_ERROR " %s: Failed to get handle for parameter '%s'\n", name.c_str(), path.c_str());
            continue;
        }

        try {
            param_value = param_value_node.template get<std::string>();
        } catch (json::exception & ex) {
            errlogPrintf(ERL_ERROR " %s: Failed to get value for parameter '%s'\n", name.c_str(), path.c_str());
            continue;
        }

        param->set(param_handle, param_value);
        //printf("P[%s] (%lu) = %s\n", path.c_str(), param->handle_, param->value_.c_str());
    }
}

CaenDigitizer::ParameterWriter::ParameterWriter(const std::string & name)
: name(name), running(false), task_commands(MAX_PENDING_TASK_COMMANDS, sizeof(struct TaskCommand)),
  pending_writes(MAX_PENDING_WRITES, sizeof(PendingWrite*))
{}

CaenDigitizer::ParameterWriter::~ParameterWriter() {}

void CaenDigitizer::ParameterWriter::run() {
    size_t num_starts = 0;
    uint64_t handle = NO_HANDLE;

    while (running) {
        TaskCommand tc;
        if (task_commands.tryReceive(&tc, sizeof(tc)) > 0) {
            switch (tc.type) {
                case TaskCommand::TASK_START:
                    handle = tc.data.handle;
                    ++num_starts;
                    break;

                case TaskCommand::TASK_STOP:
                    handle = NO_HANDLE;
                    break;
            }
        }

        // If we never connected, keep waiting (so we don't drop writes near IOC init)
        if (num_starts == 0 && handle == NO_HANDLE) {
            epicsThreadSleep(0.1);
            continue;
        }

        PendingWrite *pw = NULL;
        if (pending_writes.receive(&pw, sizeof(pw), WAIT_FOR_WRITE_SECS) > 0) {
            // Copy to local vars so that pw can be deallocated early
            enum PendingWrite::Type type = pw->type;
            std::string path = pw->path;
            std::string value = pw->value;
            delete pw;
            pw = NULL;

            // Drop all writes while we're not connected
            if (handle == NO_HANDLE)
                continue;

            switch (type) {
                case PendingWrite::Type::Param: {
                    int ec = CAEN_FELib_SetValue(handle, path.c_str(), value.c_str());
                    printf("SET [%d] %s '%s'\n", ec, path.c_str(), value.c_str());
                    if (ec < 0) {
                        errlogPrintf(ERL_ERROR " %s: Failed to set value for path '%s'\n", name.c_str(), path.c_str());
                    }
                    break;
                }
                case PendingWrite::Type::Command: {
                    int ec = CAEN_FELib_SendCommand(handle, path.c_str());
                    printf("CMD [%d] %s\n", ec, path.c_str());
                    if (ec < 0) {
                        // TODO: better error handling
                        errlogPrintf(ERL_ERROR " %s: Failed to send command '%s'\n", name.c_str(), path.c_str());
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

CaenDigitizer::DataReader::DataReader(const std::string & name, IOSCANPVT *scan)
: name(name), running(false), scan(scan), task_commands(MAX_PENDING_TASK_COMMANDS, sizeof(struct TaskCommand)),
  pending_events(MAX_PENDING_EVENTS, sizeof(Event *))
{}

CaenDigitizer::DataReader::~DataReader() {}

void CaenDigitizer::DataReader::run() {
    uint64_t ep_handle = NO_HANDLE;
    size_t num_channels = 0;

    while (running) {
        TaskCommand tc;
        if (task_commands.tryReceive(&tc, sizeof(tc)) > 0) {
            switch (tc.type) {
                case TaskCommand::TASK_START:
                    ep_handle = tc.data.ep_handle;
                    num_channels = tc.data.num_channels;
                    break;

                case TaskCommand::TASK_STOP:
                    ep_handle = NO_HANDLE;
                    num_channels = 0;
                    break;
            }
        }

        if (ep_handle == NO_HANDLE) {
            epicsThreadSleep(0.1);
            continue;
        }

        try {
            struct Event *event = read_data(ep_handle, num_channels, WAIT_FOR_DATA_MSECS);
            if (event) {
                if (pending_events.trySend(&event, sizeof(event)) < 0) {
                    delete event;
                    errlogPrintf(ERL_ERROR " %s::run(): Failed to enqueue new event (queue is full)\n", name.c_str());
                }
            }
        } catch (std::exception & ex) {
            errlogPrintf(ERL_ERROR " %s::run(): Got exception while reading data: %s\n", name.c_str(), ex.what());
        }
    }
}

struct Event *CaenDigitizer::DataReader::read_data(uint64_t ep_handle, size_t num_channels, double wait_for_msec) {
    struct Event *event = new Event(num_channels, MAX_NUM_SAMPLES);

    int ec = CAEN_FELib_ReadData(ep_handle, wait_for_msec,
        &event->timestamp,
        &event->trigger_id,
        event->waveform,
        event->n_samples,
        &event->event_size
    );

    if (ec == CAEN_FELib_Success)
        return event;

    delete event;

    switch (ec) {
        case CAEN_FELib_Timeout: /*printf("TIMEOUT\n");*/ break;
        case CAEN_FELib_Stop: printf("GOT STOP\n"); break;
        default: throw_if_err(ec, "Failed to read data from scope"); break;
    }

    return NULL;
}
CaenDigitizer::CaenDigitizer(const std::string & name, const std::string & addr)
: name_(name), addr_(addr), running_(false),
  parameter_reader_(name + "::ParameterReader", &status_update, &params_),
  parameter_writer_(name + "::ParameterWriter"),
  data_reader_(name + "::DataReader", &data_update),
  worker_thread_(
      *this, "DT274X_Worker",
      epicsThreadGetStackSize(epicsThreadStackMedium),
      epicsThreadPriorityMedium
  ),
  parameter_reader_thread_(
      parameter_reader_, "DT274X_ParamReader",
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
}

void CaenDigitizer::start() {
    if (!running_) {
        running_ = true;

        parameter_reader_.running = true;
        parameter_writer_.running = true;
        data_reader_.running = true;

        parameter_reader_thread_.start();
        parameter_writer_thread_.start();
        data_reader_thread_.start();

        worker_thread_.start();
    }
}

static void wait_for_thread(const std::string & prefix, epicsThread *t, const char *n, epicsTime started_waiting_at) {
    double wait_for_secs = WAIT_FOR_THREADS_SECS - (epicsTime::getCurrent() - started_waiting_at);
    if (!t->exitWait(wait_for_secs))
        errlogPrintf(ERL_ERROR " %s: Waited for %.1f sec, but '%s' hasn't stopped...\n", prefix.c_str(), wait_for_secs, n);
}

void CaenDigitizer::stop() {
    if (running_) {
        running_ = false;
        parameter_reader_.running = false;
        parameter_writer_.running = false;
        data_reader_.running = false;

        epicsTime start_wait_time = epicsTime::getCurrent();
        wait_for_thread(name_, &worker_thread_,           "worker",       start_wait_time);
        wait_for_thread(name_, &parameter_reader_thread_, "param reader", start_wait_time);
        wait_for_thread(name_, &parameter_writer_thread_, "param writer", start_wait_time);
        wait_for_thread(name_, &data_reader_thread_,      "data reader",  start_wait_time);
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

        try {
            run_with(handle);
        } catch (std::exception & ex) {
            errlogPrintf(ERL_ERROR " %s: Got exception while running: %s\n", name_.c_str(), ex.what());

            TaskCommand stop = TaskCommand::create_stop();
            parameter_reader_.task_commands.send(&stop, sizeof(stop));
            parameter_writer_.task_commands.send(&stop, sizeof(stop));
            data_reader_.task_commands.send(&stop, sizeof(stop));

            CAEN_FELib_Close(handle);

            // TODO: exponential back off
            epicsThreadSleep(1.0);
        }
    }

    // Cleanup
    if (handle != NO_HANDLE)
        CAEN_FELib_Close(handle);
}

void CaenDigitizer::prepare_scope(uint64_t handle, uint64_t *ep_handle, size_t *num_channels) {
    // Ensure we are in 'scope' mode
    int ec = CAEN_FELib_SetValue(handle, "/endpoint/par/ActiveEndpoint", "scope");
    throw_if_err(ec, "Failed to set device to 'scope' mode");

    // Get handle to the 'scope' endpoint
    ec = CAEN_FELib_GetHandle(handle, "/endpoint/scope", ep_handle);
    throw_if_err(ec, "Failed to get endpoint handle");

    char value[32];
    ec = CAEN_FELib_GetValue(handle, "/par/NumCh", value);
    throw_if_err(ec, "Failed to get number of channels");
    *num_channels = std::stoul(value);

    // Configure data format
    ec = CAEN_FELib_SetReadDataFormat(*ep_handle, Event::DATA_FORMAT.c_str());
    throw_if_err(ec, "Failed to set data format");

    // Stop any ongoing acquisition
    ec = CAEN_FELib_SendCommand(handle, "/cmd/SWStopAcquisition");
    throw_if_err(ec, "Failed to stop ongoing acquisitions");

    // Disarm it
    ec = CAEN_FELib_SendCommand(handle, "/cmd/DisarmAcquisition");
    throw_if_err(ec, "Failed to disarm scope");
}


void CaenDigitizer::write_parameter(const std::string & path, const std::string & value) {
    const std::string lpath(str_tolower(path));
    PendingWrite *pw = new PendingWrite(lpath, value);

    if (parameter_writer_.pending_writes.trySend(&pw, sizeof(pw)) < 0) {
        // TODO: better error
        throw std::runtime_error("Failed to enqueue write: queue is full");
    }
}

void CaenDigitizer::send_command(const std::string & path) {
    const std::string lpath(str_tolower(path));
    PendingWrite *pw = new PendingWrite(lpath);

    if (parameter_writer_.pending_writes.trySend(&pw, sizeof(pw)) < 0) {
        // TODO: better error
        throw std::runtime_error("Failed to enqueue command: queue is full");
    }
}

void CaenDigitizer::run_with(uint64_t handle) {
    // Force scope mode
    uint64_t ep_handle = NO_HANDLE;
    size_t num_channels = 0;
    prepare_scope(handle, &ep_handle, &num_channels);

    TaskCommand task_start = TaskCommand::create_start(handle, ep_handle, num_channels);
    parameter_reader_.task_commands.send(&task_start, sizeof(task_start));
    parameter_writer_.task_commands.send(&task_start, sizeof(task_start));
    data_reader_.task_commands.send(&task_start, sizeof(task_start));

    while (running_) {
        // Fetch all parameter values from the device
        epicsTime start = epicsTime::getCurrent();

        struct Event *event = NULL;
        while (data_reader_.pending_events.receive(&event, sizeof(event), WAIT_FOR_WRITE_SECS) > 0) {
            printf("timestamp = %lu\n", event->timestamp);
            printf("trigger_id = %u\n", event->trigger_id);
            printf("event_size = %lu\n", event->event_size);
            printf("n_channels = %lu\n", event->n_channels);
            printf("n_samples = [");
            for (size_t ch = 0; ch < event->n_channels; ++ch) {
                printf("%lu ", event->n_samples[ch]);
            }
            printf("]\n");
            delete event;
            event = NULL;
        }

        double duration = epicsTime::getCurrent() - start;
        if (duration < 1.0) {
            epicsThreadSleep(1.0 - duration);
        }

        //printf("Fetched bytes in %.3f sec\n", duration);
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
