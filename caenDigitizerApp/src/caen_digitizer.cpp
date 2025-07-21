#include "caen_digitizer.h"

#include <CAEN_FELib.h>
#include <cstdio>
#include <stdexcept>

CaenDigitizer::CaenDigitizer(const std::string & name, const std::string & addr)
: _name(name), _addr(addr)
{
    int ec = CAEN_FELib_Open(addr.c_str(), &handle_);
    if (ec != CAEN_FELib_Success) {
        char err_name[1024];
        char err_desc[1024];
        CAEN_FELib_GetErrorName(static_cast<CAEN_FELib_ErrorCode>(ec), err_name);
        CAEN_FELib_GetErrorDescription(static_cast<CAEN_FELib_ErrorCode>(ec), err_desc);
        char exc_desc[2048];
        snprintf(
            exc_desc, sizeof(exc_desc),
            "Failed to open digitizer '%s' @ '%s': %s. %s",
            name.c_str(), addr.c_str(), err_name, err_desc
        );
        throw std::runtime_error(exc_desc);
    }
    printf("OK, got here\n");
}

void CaenDigitizer::destroy() {
    printf("Destroying lib\n");
    CAEN_FELib_Close(handle_);
}
