# The BBC Model B: beebjit (https://github.com/scarybeasts/beebjit, GPL v3), ported to the RP2350
# (src/cores/beebjit), on the board: presto_beebjit. Included by the top-level CMakeLists.txt
# (-DMACHINE=beeb, the default). See docs/presto/BEEBJIT_PORT.md.

set(BEEBJIT_SRC ${CORES_DIR}/beebjit)
set(PRESTO_DIR ${BOARD_DIR})
set(BEEB_ROMS ${TOP_DIR}/roms)

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/beebjit_sources.txt BEEBJIT_SOURCES REGEX "^[^#]")
list(TRANSFORM BEEBJIT_SOURCES PREPEND ${BEEBJIT_SRC}/)

add_executable(presto_beebjit
        ${BEEBJIT_SOURCES}
        ${CMAKE_CURRENT_LIST_DIR}/os_embedded.c
        ${CMAKE_CURRENT_LIST_DIR}/beebjit_embedded.c
        ${CMAKE_CURRENT_LIST_DIR}/beebjit_panel.c
        ${CMAKE_CURRENT_LIST_DIR}/beebjit_wire.c
        ${CMAKE_CURRENT_LIST_DIR}/presto_beebjit.c
        ${CMAKE_CURRENT_LIST_DIR}/presto_machine.c
        ${CMAKE_CURRENT_LIST_DIR}/romfs.c
        ${PRESTO_DIR}/presto_lcd.c
        ${PRESTO_DIR}/presto_psram.c
        ${COMMON_DIR}/presto_boot.c
        # the front end: the web panel, the SD card
        ${PRESTO_DIR}/presto_net.c
        ${PRESTO_DIR}/presto_sd.c
        ${PRESTO_DIR}/fatfs/ff.c
        ${PRESTO_DIR}/fatfs/ffunicode.c
        )

# the web control panel's page, gzipped into flash
set(PRESTO_BEEBJIT_WEB_C ${CMAKE_CURRENT_BINARY_DIR}/presto_beebjit_web_page.c)
add_custom_command(OUTPUT ${PRESTO_BEEBJIT_WEB_C}
        DEPENDS ${PRESTO_DIR}/web/index.html ${TOP_DIR}/tools/embed_web.py
        COMMAND python3 ${TOP_DIR}/tools/embed_web.py ${PRESTO_DIR}/web/index.html ${PRESTO_BEEBJIT_WEB_C})
target_sources(presto_beebjit PRIVATE ${PRESTO_BEEBJIT_WEB_C})

# Built-in files, by the names beebjit opens them with.
set(BEEBJIT_EMBED
        roms/os12.rom=${BEEB_ROMS}/os/os12.rom
        roms/basic2.rom=${BEEB_ROMS}/general/basic2.rom
        roms/dfs09.rom=${BEEB_ROMS}/general/dfs09.rom
        )
# The Graphics Extension ROM (BBC Micro Bot programs mostly assume it) isn't in the repository; if
# roms/general/gxr.rom is there, it's built in, for snapshots that had it (presto_machine.c)
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
# -DPRESTO_BEEBJIT_SOUND=ON: the BBC's sound on the piezo (presto_audio.c), made on core 1.
option(PRESTO_BEEBJIT_SOUND "The BBC's sound on the piezo" OFF)
if (PRESTO_BEEBJIT_SOUND)
    if (NOT PRESTO_BEEBJIT_WIRE)
        message(FATAL_ERROR "PRESTO_BEEBJIT_SOUND needs core 1 (PRESTO_BEEBJIT_WIRE)")
    endif()
    target_sources(presto_beebjit PRIVATE ${COMMON_DIR}/presto_audio.c)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_SOUND=1
            BEEBJIT_SOUND_WRITE_HOOK=presto_audio_sn_write)
endif()
# -DPRESTO_BEEBJIT_AMBIENT=ON: the LEDs on the back lit with what's on screen (presto_ambient.c),
# on core 1.
option(PRESTO_BEEBJIT_AMBIENT "Light the LEDs on the back with what's on screen" OFF)
if (PRESTO_BEEBJIT_AMBIENT)
    if (NOT PRESTO_BEEBJIT_WIRE)
        message(FATAL_ERROR "PRESTO_BEEBJIT_AMBIENT needs core 1 (PRESTO_BEEBJIT_WIRE)")
    endif()
    target_sources(presto_beebjit PRIVATE ${PRESTO_DIR}/presto_ambient.c)
    pico_generate_pio_header(presto_beebjit ${PRESTO_DIR}/ws2812.pio)
    target_compile_definitions(presto_beebjit PRIVATE PRESTO_BEEBJIT_AMBIENT=1)
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
# -DPRESTO_BEEBJIT_PROF=ON: sample core 0 and print the samples (presto_beebjit.c)
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

pico_generate_pio_header(presto_beebjit ${PRESTO_DIR}/st7701_parallel.pio)
pico_generate_pio_header(presto_beebjit ${PRESTO_DIR}/st7701_timing.pio)

target_include_directories(presto_beebjit PRIVATE
        ${BEEBJIT_SRC}
        ${CMAKE_CURRENT_LIST_DIR}
        ${COMMON_DIR}
        ${PRESTO_DIR}
        ${PRESTO_DIR}/fatfs
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
        PRESTO_BOOT_GUARD=1 PRESTO_BOOT_GUARD_KEEP=$<BOOL:${PRESTO_BOOT_GUARD_KEEP}>     # a build that dies at boot reports why over USB (presto_boot.c)
        PICO_PANIC_FUNCTION=presto_boot_panic   # an SDK panic (out of memory, hard_assert) is reported the same way
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
        CYW43_HOST_NAME="beeb"      # its DHCP name; it also answers as beeb.local (mDNS)
        )

# beebjit is written for 64-bit hosts: quieten what is harmless with a 32-bit long / int32_t.
set_source_files_properties(${BEEBJIT_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-Wno-format;-Wno-char-subscripts;-Wno-address-of-packed-member;-Wno-pointer-to-int-cast;-Wno-int-to-pointer-cast;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable")
target_compile_options(presto_beebjit PRIVATE -O2 -Wall)

target_link_libraries(presto_beebjit PRIVATE
        pico_stdlib
        hardware_pio
        hardware_dma
        hardware_spi
        hardware_pwm
        hardware_adc
        hardware_flash
        pico_flash
        pico_multicore
        pico_cyw43_arch_lwip_poll   # WiFi, polled from core 0 (see presto_net.c)
        pico_lwip_mdns
        )

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
