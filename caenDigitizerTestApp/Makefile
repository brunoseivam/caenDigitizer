TOP=..

include $(TOP)/configure/CONFIG

PROD_IOC = caenDigitizerTest
$(PROD_IOC)_SRCS += caenDigitizerTest.cpp
$(PROD_IOC)_LIBS += $(EPICS_BASE_IOC_LIBS)

# Link to CAEN libraries, which are assumed to be installed on the system
$(PROD_IOC)_SYS_LIBS += CAEN_Dig2 CAEN_FELib

include $(TOP)/configure/RULES
