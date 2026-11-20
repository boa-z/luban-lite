@echo off
rem SPDX-License-Identifier: Apache-2.0
rem
rem Copyright (C) 2023-2024 ArtInChip Technology Co., Ltd

if "%CD%"=="C:\Windows\system32" (
    echo Current user is administrator, need change path to Luban-Lite ...
    cd /d %~dp0
)

rem Luban-Lite 2.0 (RT-Thread 5.x): skip the ConEmu launcher and delegate to the
rem single robust entry win_cmd.bat. tools/ is shared with 1.0, so gate on the
rem LUBAN_LITE_2_0 marker; 1.0 keeps the original ConEmu behavior below.
findstr /C:"LUBAN_LITE_2_0" "%~dp0kernel\rt-thread\Kconfig" >nul 2>&1
if not errorlevel 1 (
    cd /d "%~dp0"
    call "%~dp0win_cmd.bat" %*
    exit /b %errorlevel%
)

if /i "%processor_architecture%"=="x86" (
        start  .\tools\env\tools\ConEmu\ConEmu.exe 
) else if /i "%processor_architecture%"=="amd64" (
    if defined processor_architew6432 (
        start  .\tools\env\tools\ConEmu\ConEmu.exe 
    ) else (
        start  .\tools\env\tools\ConEmu\ConEmu64.exe 
    )
)

@echo success
