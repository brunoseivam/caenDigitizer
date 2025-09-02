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

# Setup example acquisition for the first channel

# Wait for connection
epicsThreadSleep 2.0

# Enable channel 0
dbpf $(PREFIX)Ch0:Enable 1

# Setup number of samples for acquisition
dbpf $(PREFIX)RecordLenS 1024

# Setup number of samples for pre-trigger
dbpf $(PREFIX)PreTriggerS 100

# Setup Trigger Source
dbpf $(PREFIX)AcqTrigSource_C03 1
dbpf $(PREFIX)AcqTrigSource_C11 1

# Configure test pulse
dbpf $(PREFIX)TestPulsePeriod 100e6
dbpf $(PREFIX)TestPulseWidth 1000

# Configure DC offset
dbpf $(PREFIX)Ch0:DCOffset 50.0

# Arm and start acquisition
dbpf $(PREFIX)Cmd:ArmAcquisition 1
dbpf $(PREFIX)Cmd:SWStartAcquisition 1
