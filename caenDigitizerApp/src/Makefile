TOP=../..

include $(TOP)/configure/CONFIG

PROD_IOC = caenDigitizer

DBD += $(PROD_IOC).dbd

$(PROD_IOC)_DBD += base.dbd
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
