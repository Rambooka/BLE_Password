# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_password)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)
FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# The app is an MCUboot image (sysbuild.conf); BLE update packages come from
# sysbuild as <build>/dfu_application.zip.  The official dongle's one-time
# Nordic-bootloader migration package (MCUboot only) is made in sysbuild.cmake.
