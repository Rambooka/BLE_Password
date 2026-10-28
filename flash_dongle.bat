@echo off
REM ONE-TIME migration of an official nRF52840 Dongle to MCUboot.
REM After this (and step 2 below), firmware updates go over BLE from the
REM Android app ("UPDATE DONGLE FIRMWARE") - this script is not needed again.
REM
REM Usage: flash_dongle.bat [COM_PORT]      Default: COM20

set COM_PORT=%1
if "%COM_PORT%"=="" set COM_PORT=COM20

set PKG=build_nRF52840\dongle_mcuboot_migration.zip
set APP=build_nRF52840\BLE_Password\zephyr\zephyr.signed.bin

echo.
echo ========================================
echo nRF52840 Dongle - install MCUboot (one time)
echo ========================================
echo Package: %PKG%   (MCUboot only)
echo COM Port: %COM_PORT%
echo.

if not exist "%PKG%" (
    echo ERROR: %PKG% not found - build the dongle firmware first.
    pause
    exit /b 1
)

echo Put the dongle in Nordic bootloader mode: press its RESET button
echo (or hold SW1 while plugging in). The red LED should pulse.
echo.
pause

nrfutil dfu usb-serial -pkg "%PKG%" -p %COM_PORT%
if not %ERRORLEVEL%==0 (
    echo.
    echo Flash FAILED - check the COM port and bootloader mode.
    pause
    exit /b 1
)

echo.
echo ========================================
echo Step 1 done: MCUboot installed.
echo ========================================
echo Step 2 - load the first app image through MCUboot's USB recovery:
echo   1. Unplug the dongle, then HOLD SW1 while plugging it back in
echo      (do NOT press RESET - that starts the Nordic bootloader instead).
echo      A new "MCUBOOT" COM port appears.
echo   2. Upload %APP%
echo      to that port with an SMP client, e.g.:
echo        smpmgr --port COMx upgrade %APP%
echo      (pip install smpmgr), or nrfutil mcu-manager / mcumgr.
echo   3. Replug the dongle normally - it boots the app.
echo.
pause
