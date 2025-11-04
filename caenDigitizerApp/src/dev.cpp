#include "caen_digitizer.h"
#include "dbAccessDefs.h"
#include "dbCommon.h"
#include "dbLock.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <map>
#include <regex>

#include <iocsh.h>
#include <epicsExit.h>
#include <errlog.h>
#ifndef ERL_ERROR
#  define ERL_ERROR "ERROR"
#endif
#include <dbAccess.h>
#include <dbStaticLib.h>
#include <epicsExport.h>
#include <recGbl.h>
#include <devLib.h>
#include <alarm.h>
#include <initHooks.h>
#include <menuFtype.h>

#include <stringinRecord.h>
#include <stringoutRecord.h>
#include <longinRecord.h>
#include <longoutRecord.h>
#include <int64inRecord.h>
#include <int64outRecord.h>
#include <aiRecord.h>
#include <aoRecord.h>
#include <biRecord.h>
#include <boRecord.h>
#include <mbbiRecord.h>
#include <mbboRecord.h>
#include <mbbiDirectRecord.h>
#include <waveformRecord.h>

typedef std::map<std::string, CaenDigitizer*> dev_map_t;
static dev_map_t dev_map;

struct ParamPvt {
    CaenDigitizerParam *param;
    bool pending_update;

    ParamPvt(CaenDigitizerParam *param)
    : param(param), pending_update(false)
    {}
};

struct ChannelPvt {
    CaenDigitizer *dev;
    size_t chan;

    ChannelPvt(CaenDigitizer *dev, size_t chan)
    : dev(dev), chan(chan)
    {}
};

static std::string str_tolower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Housekeeping: stop worker threads when IOC is exiting
void atExitHandler(void *_) {
    dev_map_t::iterator it;
    for (it = dev_map.begin(); it != dev_map.end(); ++it) {
        it->second->stop();
    }
}

void initHookHandler(initHookState state) {
    switch (state) {
        case initHookAfterIocRunning: {
            // Start the workers for all created devices on IOC startup
            dev_map_t::iterator it;
            for (it = dev_map.begin(); it != dev_map.end(); ++it) {
                it->second->start();
            }
            break;
        }
        default:
            break;
    }
}

void createCaenDigitizer(const std::string & name, const std::string & addr) {
    try {
        if (dev_map.find(name) != dev_map.end()) {
            std::string error = std::string("Digitizer with name ") + name + " already created";
            throw std::invalid_argument(error);
        }

        CaenDigitizer *dev = new CaenDigitizer(name, addr);
        dev_map[name] = dev;
    } catch (std::exception& e) {
        errlogPrintf(ERL_ERROR ": exception thrown in createCaenDigitizer: %s\n", e.what());
        throw e;
    }
}

// Device support
long get_dev_link_match(dbCommon *prec, const std::regex & re, std::cmatch & m) {
    DBLINK *plink = dbGetDevLink(prec);

    if (!plink) {
        recGblRecordError(S_db_badField, prec, "can't find dev link");
        errlogPrintf(ERL_ERROR " %s: can't find dev link\n", prec->name);
        return S_db_badField;
    }

    if (plink->type != INST_IO) {
        recGblRecordError(S_db_badField, prec, "unexpected link type");
        errlogPrintf(ERL_ERROR " %s: expected link type INST_IO, got %d\n", prec->name, plink->type);
        return S_db_badField;
    }

    const char *link = plink->value.instio.string;

    if (!std::regex_match(link, m, re)) {
        recGblRecordError(S_db_badField, prec, "unexpected link format");
        errlogPrintf(ERL_ERROR " %s: link '%s' does not match the expected format\n", prec->name, link);
        return S_db_badField;
    }

    return 0;
}

enum RecType {
    REC_INP,
    REC_OUT,
    REC_CMD,
};
long init_record_common(dbCommon *prec, enum RecType rt);

long init_record_inp(dbCommon *prec) {
    return init_record_common(prec, RecType::REC_INP);
}

long init_record_out(dbCommon *prec) {
    return init_record_common(prec, RecType::REC_OUT);
}

long init_record_cmd(dbCommon *prec) {
    return init_record_common(prec, RecType::REC_CMD);
}

long init_record_common(dbCommon *prec, enum RecType rt) {
    static std::regex LINK_RE("([^\\s]+) ([^\\s]+)");

    try {
        std::cmatch m;
        long ret = get_dev_link_match(prec, LINK_RE, m);
        if (ret)
            return ret;

        std::string dev_name = m[1].str();
        std::string path = m[2].str();

        auto dev = dev_map.find(dev_name);
        if (dev == dev_map.end()) {
            recGblRecordError(S_db_badField, prec, "failed to find device");
            errlogPrintf(ERL_ERROR " %s: failed to find device named '%s'."
                " Make sure to create one using createCaenDigitizer.\n", prec->name, dev_name.c_str());
            return S_db_badField;
        }

        auto digitizer = dev->second;
        CaenDigitizerParam *p = digitizer->get_parameter(path);
        prec->dpvt = new ParamPvt(p);

        // For output records, register a callback to refresh the value on connection
        // or on external change
        if (rt == RecType::REC_OUT) {
            p->register_callback([prec]() {
                dbScanLock(prec);
                static_cast<ParamPvt*>(prec->dpvt)->pending_update = true;
                prec->udf = 0;
                dbProcess(prec);
                static_cast<ParamPvt*>(prec->dpvt)->pending_update = false;
                dbScanUnlock(prec);
            });
        }

        return 0;
    } catch (std::exception & ex) {
        recGblRecordError(S_dev_badInit, prec, "failed to initialize record");
        errlogPrintf(ERL_ERROR " %s: got exception while initializing: %s\n", prec->name, ex.what());
        return S_dev_badInit;
    }
}

long init_record_chan(dbCommon *prec) {
    static std::regex LINK_RE("([^\\s]+) (\\d|\\d\\d)");

    try {
        std::cmatch m;
        long ret = get_dev_link_match(prec, LINK_RE, m);
        if (ret)
            return ret;

        std::string dev_name = m[1].str();
        std::string chan_str = m[2].str();

        auto dev = dev_map.find(dev_name);
        if (dev == dev_map.end()) {
            recGblRecordError(S_db_badField, prec, "failed to find device");
            errlogPrintf(ERL_ERROR " %s: failed to find device named '%s'."
                " Make sure to create one using createCaenDigitizer.\n", prec->name, dev_name.c_str());
            return S_db_badField;
        }

        auto digitizer = dev->second;
        size_t chan = std::stoul(chan_str);
        prec->dpvt = new ChannelPvt(digitizer, chan);

        return 0;
    } catch (std::exception & ex) {
        recGblRecordError(S_dev_badInit, prec, "failed to initialize record");
        errlogPrintf(ERL_ERROR " %s: got exception while initializing: %s\n", prec->name, ex.what());
        return S_dev_badInit;
    }
}

long get_status_update(int, dbCommon* prec, IOSCANPVT* scan) {
    ParamPvt *pvt = static_cast<ParamPvt*>(prec->dpvt);
    *scan = pvt->param->get_status_update();
    return 0;
}

long get_data_update(int, dbCommon* prec, IOSCANPVT* scan) {
    ChannelPvt *pvt = static_cast<ChannelPvt*>(prec->dpvt);
    *scan = pvt->dev->get_data_update();
    return 0;
}

template<typename R>
using read_func_t = std::function<void(CaenDigitizerParam*)>;

template<typename R>
long do_param_read(R *prec, long ret, read_func_t<R> read_func) {
    ParamPvt *pvt = static_cast<ParamPvt*>(prec->dpvt);
    CaenDigitizerParam *param = pvt->param;

    if (!param->is_connected()) {
        recGblSetSevrMsg(prec, COMM_ALARM, MAJOR_ALARM, "Disconnected");
        return ret;
    }

    try {
        read_func(param);
    } catch (std::exception & ex) {
        recGblSetSevr(prec, READ_ALARM, INVALID_ALARM);
        errlogPrintf(ERL_ERROR " %s: got exception while processing record: %s\n", prec->name, ex.what());
    }
    return ret;
}

template<typename R>
using write_func_t = std::function<void(CaenDigitizerParam*, bool)>;

template<typename R>
long do_param_write(R *prec, long ret, write_func_t<R> write_func) {
    ParamPvt *pvt = static_cast<ParamPvt*>(prec->dpvt);
    CaenDigitizerParam *param = pvt->param;
    bool pending_update = pvt->pending_update;

    if (!param->is_connected()) {
        recGblSetSevrMsg(prec, COMM_ALARM, MAJOR_ALARM, "Disconnected");
        return ret;
    }

    try {
        write_func(param, pending_update);
    } catch (std::exception & ex) {
        recGblSetSevr(prec, WRITE_ALARM, INVALID_ALARM);
        errlogPrintf(ERL_ERROR " %s: got exception while processing record: %s\n", prec->name, ex.what());
    }
    return ret;
}

long read_si(stringinRecord *prec) {
    return do_param_read(prec, 0, [&](CaenDigitizerParam *param) {
        std::string value;
        param->get_value(prec->time, value);
        snprintf(prec->val, sizeof(prec->val), "%s", value.c_str());
    });
}

long write_so(stringoutRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        if (pending_update) {
            std::string value;
            param->get_value(prec->time, value);
            snprintf(prec->val, sizeof(prec->val), "%s", value.c_str());
        } else {
            std::string value(prec->val);
            param->set_value(value);
        }
    });
}

long read_li(longinRecord *prec) {
    return do_param_read(prec, 0, [&](CaenDigitizerParam *param) {
        int64_t value;
        param->get_value(prec->time, value);
        prec->val = value;
    });
}

long write_lo(longoutRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        if (pending_update) {
            int64_t value;
            param->get_value(prec->time, value);
            prec->val = value;
        } else {
            int64_t value = prec->val;
            param->set_value(value);
        }
    });
}

long read_int64in(int64inRecord *prec) {
    return do_param_read(prec, 0, [&](CaenDigitizerParam *param) {
        int64_t value;
        param->get_value(prec->time, value);
        prec->val = value;
    });
}

long write_int64out(int64outRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        if (pending_update) {
            int64_t value;
            param->get_value(prec->time, value);
            prec->val = value;
        } else {
            int64_t value = prec->val;
            param->set_value(value);
        }
    });
}

long read_ai(aiRecord *prec) {
    return do_param_read(prec, 2, [&](CaenDigitizerParam *param) {
        double value;
        param->get_value(prec->time, value);
        prec->val = value;
    });
}

long write_ao(aoRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        if (pending_update) {
            double value;
            param->get_value(prec->time, value);
            prec->val = value;
        } else {
            double value = prec->val;
            param->set_value(value);
        }
    });
}

long read_bi(biRecord *prec) {
    return do_param_read(prec, 2, [&](CaenDigitizerParam *param) {
        bool value;
        param->get_value(prec->time, value);
        prec->val = value ? 1 : 0;
        prec->udf = 0;
    });
}

long write_bo(boRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        if (pending_update) {
            bool value;
            param->get_value(prec->time, value);
            prec->val = value ? 1 : 0;
            prec->udf = 0;
        } else {
            bool value = !!prec->val;
            param->set_value(value);
        }
    });
}

long read_mbbi(mbbiRecord *prec) {
    return do_param_read(prec, 2, [&](CaenDigitizerParam *param) {
        std::string value;
        param->get_value(prec->time, value);
        value = str_tolower(value);

        size_t i = 0;
        char (*p)[26] = &prec->zrst;
        for (; i < 16; ++p, ++i) {
            if (str_tolower(*p) == value) {
                prec->val = i;
                prec->udf = 0;
                return;
            }
        }
        throw std::runtime_error(
            std::string("Failed to match value '") + value + "' to one of the choices"
        );
    });
}

long write_mbbo(mbboRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        if (pending_update) {
            std::string value;
            param->get_value(prec->time, value);
            value = str_tolower(value);

            size_t i = 0;
            char (*p)[26] = &prec->zrst;
            for (; i < 16; ++p, ++i) {
                if (str_tolower(*p) == value) {
                    prec->val = i;
                    prec->udf = 0;
                    return;
                }
            }
            throw std::runtime_error(
                std::string("Failed to match value '") + value + "' to one of the choices"
            );
        } else {
            char (*p)[26] = &prec->zrst;
            p += prec->val;
            std::string value = *p;
            param->set_value(value);
        }
    });
}

long send_command_bo(boRecord *prec) {
    return do_param_write(prec, 0, [&](CaenDigitizerParam *param, bool pending_update) {
        param->send_command();
    });
}

long read_chan_data(waveformRecord *prec) {
    assert(prec->ftvl==menuFtypeUSHORT);

    ChannelPvt *pvt = static_cast<ChannelPvt*>(prec->dpvt);
    uint16_t *bptr = (uint16_t *)prec->bptr;

    try {
        pvt->dev->with_latest_event([&](Event *event) {
            size_t ch = pvt->chan;

            size_t nelm_record = prec->nelm;
            size_t nelm_event = event->n_samples[ch];

            size_t nord = std::min(nelm_record, nelm_event);
            prec->nord = nord;

            if (nord)
                memcpy(bptr, event->waveform[ch], nord*sizeof(*bptr));

            /*
            TODO: derive time from event->timestamp
            if(prec->tse==epicsTimeEventDeviceTime) {
            prec->time = info->dev->updatetime;
            */
        });
    } catch (std::exception & ex) {
        recGblSetSevr(prec, READ_ALARM, INVALID_ALARM);
        errlogPrintf(ERL_ERROR " %s: got exception while processing record: %s\n", prec->name, ex.what());
    }
    return 0;
}

// EPICS Registration
const iocshArg createCaenDigitizerArg0 = {"name", iocshArgString};
const iocshArg createCaenDigitizerArg1 = {"addr", iocshArgString};
const iocshArg * const createCaenDigitizerArgs[] = {
    &createCaenDigitizerArg0, &createCaenDigitizerArg1
};
const iocshFuncDef createCaenDigitizerFuncDef = {
    "createCaenDigitizer", 2, createCaenDigitizerArgs
};
void createCaenDigitizerCall(const iocshArgBuf *args) {
    createCaenDigitizer(args[0].sval, args[1].sval);
}

void caenDigitizerRegistrar() {
    initHookRegister(initHookHandler);
    epicsAtExit(&atExitHandler, NULL);
    iocshRegister(&createCaenDigitizerFuncDef, &createCaenDigitizerCall);
}

template<typename R>
struct dset6 {
    long N;
    long (*report)(R*);
    long (*init)(int);
    long (*init_record)(dbCommon*);
    long (*get_io_intr_info)(int, dbCommon* prec, IOSCANPVT*);
    long (*readwrite)(R*);
    long (*linconv)(R*);
};

#define DSET(NAME, REC, INIT, IOINTR, RW) static dset6<REC ## Record> NAME = \
    {6, NULL, NULL, INIT, IOINTR, RW, NULL}; epicsExportAddress(dset, NAME)

DSET(devCaenDigParamSi,       stringin,  &init_record_inp,  &get_status_update, &read_si);
DSET(devCaenDigParamSo,       stringout, &init_record_out,  NULL,               &write_so);
DSET(devCaenDigParamLi,       longin,    &init_record_inp,  &get_status_update, &read_li);
DSET(devCaenDigParamLo,       longout,   &init_record_out,  NULL,               &write_lo);
DSET(devCaenDigParamInt64In,  int64in,   &init_record_inp,  &get_status_update, &read_int64in);
DSET(devCaenDigParamInt64Out, int64out,  &init_record_out,  NULL,               &write_int64out);
DSET(devCaenDigParamAi,       ai,        &init_record_inp,  &get_status_update, &read_ai);
DSET(devCaenDigParamAo,       ao,        &init_record_out,  NULL,               &write_ao);
DSET(devCaenDigParamBi,       bi,        &init_record_inp,  &get_status_update, &read_bi);
DSET(devCaenDigParamBo,       bo,        &init_record_out,  NULL,               &write_bo);
DSET(devCaenDigParamMbbi,     mbbi,      &init_record_inp,  &get_status_update, &read_mbbi);
DSET(devCaenDigParamMbbo,     mbbo,      &init_record_out,  NULL,               &write_mbbo);
DSET(devCaenDigCmdBo,         bo,        &init_record_cmd,  NULL,               &send_command_bo);
DSET(devCaenDigChDataWf,      waveform,  &init_record_chan, &get_data_update,   &read_chan_data);

epicsExportRegistrar(caenDigitizerRegistrar);
