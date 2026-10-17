# A BBC Master 128 on the Pimoroni Presto (RP2350): Kieran Connell's emulator core (core/),
# the Presto's panel, PSRAM and USB around it (presto/). scripts/setup.sh fetches everything and
# builds; by hand, after ". deps/env.sh":
#   cmake -S . -B build -G Ninja -DBEEB_DISC=discs/pop-beeb.ssd && ninja -C build
# BEEB_ROMS is the MOS 3.20 set (roms/mos3.20, fetched by setup.sh); BEEB_DISC is the disc built
# in and booted at power on.
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pimoroni_presto)
set(PICO_PLATFORM rp2350-arm-s)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(presto_master C CXX ASM)
pico_sdk_init()

set(PRESTO_DIR ${CMAKE_CURRENT_LIST_DIR}/presto)
set(BEEB_ROMS ${CMAKE_CURRENT_LIST_DIR}/roms/mos3.20 CACHE PATH "MOS 3.20 ROMs")
set(BEEB_DISC ${CMAKE_CURRENT_LIST_DIR}/discs/elite-master-flicker-free-sng47.ssd CACHE FILEPATH "the disc to build in and boot")

set(EMBED_C ${CMAKE_CURRENT_BINARY_DIR}/embedded.c)
set(EMBED_ARGS)
foreach(R mos dfs viewsht edit basic4 adfs view terminal)
    list(APPEND EMBED_ARGS rom_${R}=${BEEB_ROMS}/${R}.rom)
endforeach()
list(APPEND EMBED_ARGS disc_image=${BEEB_DISC})
add_custom_command(OUTPUT ${EMBED_C}
        COMMAND python3 ${PRESTO_DIR}/embed.py ${EMBED_C} ${EMBED_ARGS}
        DEPENDS ${PRESTO_DIR}/embed.py ${BEEB_DISC})

add_executable(presto_master
        ${PRESTO_DIR}/main.c
        ${PRESTO_DIR}/machine_unit.c
        ${PRESTO_DIR}/render_unit.c
        ${EMBED_C}
        ${PRESTO_DIR}/presto_lcd.c
        ${PRESTO_DIR}/presto_psram.c
        ${PRESTO_DIR}/presto_machine.c
        ${PRESTO_DIR}/presto_net.c
        ${PRESTO_DIR}/presto_sd.c
        ${PRESTO_DIR}/fatfs/ff.c
        ${PRESTO_DIR}/fatfs/ffunicode.c
        )
# The web panel's page, gzipped into flash.
set(WEB_C ${CMAKE_CURRENT_BINARY_DIR}/presto_web_page.c)
add_custom_command(OUTPUT ${WEB_C}
        COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/tools/embed_web.py ${PRESTO_DIR}/web/index.html ${WEB_C}
        DEPENDS ${PRESTO_DIR}/web/index.html ${PRESTO_DIR}/web/fonts/OpenGorton-Regular.otf
                ${PRESTO_DIR}/web/fonts/OpenGorton-Bold.otf ${CMAKE_CURRENT_LIST_DIR}/tools/embed_web.py)
target_sources(presto_master PRIVATE ${WEB_C})
pico_generate_pio_header(presto_master ${PRESTO_DIR}/st7701_parallel.pio)
pico_generate_pio_header(presto_master ${PRESTO_DIR}/st7701_timing.pio)
target_include_directories(presto_master PRIVATE ${PRESTO_DIR} ${PRESTO_DIR}/fatfs ${CMAKE_CURRENT_LIST_DIR}/core)
target_compile_options(presto_master PRIVATE -O2 -fwrapv -Wall -Wno-unused-function)
# A program built in to run alongside the machine (e.g. a player that reads its RAM and presses
# keys, through core/machine.h's machine_frame_hook and machine_command_hook): its sources and
# include directories, given by the project that builds it. Empty: none.
set(MACHINE_EXTRA_SOURCES "" CACHE STRING "extra sources built into the firmware (;-separated)")
set(MACHINE_EXTRA_INCLUDES "" CACHE STRING "their include directories (;-separated)")
set(MACHINE_EXTRA_DEFINES "" CACHE STRING "their compile definitions (;-separated NAME=value)")
if (MACHINE_EXTRA_SOURCES)
    target_sources(presto_master PRIVATE ${MACHINE_EXTRA_SOURCES})
    target_include_directories(presto_master PRIVATE ${MACHINE_EXTRA_INCLUDES})
    target_compile_definitions(presto_master PRIVATE ${MACHINE_EXTRA_DEFINES})
endif()
set(VS_RING 8192 CACHE STRING "video stream ring, halfwords (a power of 2): 16 KB; 8 KB slowed Evil Influences 56 -> 51%")
target_compile_definitions(presto_master PRIVATE NDEBUG=1 VS_RING=${VS_RING}
        PRESTO_DISC_STAGE_OFFSET=0  # a disc is expanded into track data as it goes in (presto_machine.c)
        CYW43_HOST_NAME="beeb")     # its DHCP name; it also answers as beeb.local (mDNS)
target_link_libraries(presto_master PRIVATE pico_stdlib pico_multicore hardware_pio hardware_dma hardware_pwm hardware_spi
        hardware_adc hardware_flash pico_flash
        pico_cyw43_arch_lwip_poll   # WiFi, polled from core 0 (presto_net.c)
        pico_lwip_mdns)

# The machine and the renderer run from SRAM: the SDK's linker script keeps in flash what
# it doesn't exclude; exclude these objects (and newlib's mem* functions, which this
# toolchain names libc_a-mem*.o, missed by the SDK's pattern).
file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld MEMMAP)
string(REPLACE "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*"
        "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libg.a:*libc_a-mem*.o *libc.a:*libc_a-mem*.o *libm.a: *machine_unit.c.o *render_unit.c.o) .text*"
        MEMMAP_NEW "${MEMMAP}")
if (MEMMAP_NEW STREQUAL MEMMAP)
    message(FATAL_ERROR "SDK linker script not as expected")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_presto_master.ld "${MEMMAP_NEW}")
pico_set_linker_script(presto_master ${CMAKE_CURRENT_BINARY_DIR}/memmap_presto_master.ld)

pico_enable_stdio_uart(presto_master 0)
pico_enable_stdio_usb(presto_master 1)
pico_add_extra_outputs(presto_master)
