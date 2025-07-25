#!../../bin/linux-x86_64/caenDigitizer

< envPaths

epicsEnvSet("PREFIX", "DT274X:")
epicsEnvSet("PORT",   "DIG1")
epicsEnvSet("ADDR",   "dig2://127.0.0.1")

epicsEnvSet("LD_LIBRARY_PATH", "/home/bmartins/osprey/epics/caen/lib")

dbLoadDatabase("$(TOP)/dbd/caenDigitizer.dbd")
caenDigitizer_registerRecordDeviceDriver(pdbbase)

createCaenDigitizer("$(PORT)", "$(ADDR)")

dbLoadRecords("$(TOP)/db/dt274x.db","P=$(PREFIX),R=,PORT=$(PORT)")

iocInit()
