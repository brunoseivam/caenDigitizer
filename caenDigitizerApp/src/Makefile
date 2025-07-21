# SPDX-FileCopyrightText: 2003 Argonne National Laboratory
#
# SPDX-License-Identifier: EPICS

TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

PROD_IOC = caenDigitizer
# caenDigitizer.dbd will be created and installed
DBD += caenDigitizer.dbd

# caenDigitizer.dbd will be made up from these files:
caenDigitizer_DBD += base.dbd

# Include dbd files from all support applications:
#caenDigitizer_DBD += xxx.dbd

# Add all the support libraries needed by this IOC
#caenDigitizer_LIBS += xxx

# caenDigitizer_registerRecordDeviceDriver.cpp derives from caenDigitizer.dbd
caenDigitizer_SRCS += caenDigitizer_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
caenDigitizer_SRCS_DEFAULT += caenDigitizerMain.cpp
caenDigitizer_SRCS_vxWorks += -nil-

# Add support from base/src/vxWorks if needed
#caenDigitizer_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
caenDigitizer_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

