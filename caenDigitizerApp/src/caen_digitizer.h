#pragma once

#include "devSup.h"
#include <cstdint>
#include <string>
#include <map>

#include <epicsThread.h>
#include <epicsMessageQueue.h>
#include <dbScan.h>

class CaenDigitizer;

class CaenDigitizerParam {
    CaenDigitizer *parent_;
    std::string path_;

    uint64_t handle_;
    std::string value_;

    // Reset this parameter to its initial, undefined value
    void reset();

    // Set the inner value for this parameter. Called whenever there's
    // an update
    void set(uint64_t handle, const std::string & value);

public:
    CaenDigitizerParam(CaenDigitizer* parent, const std::string & path);

    IOSCANPVT get_status_update();

    // Get the inner value, converting it to the target type
    void get_value(std::string & v);
    void get_value(int64_t & v);
    void get_value(double & v);
    void get_value(bool & v);

    // Enqueue a value update
    void set_value(const std::string & v);
    void set_value(int64_t v);
    void set_value(double v);
    void set_value(bool v);

    // Enqueue a command
    void send_command();

    friend class CaenDigitizer;
};

class CaenDigitizer : public epicsThreadRunable {
    std::string name_;
    std::string addr_;

    // Buffer to hold the device tree json
    size_t device_tree_buffer_len_;
    char *device_tree_buffer_;

    bool running_;

    // Pending writes to send to the device
    epicsMessageQueue pending_writes_;

    std::map<std::string, CaenDigitizerParam*> params_;

    // The worker thread polls the device periodically
    // and sends pending write requests
    epicsThread worker_thread_;

    void fetch_all_params(uint64_t handle);
    void send_all_pending_requests(uint64_t handle);
    void write_parameter(const std::string & path, const std::string & value);
    void send_command(const std::string & path);

    void run_with(uint64_t handle);

    friend class CaenDigitizerParam;

public:
    IOSCANPVT status_update;
    IOSCANPVT data_update;
    IOSCANPVT error_update;

    CaenDigitizer(const std::string & name, const std::string & addr);
    virtual ~CaenDigitizer();

    void start();
    void stop();
    virtual void run();

    CaenDigitizerParam *get_parameter(const std::string & path);

};
