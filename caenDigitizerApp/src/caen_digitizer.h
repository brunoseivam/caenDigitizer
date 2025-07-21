#pragma once

#include <cstdint>
#include <string>

class CaenDigitizer {
    std::string _name;
    std::string _addr;

    uint64_t handle_;

public:
    CaenDigitizer(const std::string & name, const std::string & addr);
    void destroy();
};
