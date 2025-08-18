#include "caen_digitizer.h"

#include <CAEN_FELib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <iostream>

#include <nlohmann/json.hpp>

#include <epicsThread.h>
#include <epicsTime.h>

using json = nlohmann::json;

static const size_t NO_HANDLE = -1;

// How many pending wriite messages can there be
static const size_t MAX_PENDING_WRITES = 1024;

// Maximum size of an individual value to be written
static const size_t MAX_PENDING_WRITE_LEN = 2048;

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
: parent_(parent), path_(path), handle_(NO_HANDLE), value_()
{
    reset();
}

IOSCANPVT CaenDigitizerParam::get_status_update() {
    printf("GET STATUS UPDATE %p\n", parent_->status_update);
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
    // TODO: implement async processing
    parent_->send_command(path_);
}

CaenDigitizer::CaenDigitizer(const std::string & name, const std::string & addr)
: name_(name), addr_(addr), device_tree_buffer_len_(4*1024*1024),
  device_tree_buffer_(new char[device_tree_buffer_len_]),
  running_(false), pending_writes_(MAX_PENDING_WRITES, MAX_PENDING_WRITE_LEN),
  worker_thread_(
      *this, "DT274X_Worker",
      epicsThreadGetStackSize(epicsThreadStackMedium),
      epicsThreadPriorityHigh
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
        worker_thread_.start();
    }
}

void CaenDigitizer::stop() {
    static const double WAIT_FOR_SECS = 10.0;
    if (running_) {
        running_ = false;
        if (!worker_thread_.exitWait(WAIT_FOR_SECS)) {
            fprintf(stderr, "Waited for %.1f sec, but worker hasn't stopped...", WAIT_FOR_SECS);
        }
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
            printf("Failed to open device: %s: %s\n", err_name, err_desc);
            epicsThreadSleep(1.0);
            continue;
        }

        try {
            run_with(handle);
        } catch (std::exception & ex) {
            printf("OH NO: %s\n", ex.what());
            CAEN_FELib_Close(handle);
            epicsThreadSleep(1.0);
        }
    }

    // Cleanup
    if (handle != NO_HANDLE)
        CAEN_FELib_Close(handle);
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

        auto node = device_tree[json::json_pointer(path)];

        if (node == nullptr) {
            fprintf(stderr, "Failed to find parameter %s in device %s\n", path.c_str(), addr_.c_str());
            continue;
        }

        auto param_handle_node = node["handle"];
        auto param_value_node = node["value"];

        if (param_handle_node == nullptr) {
            fprintf(stderr, "Failed to get handle for parameter %s\n", path.c_str());
            continue;
        }

        if (param_value_node == nullptr) {
            fprintf(stderr, "Failed to get value for parameter %s\n", path.c_str());
            continue;
        }

        std::cout << path << " " << node << std::endl;

        uint64_t param_handle = NO_HANDLE;
        std::string param_value;

        try {
            param_handle = param_handle_node.template get<uint64_t>();
        } catch (json::exception & ex) {
            // TODO: improve error message
            fprintf(stderr, "Failed to get handle for parameter %s\n", path.c_str());
            continue;
        }

        try {
            param_value = param_value_node.template get<std::string>();
        } catch (json::exception & ex) {
            fprintf(stderr, "Failed to get value for parameter %s\n", path.c_str());
            continue;
        }

        param->set(param_handle, param_value);
        //printf("P[%s] (%lu) = %s\n", path.c_str(), param->handle_, param->value_.c_str());
    }
}

void CaenDigitizer::send_all_pending_requests(uint64_t handle) {
    PendingWrite pw;
    while (pending_writes_.tryReceive(&pw, sizeof(pw)) > 0) {
        printf("GOT PENDING REQUEST '%s' '%s'\n", pw.path, pw.value);
        switch (pw.type) {
        case PendingWrite::Type::Param: {
            int ec = CAEN_FELib_SetValue(handle, pw.path, pw.value);
            if (ec < 0) {
                printf("Faliled to set value for path %s\n", pw.path);
            }
            break;
        }
        case PendingWrite::Type::Command: {
            int ec = CAEN_FELib_SendCommand(handle, pw.path);
            if (ec < 0) {
                // TODO: better error handling
                printf("Faliled to send command %s\n", pw.path);
            }
            break;
        }
        default:
            break;
        }
        pw = {};
    }
}

void CaenDigitizer::write_parameter(const std::string & path, const std::string & value) {
    const std::string lpath(str_tolower(path));
    auto pw = PendingWrite {};

    pw.type = PendingWrite::Type::Param;
    strncpy(pw.path, lpath.c_str(), sizeof(pw.path)-1);
    strncpy(pw.value, value.c_str(), sizeof(pw.value)-1);

    if (pending_writes_.trySend(&pw, sizeof(pw)) < 0) {
        // TODO: better error
        throw std::runtime_error("Failed to enqueue write: queue is full");
    }
}

void CaenDigitizer::send_command(const std::string & path) {
    const std::string lpath(str_tolower(path));
    auto pw = PendingWrite {};

    pw.type = PendingWrite::Type::Command;
    strncpy(pw.path, lpath.c_str(), sizeof(pw.path)-1);

    if (pending_writes_.trySend(&pw, sizeof(pw)) < 0) {
        // TODO: better error
        throw std::runtime_error("Failed to enqueue command: queue is full");
    }
}

void CaenDigitizer::run_with(uint64_t handle) {
    while (running_) {
        epicsTime start = epicsTime::getCurrent();
        fetch_all_params(handle);
        scanIoRequest(status_update);
        double duration = epicsTime::getCurrent() - start;

        send_all_pending_requests(handle);

        printf("Fetched bytes in %.3f sec\n", duration);

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
