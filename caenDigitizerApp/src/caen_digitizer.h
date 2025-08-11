#pragma once

#include <cstdint>
#include <string>

class CaenDigitizer;

class CaenDigitizerParam {
    CaenDigitizer *parent_;
    std::string path_;

public:
    CaenDigitizerParam(CaenDigitizer* parent, const std::string & path);

    void get_value(std::string & v);
    void get_value(int64_t & v);
    void get_value(double & v);
    void get_value(bool & v);

    void set_value(const std::string & v);
    void set_value(int64_t v);
    void set_value(double v);
    void set_value(bool v);

    void send_command();
};

class CaenDigitizer {
    std::string name_;
    std::string addr_;

    uint64_t handle_;
    bool is_open_;

public:
    CaenDigitizer(const std::string & name, const std::string & addr);
    void destroy();

    uint64_t get_handle(const std::string & path);
    CaenDigitizerParam *get_parameter(const std::string & path);

};
