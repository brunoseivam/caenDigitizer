TOP=../..

include $(TOP)/configure/CONFIG

PROD_IOC = caenDigitizer

DBD += $(PROD_IOC).dbd

Common_DBDs += base.dbd
Common_DBDs += system.dbd


Common_SRCs +=

ifneq ($(PMAC),)
Common_DBDs += drvAsynPowerPMACPort.dbd 
Common_DBDs += pmacAsynIPPort.dbd 
Common_DBDs += pmacAsynMotorPort.dbd
Common_LIBs += pmacAsynIPPort
Common_LIBs += pmacAsynMotorPort
Common_LIBs += powerPmacAsynPort
endif

ifneq ($(MOTOR),)
Common_DBDs += motorSupport.dbd
Common_DBDs += devSoftMotor.dbd
Common_LIBs += motor
Common_LIBs += softMotor
endif

ifneq ($(ASYN),)
Common_DBDs += asyn.dbd
Common_DBDs += drvAsynIPPort.dbd
Common_DBDs += drvAsynSerialPort.dbd
Common_LIBs += asyn
endif

ifneq ($(MODBUS),)
Common_DBDs += modbusSupport.dbd
Common_LIBs += modbus
endif

ifneq ($(SNMP),)
Common_DBDs += devSnmp.dbd
Common_LIBs += devSnmp
SYS_PROD_LIBS += netsnmp
endif

ifneq ($(STD),)
Common_DBDs += stdSupport.dbd
Common_LIBs += std
endif

ifneq ($(CALC),)
Common_DBDs += calcSupport.dbd
Common_LIBs += calc
endif

ifneq ($(AUTOSAVE),)
Common_DBDs += asSupport.dbd
Common_LIBs += autosave
endif

ifneq ($(IOCSTATS),)
Common_DBDs += devIocStats.dbd
Common_LIBs += devIocStats
endif

ifneq ($(RECCASTER),)
Common_DBDs += reccaster.dbd
Common_LIBs += reccaster
endif

ifneq ($(SNCSEQ),)
 sncExample_SNCFLAGS += +r
 #Common_DBDs += sncExample.dbd
 #A .stt sequence program is *not* pre-processed:
 #Common_SRCs += sncExample.stt
 Common_LIBs += seq pv
endif

ifneq ($(STREAM),)
Common_DBDs += stream.dbd
Common_LIBs += stream
endif

ifneq ($(RETOOLS),)
Common_DBDs += retools.dbd
Common_LIBs += retools
endif

ifneq ($(CAPUTLOG),)
Common_DBDs += caPutLog.dbd
Common_LIBs += caPutLog
endif

ifneq ($(MEASCOMP),)
Common_DBDs += measCompSupport.dbd
Common_LIBs += measComp
mot_SYS_LIBS += uldaq
endif

ifneq ($(SSCAN),)
Common_DBDs += sscanSupport.dbd
Common_LIBs += sscan
endif

ifneq ($(BUSY),)
Common_DBDs += busySupport.dbd
Common_LIBs += busy
endif

ifneq ($(SCALER),)
Common_DBDs += scalerSupport.dbd
Common_LIBs += scaler
endif

ifneq ($(MCA),)
Common_DBDs += mcaSupport.dbd
Common_LIBs += mca
endif

ifneq ($(ETHER_IP),)
Common_DBDs += ether_ip.dbd
Common_LIBs += ether_ip
endif

# https://epics.anl.gov/tech-talk/2024/msg00460.php
# QSRV2
ifdef PVXS_MAJOR_VERSION # prefer QSRV2 :)
Common_DBDs += pvxsIoc.dbd
Common_LIBs += pvxsIoc pvxs
else
ifdef EPICS_QSRV_MAJOR_VERSION # fallback to QSRV1
Common_LIBs += qsrv
Common_LIBs += $(EPICS_BASE_PVA_CORE_LIBS)
Common_DBDs += PVAServerRegister.dbd
Common_DBDs += qsrv.dbd
endif
endif

# Include dbd files from all support applications:
#$(PROD_IOC)_DBD += xxx.dbd
$(PROD_IOC)_DBD += $(Common_DBDs)
$(PROD_IOC)_LIBS += $(Common_LIBs)
$(PROD_IOC)_SRCS += $(Common_SRCs)
$(PROD_IOC)_DBD += $(PROD_IOC)Support.dbd



$(PROD_IOC)_SRCS += $(PROD_IOC)_registerRecordDeviceDriver.cpp

# Wrapper around CAEN_FELib
$(PROD_IOC)_SRCS += caen_digitizer.cpp

# Device Support implementation
$(PROD_IOC)_SRCS += dev.cpp

# Build the main IOC entry point
$(PROD_IOC)_SRCS += $(PROD_IOC)Main.cpp

$(PROD_IOC)_LIBS += $(EPICS_BASE_IOC_LIBS)

# Link to CAEN libraries, which are assumed to be installed on the system
$(PROD_IOC)_SYS_LIBS += CAEN_Dig2 CAEN_FELib

include $(TOP)/configure/RULES
