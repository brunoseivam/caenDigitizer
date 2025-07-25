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

#include <stringinRecord.h>
#include <longinRecord.h>
#include <longoutRecord.h>
#include <aiRecord.h>
#include <aoRecord.h>
#include <biRecord.h>
#include <boRecord.h>
#include <mbbiRecord.h>
#include <mbboRecord.h>

typedef std::map<std::string, CaenDigitizer*> dev_map_t;
static dev_map_t dev_map;

void destroyCaenDigitizer(void *dev);

void createCaenDigitizer(const std::string & name, const std::string & addr) {
    try {
        if (dev_map.find(name) != dev_map.end()) {
            std::string error = std::string("Digitizer with name ") + name + " already created";
            throw std::invalid_argument(error);
        }

        CaenDigitizer *dev = new CaenDigitizer(name, addr);
        epicsAtExit(&destroyCaenDigitizer, dev);
        dev_map[name] = dev;
    } catch (std::exception& e) {
        errlogPrintf(ERL_ERROR ": exception thrown in createCaenDigitizer: %s\n", e.what());
        throw e;
    }
}

void destroyCaenDigitizer(void *dev) {
    static_cast<CaenDigitizer*>(dev)->destroy();
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

struct Pvt {
    CaenDigitizer *digitizer;
    uint64_t handle;
    std::string path;
    std::string data_type;

    Pvt(CaenDigitizer *digitizer, uint64_t handle, const std::string & path,
        const std::string & data_type
    ) : digitizer(digitizer), handle(handle), path(path), data_type(data_type)
    {}
};

long init_record_common(dbCommon *prec) {
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
        uint64_t handle = digitizer->get_handle(path);
        Pvt *pvt = new Pvt(digitizer, handle, path, data_type);
        prec->dpvt = pvt;

        return 0;
    } catch (std::exception & ex) {
        recGblRecordError(S_dev_badInit, prec, "failed to initialize record");
        errlogPrintf(ERL_ERROR " %s: got exception while initializing: %s\n", prec->name, ex.what());
        return S_dev_badInit;
    }
}

template<typename R>
using io_func_t = std::function<void(Pvt*)>;

template<typename R>
long do_param_io(R *prec, long ret, int alarm, io_func_t<R> io_func) {
    Pvt *pvt = static_cast<Pvt*>(prec->dpvt);

    try {
        io_func(pvt);
    } catch (std::exception & ex) {
        recGblSetSevr(prec, alarm, INVALID_ALARM);
        errlogPrintf(ERL_ERROR " %s: got exception while processing record: %s\n", prec->name, ex.what());
    }
    return ret;
}

long read_si(stringinRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](Pvt *pvt) {
        std::string value(pvt->digitizer->get_value(pvt->handle));
        snprintf(prec->val, sizeof(prec->val), "%s", value.c_str());
    });
}

long read_li(longinRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](Pvt *pvt) {
        prec->val = pvt->digitizer->get_int_value(pvt->handle);
    });
}

long write_lo(longoutRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](Pvt *pvt) {
        pvt->digitizer->set_int_value(pvt->handle, prec->val);
    });
}

long read_ai(aiRecord *prec) {
    return do_param_io(prec, 2, READ_ALARM, [&](Pvt *pvt) {
        prec->val = pvt->digitizer->get_double_value(pvt->handle);
    });
}

long write_ao(aoRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](Pvt *pvt) {
        pvt->digitizer->set_double_value(pvt->handle, prec->val);
    });
}

long read_bi(biRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](Pvt *pvt) {
        prec->val = pvt->digitizer->get_bool_value(pvt->handle) ? 1 : 0;
    });
}

long write_bo(boRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](Pvt *pvt) {
        pvt->digitizer->set_bool_value(pvt->handle, !!prec->val);
    });
}

std::string str_tolower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

long read_mbbi(mbbiRecord *prec) {
    return do_param_io(prec, 0, READ_ALARM, [&](Pvt *pvt) {
        std::string value(str_tolower(pvt->digitizer->get_value(pvt->handle)));

        size_t i = 0;
        char (*p)[26] = &prec->zrst;
        for (; i < 16; ++p, ++i) {
            //printf("%s[%lu] = %s    '%s'\n", prec->name, i, *p, value.c_str());
            if (str_tolower(*p) == value) {
                prec->val = i;
                return;
            }
        }
        throw std::runtime_error(std::string("Failed to match value '") + value + "' to one of the choices");
    });
}

long write_mbbo(mbboRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](Pvt *pvt) {
        char (*p)[26] = &prec->zrst;
        p += prec->val;
        pvt->digitizer->set_value(pvt->handle, *p);
    });
}

long send_command_bo(boRecord *prec) {
    return do_param_io(prec, 0, WRITE_ALARM, [&](Pvt *pvt) {
        pvt->digitizer->send_command(pvt->handle);
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

#define DSET(NAME, REC, RW) static dset6<REC ## Record> NAME = \
    {6, NULL, NULL, &init_record_common, NULL, RW, NULL}; epicsExportAddress(dset, NAME)

DSET(devCaenDigParamSi, stringin, &read_si);
DSET(devCaenDigParamLi, longin, &read_li);
DSET(devCaenDigParamLo, longout, &write_lo);
DSET(devCaenDigParamAi, ai, &read_ai);
DSET(devCaenDigParamAo, ao, &write_ao);
DSET(devCaenDigParamBi, bi, &read_bi);
DSET(devCaenDigParamBo, bo, &write_bo);
DSET(devCaenDigParamMbbi, mbbi, &read_mbbi);
DSET(devCaenDigParamMbbo, mbbo, &write_mbbo);

DSET(devCaenDigCmdBo, bo, &send_command_bo);

epicsExportRegistrar(caenDigitizerRegistrar);
