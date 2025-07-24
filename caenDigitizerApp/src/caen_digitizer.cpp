#include "caen_digitizer.h"

#include <CAEN_FELib.h>
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

CaenDigitizer::CaenDigitizer(const std::string & name, const std::string & addr)
: _name(name), _addr(addr)
{
    int ec = CAEN_FELib_Open(addr.c_str(), &handle_);
    throw_if_err(
        ec,
        std::string("Failed to open digitizer '") + name + "' @ '" + addr + "'"
    );
}

void CaenDigitizer::destroy() {
    CAEN_FELib_Close(handle_);
}

uint64_t CaenDigitizer::get_handle(const std::string & path) {
    uint64_t handle;
    int ec = CAEN_FELib_GetHandle(handle_, path.c_str(), &handle);
    throw_if_err(ec, std::string("Failed to get handle for path ") + path);
    return handle;
}

std::string CaenDigitizer::get_value(uint64_t handle) {
    char value[256];
    int ec = CAEN_FELib_GetValue(handle, NULL, value);
    throw_if_err(ec, handle, "Failed to get value");
    return std::string(value);
}

int64_t CaenDigitizer::get_int_value(uint64_t handle) {
    return std::stol(get_value(handle));
}

double CaenDigitizer::get_double_value(uint64_t handle) {
    return std::stod(get_value(handle));
}
