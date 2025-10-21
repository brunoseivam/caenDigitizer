#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <functional>

#include <devSup.h>
#include <epicsMutex.h>
#include <epicsThread.h>
#include <epicsMessageQueue.h>
#include <dbScan.h>

class CaenDigitizer;
class CaenDigitizerParam;

typedef std::map<std::string, CaenDigitizerParam *> ParameterMap;

// Acquisition event from the scope
struct Event {
    static const std::string DATA_FORMAT;
    size_t n_channels;
    uint64_t timestamp;
    uint32_t trigger_id;
    uint16_t **waveform;
    size_t *n_samples;
    size_t event_size;

    Event(size_t n_channels, size_t max_samples);
    ~Event();
};

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
    struct ParameterReader : epicsThreadRunable {
        std::string name;
        bool running;
        IOSCANPVT *scan;
        ParameterMap *params;

        size_t device_tree_buffer_len;
        char *device_tree_buffer;

        // Commands to control this task
        epicsMessageQueue task_commands;

        ParameterReader(const std::string & name, IOSCANPVT *scan, ParameterMap *params);
        virtual ~ParameterReader();
        virtual void run();

        void fetch_all_params(uint64_t handle);
    };

    struct ParameterWriter : epicsThreadRunable {
        std::string name;
        bool running;

        // Commands to control this task
        epicsMessageQueue task_commands;

        // Pending writes to send to the device
        epicsMessageQueue pending_writes;

        ParameterWriter(const std::string & name);
        virtual ~ParameterWriter();
        virtual void run();
    };

    struct DataReader : epicsThreadRunable {
        std::string name;
        bool running;
        IOSCANPVT *scan;

        // Pointers to the latest event that is stored in the parent
        Event **latest_event;
        epicsMutex *latest_event_lock;

        // Commands to control this task
        epicsMessageQueue task_commands;

        DataReader(const std::string & name, IOSCANPVT *scan,
            Event **latest_event, epicsMutex *latest_event_mutex);
        virtual ~DataReader();
        virtual void run();

        struct Event *read_data(uint64_t ep_handle, size_t num_channels, double wait_for_msec);
    };

    std::string name_;
    std::string addr_;

    // Flag to control running state of all threads
    bool running_;

    ParameterMap params_;

    Event *latest_event_;
    epicsMutex latest_event_lock_;

    // Worker sub-threads
    ParameterReader parameter_reader_;
    ParameterWriter parameter_writer_;
    DataReader data_reader_;

    // The worker thread is the parent of the other threads.
    epicsThread worker_thread_;
    // The parameter reader that polls the device for all params
    epicsThread parameter_reader_thread_;
    // The command sender sends all pending commands to the device
    epicsThread parameter_writer_thread_;
    // The data reader thread reads pending acquisition data
    epicsThread data_reader_thread_;

    void prepare_scope(uint64_t handle, uint64_t *ep_handle, size_t *num_channels);
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

    IOSCANPVT get_data_update();

    // Run the function f with latest_event_lock locked, passing in
    // latest_event
    void with_latest_event(std::function<void(Event*)> f);
};
