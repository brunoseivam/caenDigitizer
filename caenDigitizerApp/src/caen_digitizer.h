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
    uint64_t get_handle(const std::string & path);
    std::string get_value(uint64_t handle);
    int64_t get_int_value(uint64_t handle);
    double get_double_value(uint64_t handle);
};
