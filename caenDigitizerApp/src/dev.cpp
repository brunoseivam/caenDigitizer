#include "caen_digitizer.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <map>

#include <iocsh.h>
#include <epicsExit.h>
#include <errlog.h>
#ifndef ERL_ERROR
#  define ERL_ERROR "ERROR"
#endif
#include <dbAccess.h>
#include <dbStaticLib.h>
#include <epicsExport.h>

#include <stringinRecord.h>



typedef std::map<std::string, CaenDigitizer*> dev_map_t;
static dev_map_t dev_map;


void destroyCaenDigitizer(void *dev) {
    static_cast<CaenDigitizer*>(dev)->destroy();
}

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

DBLINK* get_dev_link(dbCommon *prec)
{
    DBLINK *ret = NULL;
    DBENTRY ent;

    dbInitEntry(pdbbase, &ent);

    dbFindRecord(&ent, prec->name);
    assert(ent.precnode->precord==(void*)prec);

    if(dbFindField(&ent, "INP") && dbFindField(&ent, "OUT")) {
        errlogPrintf(ERL_ERROR " %s: can't find dev link\n", prec->name);
    } else {
        ret = (DBLINK*)((char*)prec + ent.pflddes->offset);
    }

    dbFinishEntry(&ent);
    return ret;
}

long init_record_common(dbCommon *prec) {
    try {
        DBLINK *plink = get_dev_link(prec);
        if (!plink)
            return 0;

        if (plink->type != INST_IO) {
            errlogPrintf(ERL_ERROR " %s: expected link type INST_IO, got %d\n", prec->name, plink->type);
            return 0;
        }

        printf("GOT LINK '%s'\n", plink->value.instio.string);

        return 0;
    } catch (std::exception & e) {
        errlogPrintf(ERL_ERROR " %s: got exception while initializing: %s\n", prec->name, e.what());
        return 0;
    }
}

// Device support
long read_string_si(stringinRecord *prec) {
    snprintf(prec->val, sizeof(prec->val), "Something happened");
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

DSET(devCaenDigSi, stringin, &read_string_si);

epicsExportRegistrar(caenDigitizerRegistrar);
