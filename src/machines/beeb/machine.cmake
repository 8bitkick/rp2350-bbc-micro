# The BBC Model B: beebjit (https://github.com/scarybeasts/beebjit, GPL v3), ported to the RP2350
# (src/cores/beebjit), on the board: presto_beebjit. Included by the top-level CMakeLists.txt
# (-DMACHINE=beeb, the default). See README.md and AGENTS.md.

set(BEEBJIT_SRC ${CORES_DIR}/beebjit)
set(BEEB_ROMS ${TOP_DIR}/roms)

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/beebjit_sources.txt BEEBJIT_SOURCES REGEX "^[^#]")
list(TRANSFORM BEEBJIT_SOURCES PREPEND ${BEEBJIT_SRC}/)

add_executable(presto_beebjit
        ${BEEBJIT_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/os_embedded.c
        ${CMAKE_CURRENT_LIST_DIR}/beebjit_embedded.c
        ${CMAKE_CURRENT_LIST_DIR}/beebjit_panel.c
        ${CMAKE_CURRENT_LIST_DIR}/beebjit_wire.c
        ${CMAKE_CURRENT_LIST_DIR}/beeb_main.c
        ${CMAKE_CURRENT_LIST_DIR}/beeb_machine.c
        ${CMAKE_CURRENT_LIST_DIR}/romfs.c
        )
common_setup(presto_beebjit)    # the command layer, the USB channel, the front end, the sound
board_setup(presto_beebjit)     # the board: panel, PSRAM, SD card, network and web page, LEDs

# Built-in files, by the names beebjit opens them with.
set(BEEBJIT_EMBED
        roms/os12.rom=${BEEB_ROMS}/os/os12.rom
        roms/basic2.rom=${BEEB_ROMS}/general/basic2.rom
        roms/dfs09.rom=${BEEB_ROMS}/general/dfs09.rom
        )
# The Graphics Extension ROM (BBC Micro Bot programs mostly assume it) isn't in the repository; if
# roms/general/gxr.rom is there, it's built in, for snapshots that had it (beeb_machine.c)
if (EXISTS ${BEEB_ROMS}/general/gxr.rom)
    list(APPEND BEEBJIT_EMBED roms/gxr.rom=${BEEB_ROMS}/general/gxr.rom)
endif()
# A disc built in, in drive 0 and booted at power on: by default flicker-free Elite, if
# scripts/fetch-elite.sh has fetched it; -DPRESTO_BEEBJIT_DISC=<image> for another, "" for none.
set(PRESTO_BEEBJIT_DEFAULT_DISC "")
if (EXISTS ${TOP_DIR}/discs/elite-disc-flicker-free-sth.ssd)
    set(PRESTO_BEEBJIT_DEFAULT_DISC discs/elite-disc-flicker-free-sth.ssd)
endif()
set(PRESTO_BEEBJIT_DISC "${PRESTO_BEEBJIT_DEFAULT_DISC}" CACHE STRING "Disc image to build in and boot, relative to the repository (empty: BASIC prompt)")
if (PRESTO_BEEBJIT_DISC)
    get_filename_component(DISC_PATH ${PRESTO_BEEBJIT_DISC} ABSOLUTE BASE_DIR ${TOP_DIR})
    get_filename_component(DISC_NAME ${DISC_PATH} NAME)
    list(APPEND BEEBJIT_EMBED discs/${DISC_NAME}=${DISC_PATH})
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_DISC="discs/${DISC_NAME}")
endif()
# beebjit's JIT (the thumb2 backend, asm/thumb2), its code in the bottom 4MB of the PSRAM;
# -DPRESTO_BEEBJIT_JIT=OFF for its interpreter instead.
option(PRESTO_BEEBJIT_JIT "Run beebjit's JIT" ON)
if (PRESTO_BEEBJIT_JIT)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_JIT=1)
endif()
# -DPRESTO_BEEBJIT_FAST=ON: beebjit's fast mode (it stops drawing frames).
if (PRESTO_BEEBJIT_FAST)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_FAST=1)
endif()
# -DPRESTO_BEEBJIT_TURBO=ON: start with no throttling, every frame drawn (as the web panel's
# turbo, to compare top speeds).
if (PRESTO_BEEBJIT_TURBO)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_TURBO=1)
endif()
# Drawing on core 1, fed by video through the render wire (beebjit_wire.h);
# -DPRESTO_BEEBJIT_WIRE=OFF to draw on core 0.
option(PRESTO_BEEBJIT_WIRE "Draw on core 1" ON)
if (PRESTO_BEEBJIT_WIRE)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_WIRE=1 BEEBJIT_RENDER_WIRE=1)
endif()
# -DSOUND=ON (common_setup): beebjit's sound.c hands each SN76489 byte to sound.c; made on
# core 1. -DAMBIENT=ON (board_setup): the LEDs, on core 1 too.
if ((SOUND OR AMBIENT) AND NOT PRESTO_BEEBJIT_WIRE)
    message(FATAL_ERROR "SOUND and AMBIENT need core 1 (PRESTO_BEEBJIT_WIRE)")
endif()
if (SOUND)
    target_compile_definitions(presto_beebjit PRIVATE BEEBJIT_SOUND_WRITE_HOOK=audio_sn_write)
endif()
# -DPRESTO_BEEBJIT_TERMINAL=ON: the USB serial port is the BBC's RS423 (no "!" commands)
if (PRESTO_BEEBJIT_TERMINAL)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_TERMINAL=1 FRONTEND_NO_USB_COMMANDS=1)
endif()
# -DPRESTO_BEEBJIT_FRAME_SKIP=OFF: draw every frame, even when the picture can't have changed.
option(PRESTO_BEEBJIT_FRAME_SKIP "Skip drawing frames that can't have changed" ON)
if (PRESTO_BEEBJIT_FRAME_SKIP)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_FRAME_SKIP=1)
endif()
# -DPRESTO_BEEBJIT_PANEL_PIO=OFF: 640 -> 480 on the CPU instead of on PIO0.
option(PRESTO_BEEBJIT_PANEL_PIO "Scale rows to the panel on PIO0" ON)
if (PRESTO_BEEBJIT_PANEL_PIO)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_PANEL_PIO=1)
endif()
# -DPRESTO_BEEBJIT_PROF=ON: sample core 0 and print the samples (beeb_main.c)
if (PRESTO_BEEBJIT_PROF)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_PROF=1)
    # -DPRESTO_BEEBJIT_PROF_CORE=1: core 1 (the drawing) instead
    if (PRESTO_BEEBJIT_PROF_CORE)
        target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_PROF_CORE=1)
    endif()
endif()
set(BEEBJIT_EMBED_DEPS "")
foreach(ENTRY ${BEEBJIT_EMBED})
    string(REGEX REPLACE "^[^=]*=" "" FILE_PATH ${ENTRY})
    list(APPEND BEEBJIT_EMBED_DEPS ${FILE_PATH})
endforeach()
string(REPLACE ";" "|" BEEBJIT_EMBED_ARG "${BEEBJIT_EMBED}")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/presto_beebjit_romfs.c
        COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/presto_beebjit_romfs.c
                "-DFILES=${BEEBJIT_EMBED_ARG}" -P ${CMAKE_CURRENT_LIST_DIR}/embed_files.cmake
        DEPENDS ${BEEBJIT_EMBED_DEPS} ${CMAKE_CURRENT_LIST_DIR}/embed_files.cmake
        VERBATIM)
target_sources(presto_beebjit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/presto_beebjit_romfs.c)

target_include_directories(presto_beebjit PRIVATE
        ${BEEBJIT_SRC}
        ${CMAKE_CURRENT_LIST_DIR}
        )

target_compile_definitions(presto_beebjit PRIVATE
        BEEBJIT_NO_MMU=1
        # Set-up code tagged BEEBJIT_COLD (util.h) stays in flash (see the linker script below).
        BEEBJIT_COLD_SECTION=1
        # The thumb2 JIT backend, and the smaller data structures it needs to fit
        # (used only in JIT mode; harmless otherwise).
        BEEBJIT_ASM_THUMB2=1
        BEEBJIT_JIT_COMPACT=1
        BEEBJIT_DISC_COMPACT=1
        BEEBJIT_MALLOC_BIG=beebjit_embedded_malloc_big
        BOOT_GUARD=1 BOOT_GUARD_KEEP=$<BOOL:${BOOT_GUARD_KEEP}>     # a build that dies at boot reports why over USB (boot_guard.c)
        PICO_PANIC_FUNCTION=boot_guard_panic   # an SDK panic (out of memory, hard_assert) is reported the same way
        BEEBJIT_HEADLESS=1
        BEEBJIT_MALLOC=beebjit_embedded_malloc
        BEEBJIT_REALLOC=beebjit_embedded_realloc
        BEEBJIT_FREE=beebjit_embedded_free
        BEEBJIT_RENDER_PACKED4=1
        BEEBJIT_INTERP_COUNTDOWN32=1
        BEEBJIT_VIDEO_FAST_RUNS=1
        NDEBUG=1
        PRESTO_SYS_CLOCK_KHZ=250000
        PRESTO_BEEBJIT=1
        PRESTO_DISC_STAGE_OFFSET=0  # beebjit copies a disc in as it's inserted
        )

# beebjit is written for 64-bit hosts: quieten what is harmless with a 32-bit long / int32_t.
set_source_files_properties(${BEEBJIT_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-Wno-format;-Wno-char-subscripts;-Wno-address-of-packed-member;-Wno-pointer-to-int-cast;-Wno-int-to-pointer-cast;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable")
target_compile_options(presto_beebjit PRIVATE -O2 -Wall)

target_link_libraries(presto_beebjit PRIVATE pico_stdlib pico_multicore)

# Hot code runs from SRAM: the XIP cache is only 16K and the interpreter alone is ~60K of
# Thumb. The SDK's linker script keeps in flash only what it doesn't exclude; exclude these.
set(PRESTO_BEEBJIT_RAM_OBJECTS "interp.c.o;timing.c.o;render.c.o;state_6502.c.o;beebjit_panel.c.o;video.c.o;teletext.c.o;beebjit_wire.c.o;bbc.c.o;jit.c.o;jit_compiler.c.o;jit_metadata.c.o;intel_fdc.c.o;disc_drive.c.o;via.c.o;cpu_driver.c.o;ibm_disc_format.c.o"
        CACHE STRING "beebjit object files whose code is copied to SRAM at boot")
set(RAM_EXCLUDES "")
foreach(OBJ ${PRESTO_BEEBJIT_RAM_OBJECTS})
    # objects are .c.obj when CMake runs on Windows
    string(REGEX REPLACE "\\.o$" "${CMAKE_C_OUTPUT_EXTENSION}" OBJ "${OBJ}")
    string(APPEND RAM_EXCLUDES " *${OBJ}")
endforeach()
file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld MEMMAP)
# newlib's memcpy / memset / memcmp / memmove are meant to be excluded too, but this toolchain
# names them libg.a(libc_a-mem*.o), which the SDK's pattern misses: they ran from flash. And
# set-up code in the RAM objects (BEEBJIT_COLD, section .flashtext) stays in flash.
string(REPLACE "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*"
        "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libg.a:*libc_a-mem*.o *libc.a:*libc_a-mem*.o *libm.a:${RAM_EXCLUDES}) .text*)\n        *(.flashtext*" MEMMAP_NEW "${MEMMAP}")
if (MEMMAP_NEW STREQUAL MEMMAP)
    message(FATAL_ERROR "presto_beebjit: SDK linker script not as expected")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_presto_beebjit.ld "${MEMMAP_NEW}")
pico_set_linker_script(presto_beebjit ${CMAKE_CURRENT_BINARY_DIR}/memmap_presto_beebjit.ld)

pico_enable_stdio_uart(presto_beebjit 0)
pico_enable_stdio_usb(presto_beebjit 1)
pico_add_extra_outputs(presto_beebjit)
