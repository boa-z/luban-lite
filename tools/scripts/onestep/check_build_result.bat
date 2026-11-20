@echo off
rem SPDX-License-Identifier: Apache-2.0
rem
rem Copyright (C) 2026 ArtInChip Technology Co., Ltd

rem Check the build result by parsing the build log.
rem
rem The exit code of 'scons' can not be got here, for it is piped to 'tee',
rem which does not pass the exit code of 'scons' back, so the log is the only
rem evidence:
rem   1. 'scons: done building targets.' is printed when the build ends
rem      without error, including the case that all targets are up to date.
rem      'Luban-Lite is built successfully' is not used here, for it is
rem      printed by the post action and may be lost in an incomplete log.
rem   2. Any error ends the build with 'scons: *** <target> Error <n>' and
rem      'scons: building terminated because of errors.'(by default), or
rem      'scons: done building targets (errors occurred during build).'(with
rem      -k, note that it does not contain 'done building targets.').
rem
rem %1 - the build log file
rem return 0: the build is OK, others: the build is failed

setlocal EnableDelayedExpansion

set LOG_FILE=%~1

set BUILD_OK=0
if exist "!LOG_FILE!" (
	rem The build must end normally, otherwise the log may be incomplete
	findstr /c:"scons: done building targets." "!LOG_FILE!" > nul
	if !errorlevel! equ 0 set BUILD_OK=1

	rem Any error message in log means the build is failed
	findstr /c:"scons: ***" /c:"terminated because of errors" ^
		/c:"errors occurred during" "!LOG_FILE!" > nul
	if !errorlevel! equ 0 set BUILD_OK=0
)

if !BUILD_OK! equ 1 exit /b 0

exit /b 1
