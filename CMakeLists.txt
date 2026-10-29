cmake_minimum_required(VERSION 3.13)

# A BBC Micro on an RP2350 board: see README.md. One machine and one board per build:
#   cmake -S . -B build -G Ninja && ninja -C build
#       BBC Model B on beebjit (the default): build/presto_beebjit.uf2
#   cmake -S . -B build-master -G Ninja -DMACHINE=master && ninja -C build-master
#       BBC Master 128 on Kieran Connell's emulator: build-master/presto_master.uf2
# (scripts/setup.sh [--master] does it all, toolchain and SDK included)
#
#   src/cores/<core>/       the emulators: beebjit (ported to the RP2350), kc-beeb (Kieran Connell's)
#   src/machines/<machine>/ each machine's glue: a core on an RP2350
#   src/common/             RP2350 code shared by the machines, board-independent
#   src/boards/<board>/     a board's hardware (docs/porting-a-board.md)

# -DMACHINE=beeb|master (-DPRESTO_MACHINE=... is the same, the name it had first)
if (DEFINED PRESTO_MACHINE)
    set(MACHINE ${PRESTO_MACHINE} CACHE STRING "" FORCE)
    unset(PRESTO_MACHINE CACHE)
endif()
# MACHINE_EXTRA_SOURCES (a program built in, below) was the Master build's alone: given with no
# machine named, it means the Master, so a project that built one that way still does.
if (DEFINED MACHINE_EXTRA_SOURCES AND NOT DEFINED MACHINE)
    set(MACHINE master CACHE STRING "" FORCE)
endif()
set(MACHINE beeb CACHE STRING "The machine to build: beeb (BBC Model B, beebjit) or master (BBC Master 128)")
set_property(CACHE MACHINE PROPERTY STRINGS beeb master c64)
# Features, for either machine (each made on core 1; see docs/cores.md):
#   -DSOUND=ON    the BBC's SN76489 on the board's audio out (the Presto: its piezo)
#   -DSOUND_PIN=n with it, a PWM pin of your own (the Presto: 40, the Qw/ST socket, to a speaker)
#   -DAMBIENT=ON  the board's LEDs lit with what's on screen (the Presto: the seven on its back)
# (-DPRESTO_BEEBJIT_SOUND / -DPRESTO_BEEBJIT_AMBIENT, the Model B's first names, are the same.)
option(WEBDIAG "Show web diagnostic trace, fault details and speaker test" OFF)
foreach(F SOUND AMBIENT)
    if (DEFINED PRESTO_BEEBJIT_${F})
        set(${F} ${PRESTO_BEEBJIT_${F}} CACHE BOOL "" FORCE)
        unset(PRESTO_BEEBJIT_${F} CACHE)
    endif()
endforeach()
option(SOUND "The BBC's sound on the board's audio out" OFF)
if (DEFINED PRESTO_BEEBJIT_SOUND_PIN)
    set(SOUND_PIN ${PRESTO_BEEBJIT_SOUND_PIN} CACHE STRING "" FORCE)
    unset(PRESTO_BEEBJIT_SOUND_PIN CACHE)
endif()
set(SOUND_PIN "" CACHE STRING "The sound's PWM pin, if not the board's audio out (the Presto: 40, the Qw/ST socket)")
option(AMBIENT "The board's LEDs lit with what's on screen" OFF)
set(BOARD presto CACHE STRING "The board to build for: presto (Pimoroni Presto)")
set_property(CACHE BOARD PROPERTY STRINGS presto)
if (NOT MACHINE MATCHES "^(beeb|master|c64)$")
    message(FATAL_ERROR "MACHINE is beeb or master, not '${MACHINE}'")
endif()
if (NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/boards/${BOARD})
    message(FATAL_ERROR "no board '${BOARD}' (src/boards/)")
endif()
message(STATUS "Machine ${MACHINE}, board ${BOARD}")

set(TOP_DIR ${CMAKE_CURRENT_LIST_DIR})
set(COMMON_DIR ${TOP_DIR}/src/common)
set(BOARD_DIR ${TOP_DIR}/src/boards/${BOARD})
set(CORES_DIR ${TOP_DIR}/src/cores)
set(MACHINE_DIR ${TOP_DIR}/src/machines/${MACHINE})

include(${BOARD_DIR}/board.cmake)      # the board: PICO_BOARD, and board_setup()
include(${COMMON_DIR}/common.cmake)     # common_setup()
include(pico_sdk_import.cmake)

project(rp2350_bbc_micro C CXX ASM)
if (MACHINE STREQUAL "beeb")
    set(CMAKE_C_STANDARD 11)   # (the Master's core builds with the compiler's default)
endif()
pico_sdk_init()

# A program built into the firmware to run alongside the machine (src/common/machine_hook.h), on
# either machine: its sources, include directories and definitions (;-separated). Empty: none.
# (-DMACHINE_EXTRA_SOURCES / _INCLUDES / _DEFINES, their first names, are the same.)
foreach(F SOURCES INCLUDES DEFINES)
    if (DEFINED MACHINE_EXTRA_${F})
        set(APP_${F} "${MACHINE_EXTRA_${F}}" CACHE STRING "" FORCE)
        unset(MACHINE_EXTRA_${F} CACHE)
    endif()
endforeach()
set(APP_SOURCES "" CACHE STRING "a program's sources, built into the firmware (;-separated)")
set(APP_INCLUDES "" CACHE STRING "its include directories (;-separated)")
set(APP_DEFINES "" CACHE STRING "its compile definitions (;-separated NAME=value)")

include(${MACHINE_DIR}/machine.cmake)
