#include <stdint.h>

#include <epicsExit.h>
#include <errlog.h>

#include <string>

#include <CAEN_FELib.h>

const std::string DATA_FORMAT = std::string("\
       [ \
           { \"name\" : \"TIMESTAMP\", \"type\" : \"U64\" }, \
           { \"name\" : \"TRIGGER_ID\", \"type\" : \"U32\" }, \
           { \"name\" : \"WAVEFORM\", \"type\" : \"U16\", \"dim\" : 2 }, \
           { \"name\" : \"WAVEFORM_SIZE\", \"type\" : \"SIZE_T\", \"dim\" : 1 }, \
           { \"name\" : \"EVENT_SIZE\", \"type\" : \"SIZE_T\" } \
       ] \
   ");

struct Event {
    size_t n_channels;
    uint64_t timestamp;
    uint32_t trigger_id;
    uint16_t **waveform;
    size_t *n_samples;
    size_t event_size;

    Event(size_t n_channels, size_t max_samples)
    : n_channels(n_channels), timestamp(0), trigger_id(0),
      waveform(new uint16_t*[n_channels]),
      n_samples(new size_t[n_channels]),
      event_size(0)
    {
        for (size_t ch = 0; ch < n_channels; ++ch) {
            n_samples[ch] = max_samples;
            waveform[ch] = new uint16_t[max_samples];
        }
    }

    ~Event() {
        delete[] n_samples;
        for (size_t ch = 0; ch < n_channels; ++ch)
            delete[] waveform[ch];
        delete[] waveform;
    }
};

void usage_and_exit(const char *name);
void check(int result, const char *err_message);

static uint64_t handle = 0;

int main(int argc, const char **argv) {
    if (argc != 2)
        usage_and_exit(argv[0]);

    const char *addr = argv[1];

    // Open device
    check(CAEN_FELib_Open(addr, &handle), "Failed to open device");

    // Get number of channels
    size_t num_ch = 0;
    {
        char value[256];
        check(CAEN_FELib_GetValue(handle, "/par/NumCh", value), "Failed to get number of channels");
        num_ch = std::stoul(value);
    }

    // Reset device
    check(CAEN_FELib_SendCommand(handle, "/cmd/reset"), "Failed to reset device");

    // Disable all channels
    {
        char par_name[256];
        snprintf(par_name, sizeof(par_name), "/ch/0..%zu/par/ChEnable", num_ch - 1);
        check(CAEN_FELib_SetValue(handle, par_name, "false"), "Failed to disable all channels");
    }
    printf("Number of channels: %lu\n", num_ch);

    // Enable first channel
    check(CAEN_FELib_SetValue(handle, "/ch/0/par/ChEnable", "true"), "Failed to enable first channel");

    // Configure acquisition
    check(CAEN_FELib_SetValue(handle, "/par/RecordLengthS", "1024"), "Failed to set Record Length");
    check(CAEN_FELib_SetValue(handle, "/par/PreTriggerS", "100"), "Failed to set Pre-Trigger Length");
    check(CAEN_FELib_SetValue(handle, "/par/AcqTriggerSource", "SwTrg | TestPulse"), "Failed to set Trig Source");
    check(CAEN_FELib_SetValue(handle, "/par/TestPulsePeriod", "100000000"), "Failed to set Test Pulse Period");
    check(CAEN_FELib_SetValue(handle, "/par/TestPulseWidth", "1000"), "Failed to set Test Pulse Width");
    check(CAEN_FELib_SetValue(handle, "/ch/0/par/DCOffset", "50"), "Failed to enable first channel's DC Offset");

    // Stop any ongoing acquisition
    check(CAEN_FELib_SendCommand(handle, "/cmd/SWStopAcquisition"), "Failed to stop acquisition");
    check(CAEN_FELib_SendCommand(handle, "/cmd/DisarmAcquisition"), "Failed to disarm acquisition");

    // Prepare endpoint
    uint64_t ep_handle = 0;
    check(CAEN_FELib_GetHandle(handle, "/endpoint/scope", &ep_handle), "Failed to get endpoint handle");
    check(CAEN_FELib_SetValue(handle, "/endpoint/par/ActiveEndpoint", "scope"), "Failed to set active endpoint");
    check(CAEN_FELib_SetReadDataFormat(ep_handle, DATA_FORMAT.c_str()), "Failed to set data format");

    // Start acquisition
    check(CAEN_FELib_SendCommand(handle, "/cmd/ArmAcquisition"), "Failed to arm acquisition");
    check(CAEN_FELib_SendCommand(handle, "/cmd/SWStartAcquisition"), "Failed to start acquisition");

    for (size_t i = 0; i < 5; ++i) {

        check(CAEN_FELib_SendCommand(handle, "/cmd/sendswtrigger"), "Failed to send SW Trigger");

        struct Event *evt = new Event(num_ch, 1024*1024);

        check(
            CAEN_FELib_ReadData(
                ep_handle,
                -1,                 // Wait forever
                &evt->timestamp,
                &evt->trigger_id,
                evt->waveform,
                evt->n_samples,
                &evt->event_size
            ),
            "Failed to read data"
        );
        printf("Got event:\n");
        printf("    timestamp = %lu\n", evt->timestamp);
        printf("    trigger_id = %u\n", evt->trigger_id);
        printf("    event_size = %lu\n", evt->event_size);
        printf("    n_channels = %lu\n", evt->n_channels);
        printf("    n_samples = [\n        ");
        for (size_t ch = 0; ch < evt->n_channels; ++ch) {
            if (ch && ch % 8 == 0)
                printf("\n        ");
            printf("%4lu ", evt->n_samples[ch]);
        }
        printf("\n    ]\n");
        printf("    data[0..16] = [");
        for (size_t p = 0; p < 16; ++p) {
            printf("%5d ", evt->waveform[0][p]);
        }
        printf("]\n");

        delete evt;
    }

    CAEN_FELib_Close(handle);
    printf("Done\n");
}

void usage_and_exit(const char *name) {
    epicsPrintf("Usage: %s <device url>\n", name);
    epicsPrintf("Example: %s dig2://127.0.0.1", name);
    epicsExit(-1);
}

void check(int result, const char *err_message) {
    CAEN_FELib_ErrorCode caen_result = static_cast<CAEN_FELib_ErrorCode>(result);
    if (caen_result == CAEN_FELib_Success)
        return;

    char err_name[512];
    char err_desc[512];
    CAEN_FELib_GetErrorName(caen_result, err_name);
    CAEN_FELib_GetErrorDescription(caen_result, err_desc);

    errlogSevPrintf(errlogMajor, "%s: %s -- %s\n", err_message, err_name, err_desc);

    if (handle != 0)
        CAEN_FELib_Close(handle);

    epicsExit(-1);
}
