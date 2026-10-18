cmake_minimum_required(VERSION 3.13)

# A BBC Micro for the Pimoroni Presto (RP2350): see README.md. One machine per build, chosen here:
#   cmake -S . -B build -G Ninja && ninja -C build presto_beebjit
#       BBC Model B on beebjit (the default): build/src/pico/beebjit/presto_beebjit.uf2
#   cmake -S . -B build-master -G Ninja -DPRESTO_MACHINE=master && ninja -C build-master
#       BBC Master 128 on Kieran Connell's emulator: build-master/presto_master.uf2
#       (docs/master/README.md)
# (scripts/setup.sh [--master] does it all, toolchain and SDK included)
set(PRESTO_MACHINE beeb CACHE STRING "The machine to build: beeb (BBC Model B, beebjit) or master (BBC Master 128)")
set_property(CACHE PRESTO_MACHINE PROPERTY STRINGS beeb master)

set(PICO_BOARD pimoroni_presto CACHE STRING "Board")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/pico/boards)
include(pico_sdk_import.cmake)

project(rp2350_beebjit C CXX ASM)
if (PRESTO_MACHINE STREQUAL "beeb")
    set(CMAKE_C_STANDARD 11)   # (the Master's core builds with the compiler's default)
endif()
pico_sdk_init()

# A program built into the firmware to run alongside the machine (src/master/machine_hook.h; the
# Master build only, for now): its sources, include directories and definitions. Empty: none.
set(MACHINE_EXTRA_SOURCES "" CACHE STRING "extra sources built into the firmware (;-separated)")
set(MACHINE_EXTRA_INCLUDES "" CACHE STRING "their include directories (;-separated)")
set(MACHINE_EXTRA_DEFINES "" CACHE STRING "their compile definitions (;-separated NAME=value)")

if (PRESTO_MACHINE STREQUAL "beeb")
    if (MACHINE_EXTRA_SOURCES)
        message(FATAL_ERROR "MACHINE_EXTRA_SOURCES (a program alongside the machine) is the Master build's: add -DPRESTO_MACHINE=master")
    endif()
    add_subdirectory(src/pico/beebjit)
elseif (PRESTO_MACHINE STREQUAL "master")
    include(src/pico/master/master.cmake)
else()
    message(FATAL_ERROR "PRESTO_MACHINE is beeb or master, not '${PRESTO_MACHINE}'")
endif()
