@echo off
rem SPDX-License-Identifier: Apache-2.0
rem
rem Copyright (C) 2026 ArtInChip Technology Co., Ltd

setlocal EnableDelayedExpansion

git --version >nul 2>&1
if errorlevel 1 (
	goto :eof
)

if "%SDK_PRJ_TOP_DIR%"=="" (
	goto :eof
)

rem Case 1: the whole SDK is a single git repository
if exist "%SDK_PRJ_TOP_DIR%\.git" (
	call :checkout_one "%SDK_PRJ_TOP_DIR%" ""
	goto :eof
)

rem Case 2: the SDK is managed by repo, check each sub-project
if exist "%SDK_PRJ_TOP_DIR%\.repo\project.list" (
	for /f "usebackq delims=" %%p in ("%SDK_PRJ_TOP_DIR%\.repo\project.list") do (
		set PROJ=%%p
		rem The project path in project.list may end with '/', strip it
		if "!PROJ:~-1!"=="/" set PROJ=!PROJ:~0,-1!
		if exist "%SDK_PRJ_TOP_DIR%\!PROJ!\.git" (
			call :checkout_one "%SDK_PRJ_TOP_DIR%\!PROJ!" "!PROJ!"
		)
	)
	goto :eof
)

rem No git repository and no repo information, do nothing
goto :eof

rem %~1 - git project directory
rem %~2 - display prefix of the project path (optional)
:checkout_one
set PROJECT_DIR=%~1
set PREFIX=%~2

git -C "%PROJECT_DIR%" status >nul 2>&1
if not errorlevel 1 (
	rem Restore the prebuilt libraries (*.a) modified in the workspace
	for /f "tokens=1,*" %%a in ('git -C "%PROJECT_DIR%" status --porcelain -- *.a 2^>nul') do (
		if "%%a"=="M" (
			if exist "%PROJECT_DIR%\%%b" (
				set LIB_PATH=%%b
				if not "!PREFIX!"=="" set LIB_PATH=!PREFIX!\%%b
				echo Checkout !LIB_PATH!
				git -C "%PROJECT_DIR%" checkout -- "%%b"
			)
		)
	)
)
exit /b
