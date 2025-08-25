#pragma once

#include "devSup.h"
#include "epicsMutex.h"
#include <cstdint>
#include <string>
#include <map>

#include <epicsThread.h>
#include <epicsMessageQueue.h>
#include <dbScan.h>

class CaenDigitizer;

class CaenDigitizerParam {
public:
    enum Type {
        Parameter,
        Command,
    };

private:
    CaenDigitizer *parent_;
    std::string path_;
    enum Type type_;

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
    struct ParameterWriter : epicsThreadRunable {
        std::string name;
        bool running;
        epicsMutex *lock;
        uint64_t handle;

        // Pending writes to send to the device
        epicsMessageQueue pending_writes;

        ParameterWriter(const std::string & name, epicsMutex *lock);
        virtual ~ParameterWriter();
        virtual void run();
    };

    struct DataReader : epicsThreadRunable {
        std::string name;
        bool running;
        epicsMutex *lock;
        uint64_t handle;

        DataReader(const std::string & name, epicsMutex *lock);
        virtual ~DataReader();
        virtual void run();
    };

    std::string name_;
    std::string addr_;

    // Buffer to hold the device tree json
    size_t device_tree_buffer_len_;
    char *device_tree_buffer_;

    bool running_;

    std::map<std::string, CaenDigitizerParam*> params_;

    // Mutex for workers
    epicsMutex lock_;

    // Worker sub-threads
    ParameterWriter parameter_writer_;
    DataReader data_reader_;

    // The worker thread is the parent of the other threads.
    // It also polls parameters periodically
    epicsThread worker_thread_;
    // The command sender sends all pending commands to the device
    epicsThread parameter_writer_thread_;
    // The data reader thread reads pending acquisition data
    epicsThread data_reader_thread_;

    void prepare_scope(uint64_t handle);
    void fetch_all_params(uint64_t handle);
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
