#include "caen_digitizer.h"

#include <CAEN_FELib.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

static void throw_if_err(int ec, const std::string & msg) {
    if (ec != CAEN_FELib_Success) {
        char err_name[512];
        char err_desc[512];
        CAEN_FELib_GetErrorName(static_cast<CAEN_FELib_ErrorCode>(ec), err_name);
        CAEN_FELib_GetErrorDescription(static_cast<CAEN_FELib_ErrorCode>(ec), err_desc);
        char exc_desc[2048];
        snprintf(exc_desc, sizeof(exc_desc), "%s: %s. %s", msg.c_str(), err_name, err_desc);
        throw std::runtime_error(exc_desc);
    }
}

static void throw_if_err(int ec, uint64_t handle, const std::string & msg) {
    if (ec != CAEN_FELib_Success) {
        char path[256];
        CAEN_FELib_GetPath(handle, path);
        std::string msg_with_path = std::string(path) + ": " + msg;
        throw_if_err(ec, msg_with_path);
    }
}

CaenDigitizerParam::CaenDigitizerParam(CaenDigitizer *parent, const std::string & path)
: parent_(parent), path_(path)
{}

void CaenDigitizerParam::get_value(std::string & v) {
    char value[256];
    uint64_t handle = parent_->get_handle(path_);
    int ec = CAEN_FELib_GetValue(handle, NULL, value);
    throw_if_err(ec, handle, "Failed to get value");
    v = std::string(value);
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
    uint64_t handle = parent_->get_handle(path_);
    int ec = CAEN_FELib_SetValue(handle, NULL, v.c_str());
    throw_if_err(ec, handle, "Failed to set value");
}

void CaenDigitizerParam::set_value(int64_t v) {
    set_value(std::to_string(v));
}

void CaenDigitizerParam::set_value(double v) {
    set_value(std::to_string(v));
}

void CaenDigitizerParam::set_value(bool v) {
    set_value(v ? "true" : "false");
}

void CaenDigitizerParam::send_command() {
    uint64_t handle = parent_->get_handle(path_);
    int ec = CAEN_FELib_SendCommand(handle, NULL);
    throw_if_err(ec, handle, "Failed to send command");
}

CaenDigitizer::CaenDigitizer(const std::string & name, const std::string & addr)
: name_(name), addr_(addr), is_open_(false)
{
    if (CAEN_FELib_Open(addr.c_str(), &handle_) == CAEN_FELib_Success) {
        is_open_ = true;
    }
}

void CaenDigitizer::destroy() {
    CAEN_FELib_Close(handle_);
}

uint64_t CaenDigitizer::get_handle(const std::string & path) {
    if (!is_open_) {
        // TODO: this isn't thread safe. And we need to close it
        int ec = CAEN_FELib_Open(addr_.c_str(), &handle_);
        throw_if_err(ec, std::string("Failed to get handle for device ") + addr_);
        is_open_ = true;
    }

    uint64_t handle;
    int ec = CAEN_FELib_GetHandle(handle_, path.c_str(), &handle);
    throw_if_err(ec, std::string("Failed to get handle for path ") + path);
    return handle;
}

CaenDigitizerParam *CaenDigitizer::get_parameter(const std::string & path) {
    // TODO: manage this memory
    // TODO: keep set of registered params
    return new CaenDigitizerParam(this, path);
}
