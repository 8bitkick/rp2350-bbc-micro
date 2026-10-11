cmake_minimum_required(VERSION 3.13)

# beebjit for the Pimoroni Presto (RP2350): see README.md.
#   cmake -S . -B build -G Ninja && ninja -C build presto_beebjit
# (scripts/setup.sh does it all, toolchain and SDK included)
set(PICO_BOARD pimoroni_presto CACHE STRING "Board")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/pico/boards)
include(pico_sdk_import.cmake)

project(rp2350_beebjit C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_subdirectory(src/pico/beebjit)
