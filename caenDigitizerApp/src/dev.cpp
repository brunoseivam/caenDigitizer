#include "caen_digitizer.h"

#include <algorithm>
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

#include <stringinRecord.h>
#include <stringoutRecord.h>
#include <longinRecord.h>
#include <longoutRecord.h>
#include <aiRecord.h>
#include <aoRecord.h>
#include <biRecord.h>
#include <boRecord.h>
#include <mbbiRecord.h>
#include <mbboRecord.h>
#include <mbbiDirectRecord.h>

typedef std::map<std::string, CaenDigitizer*> dev_map_t;
static dev_map_t dev_map;

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

DBLINK* get_dev_link(dbCommon *prec) {
    DBLINK *ret = NULL;
    DBENTRY ent;

    dbInitEntry(pdbbase, &ent);

    dbFindRecord(&ent, prec->name);
    assert(ent.precnode->precord==(void*)prec);

    if (dbFindField(&ent, "INP") == 0 || dbFindField(&ent, "OUT") == 0) {
        ret = (DBLINK*)((char*)prec + ent.pflddes->offset);
    }

    dbFinishEntry(&ent);
    return ret;
}

long init_record_common(dbCommon *prec) {
    // TODO: remove param type from link
    static std::regex LINK_RE("([^\\s]+) (INT|DBL|STR|CMD|BOOL) ([^\\s]+)");

    try {
        DBLINK *plink = get_dev_link(prec);
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

        std::cmatch m;
        if (!std::regex_match(link, m, LINK_RE)) {
            recGblRecordError(S_db_badField, prec, "unexpected link format");
            errlogPrintf(ERL_ERROR " %s: link '%s' does not match the expected format\n", prec->name, link);
            return S_db_badField;
        }

        std::string dev_name = m[1].str();
        std::string data_type = m[2].str();
        std::string path = m[3].str();

        auto dev = dev_map.find(dev_name);
        if (dev == dev_map.end()) {
            recGblRecordError(S_db_badField, prec, "failed to find device");
            errlogPrintf(ERL_ERROR " %s: link '%s' failed to find device named '%s'."
                " Make sure to create one using createCaenDigitizer.\n", prec->name, link, dev_name.c_str());
            return S_db_badField;
        }

        auto digitizer = dev->second;
        CaenDigitizerParam *p = digitizer->get_parameter(path);
        prec->dpvt = p;

        return 0;
    } catch (std::exception & ex) {
        recGblRecordError(S_dev_badInit, prec, "failed to initialize record");
        errlogPrintf(ERL_ERROR " %s: got exception while initializing: %s\n", prec->name, ex.what());
        return S_dev_badInit;
    }
}

long get_status_update(int, dbCommon* prec, IOSCANPVT* scan) {
    CaenDigitizerParam *pvt = static_cast<CaenDigitizerParam*>(prec->dpvt);
    *scan = pvt->get_status_update();
    return 0;
}

template<typename R>
using io_func_t = std::function<void(CaenDigitizerParam*)>;

template<typename R>
long do_param_io(R *prec, long ret, int alarm, io_func_t<R> io_func) {
    CaenDigitizerParam *pvt = static_cast<CaenDigitizerParam*>(prec->dpvt);

    try {
        io_func(pvt);
    } catch (std::exception & ex) {
        recGblSetSevr(prec, alarm, INVALID_ALARM);
        errlogPrintf(ERL_ERROR " %s: got exception while processing record: %s\n", prec->name, ex.what());
    }
    return ret;
}

long read_si(stringinRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](CaenDigitizerParam *param) {
        std::string value;
        param->get_value(value);
        snprintf(prec->val, sizeof(prec->val), "%s", value.c_str());
    });
}

long write_so(stringoutRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](CaenDigitizerParam *param) {
        std::string value(prec->val);
        param->set_value(value);
    });
}

long read_li(longinRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](CaenDigitizerParam *param) {
        int64_t value;
        param->get_value(value);
        prec->val = value;
    });
}

long write_lo(longoutRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](CaenDigitizerParam *param) {
        int64_t value = prec->val;
        param->set_value(value);
    });
}

long read_ai(aiRecord *prec) {
    return do_param_io(prec, 2, READ_ALARM, [&](CaenDigitizerParam *param) {
        int64_t value;
        param->get_value(value);
        prec->val = value;
    });
}

long write_ao(aoRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](CaenDigitizerParam *param) {
        double value = prec->val;
        param->set_value(value);
    });
}

long read_bi(biRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](CaenDigitizerParam *param) {
        bool value;
        param->get_value(value);
        prec->val = value ? 1 : 0;
    });
}

long write_bo(boRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](CaenDigitizerParam *param) {
        bool value = !!prec->val;
        param->set_value(value);
    });
}

long read_mbbi(mbbiRecord *prec) {
    return do_param_io(prec, 2, READ_ALARM, [&](CaenDigitizerParam *param) {
        std::string value;
        param->get_value(value);
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
    return do_param_io(prec, 0, WRITE_ALARM, [&](CaenDigitizerParam *param) {
        char (*p)[26] = &prec->zrst;
        p += prec->val;
        std::string value = *p;
        param->set_value(value);
    });
}

long send_command_bo(boRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](CaenDigitizerParam *param) {
        param->send_command();
    });
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

#define DSET(NAME, REC, IOINTR, RW) static dset6<REC ## Record> NAME = \
    {6, NULL, NULL, &init_record_common, IOINTR, RW, NULL}; epicsExportAddress(dset, NAME)

DSET(devCaenDigParamSi, stringin, &get_status_update, &read_si);
DSET(devCaenDigParamSo, stringout, NULL, &write_so);
DSET(devCaenDigParamLi, longin, &get_status_update, &read_li);
DSET(devCaenDigParamLo, longout, NULL, &write_lo);
DSET(devCaenDigParamAi, ai, &get_status_update, &read_ai);
DSET(devCaenDigParamAo, ao, NULL, &write_ao);
DSET(devCaenDigParamBi, bi, &get_status_update, &read_bi);
DSET(devCaenDigParamBo, bo, NULL, &write_bo);
DSET(devCaenDigParamMbbi, mbbi, &get_status_update, &read_mbbi);
DSET(devCaenDigParamMbbo, mbbo, NULL, &write_mbbo);

DSET(devCaenDigCmdBo, bo, NULL, &send_command_bo);

epicsExportRegistrar(caenDigitizerRegistrar);
