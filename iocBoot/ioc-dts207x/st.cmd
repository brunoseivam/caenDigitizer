#!../../bin/linux-x86_64/caenDigitizer

#- SPDX-FileCopyrightText: 2005 Argonne National Laboratory
#-
#- SPDX-License-Identifier: EPICS

#- You may have to change caenDigitizer to something else
#- everywhere it appears in this file

#< envPaths

## Register all support components
dbLoadDatabase "../../dbd/caenDigitizer.dbd"
caenDigitizer_registerRecordDeviceDriver(pdbbase) 

## Load record instances
#dbLoadRecords("../../db/caenDigitizer.db","user=bmartins")

iocInit()

## Start any sequence programs
#seq snccaenDigitizer,"user=bmartins"
