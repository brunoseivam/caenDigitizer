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

# Enable channels 0-3, disable rest
dbpf $(PREFIX)Ch0:Enable 1
dbpf $(PREFIX)Ch1:Enable 1
dbpf $(PREFIX)Ch2:Enable 1
dbpf $(PREFIX)Ch3:Enable 1
dbpf $(PREFIX)Ch4:Enable 0
dbpf $(PREFIX)Ch5:Enable 0
dbpf $(PREFIX)Ch6:Enable 0
dbpf $(PREFIX)Ch7:Enable 0
dbpf $(PREFIX)Ch8:Enable 0
dbpf $(PREFIX)Ch9:Enable 0
dbpf $(PREFIX)Ch10:Enable 0
dbpf $(PREFIX)Ch11:Enable 0
dbpf $(PREFIX)Ch12:Enable 0
dbpf $(PREFIX)Ch13:Enable 0
dbpf $(PREFIX)Ch14:Enable 0
dbpf $(PREFIX)Ch15:Enable 0
dbpf $(PREFIX)Ch16:Enable 0
dbpf $(PREFIX)Ch17:Enable 0
dbpf $(PREFIX)Ch18:Enable 0
dbpf $(PREFIX)Ch19:Enable 0
dbpf $(PREFIX)Ch20:Enable 0
dbpf $(PREFIX)Ch21:Enable 0
dbpf $(PREFIX)Ch22:Enable 0
dbpf $(PREFIX)Ch23:Enable 0
dbpf $(PREFIX)Ch24:Enable 0
dbpf $(PREFIX)Ch25:Enable 0
dbpf $(PREFIX)Ch26:Enable 0
dbpf $(PREFIX)Ch27:Enable 0
dbpf $(PREFIX)Ch28:Enable 0
dbpf $(PREFIX)Ch29:Enable 0
dbpf $(PREFIX)Ch30:Enable 0
dbpf $(PREFIX)Ch31:Enable 0
dbpf $(PREFIX)Ch32:Enable 0
dbpf $(PREFIX)Ch33:Enable 0
dbpf $(PREFIX)Ch34:Enable 0
dbpf $(PREFIX)Ch35:Enable 0
dbpf $(PREFIX)Ch36:Enable 0
dbpf $(PREFIX)Ch37:Enable 0
dbpf $(PREFIX)Ch38:Enable 0
dbpf $(PREFIX)Ch39:Enable 0
dbpf $(PREFIX)Ch40:Enable 0
dbpf $(PREFIX)Ch41:Enable 0
dbpf $(PREFIX)Ch42:Enable 0
dbpf $(PREFIX)Ch43:Enable 0
dbpf $(PREFIX)Ch44:Enable 0
dbpf $(PREFIX)Ch45:Enable 0
dbpf $(PREFIX)Ch46:Enable 0
dbpf $(PREFIX)Ch47:Enable 0
dbpf $(PREFIX)Ch48:Enable 0
dbpf $(PREFIX)Ch49:Enable 0
dbpf $(PREFIX)Ch50:Enable 0
dbpf $(PREFIX)Ch51:Enable 0
dbpf $(PREFIX)Ch52:Enable 0
dbpf $(PREFIX)Ch53:Enable 0
dbpf $(PREFIX)Ch54:Enable 0
dbpf $(PREFIX)Ch55:Enable 0
dbpf $(PREFIX)Ch56:Enable 0
dbpf $(PREFIX)Ch57:Enable 0
dbpf $(PREFIX)Ch58:Enable 0
dbpf $(PREFIX)Ch59:Enable 0
dbpf $(PREFIX)Ch60:Enable 0
dbpf $(PREFIX)Ch61:Enable 0
dbpf $(PREFIX)Ch62:Enable 0
dbpf $(PREFIX)Ch63:Enable 0

# Setup number of samples for acquisition
dbpf $(PREFIX)RecordLenS 4096

# Setup number of samples for pre-trigger
dbpf $(PREFIX)PreTriggerS 128

# Setup Trigger Source
dbpf $(PREFIX)AcqTrigSource_C03 1
dbpf $(PREFIX)AcqTrigSource_C11 1

# Configure test pulse
dbpf $(PREFIX)TestPulsePeriod 100000000
dbpf $(PREFIX)TestPulseWidth 1000

# Arm and start acquisition
dbpf $(PREFIX)Cmd:ArmAcquisition 1
dbpf $(PREFIX)Cmd:SWStartAcquisition 1
