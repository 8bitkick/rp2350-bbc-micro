# A BBC Master 128 on the Pimoroni Presto (RP2350): Kieran Connell's emulator core
# (src/cores/kc-beeb/), the Presto's panel, PSRAM and USB around it (src/machines/master/). See
# docs/master/README.md. Included by the top-level CMakeLists.txt with -DMACHINE=master (which has
# already set the board and initialised the SDK):
#   cmake -S . -B build -G Ninja -DMACHINE=master [-DBEEB_DISC=discs/pop-beeb.ssd] && ninja -C build
# BEEB_ROMS is the MOS 3.20 set (roms/mos3.20, fetched by scripts/setup.sh --master); BEEB_DISC is
# the disc built in and booted at power on. The firmware is build/presto_master.uf2.

set(PRESTO_DIR ${CMAKE_CURRENT_LIST_DIR})   # the Master's own files
set(MASTER_CORE_DIR ${CORES_DIR}/kc-beeb)
set(MASTER_TOP ${TOP_DIR})
set(BEEB_ROMS ${MASTER_TOP}/roms/mos3.20 CACHE PATH "MOS 3.20 ROMs")
set(BEEB_DISC ${MASTER_TOP}/discs/elite-master-flicker-free-sng47.ssd CACHE FILEPATH "the disc to build in and boot")

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
        ${PRESTO_DIR}/master_machine.c
        )
common_setup(presto_master)     # the command layer, the USB channel, the front end, the sound
board_setup(presto_master)      # the board: panel, PSRAM, SD card, network and web page, LEDs
target_include_directories(presto_master PRIVATE ${PRESTO_DIR} ${MASTER_CORE_DIR})
target_compile_options(presto_master PRIVATE -O2 -fwrapv -Wall -Wno-unused-function)
# -DSOUND=ON (common_setup): the bytes the core's SN76489 takes go to sound.c through the
# core's own hook (MACHINE_SOUND_WRITE, machine_unit.c); made on core 1. -DAMBIENT=ON
# (board_setup): the LEDs, on core 1 when it's idle (main.c).
set(VS_RING 8192 CACHE STRING "video stream ring, halfwords (a power of 2): 16 KB; 8 KB slowed Evil Influences 56 -> 51%")
target_compile_definitions(presto_master PRIVATE NDEBUG=1 VS_RING=${VS_RING}
        PRESTO_DISC_STAGE_OFFSET=0) # a disc is expanded into track data as it goes in (master_machine.c)
target_link_libraries(presto_master PRIVATE pico_stdlib pico_multicore hardware_flash pico_flash)
# -DHANG_TRAP=ON: a freeze diagnosed (src/common/hang_trap.c): each core sampled 1000 times a second, the
# watchdog fed only while both make progress; a hang or a fault resets the board and the next boot
# prints where the cores were (HANGTRAP lines, every 5 s)
option(HANG_TRAP "Report where the firmware froze, at the next boot" OFF)
if (HANG_TRAP)
    target_sources(presto_master PRIVATE ${TOP_DIR}/src/common/hang_trap.c)
    target_compile_definitions(presto_master PRIVATE HANG_TRAP=1)
endif()

# The machine and the renderer run from SRAM: the SDK's linker script keeps in flash what
# it doesn't exclude; exclude these objects (and newlib's mem* functions, which this
# toolchain names libc_a-mem*.o, missed by the SDK's pattern).
# (objects are .c.obj when CMake runs on Windows: named .c.o, they stayed in flash, at a third
# of real speed)
set(RAM_EXCLUDES " *machine_unit.c${CMAKE_C_OUTPUT_EXTENSION} *render_unit.c${CMAKE_C_OUTPUT_EXTENSION}")
file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld MEMMAP)
string(REPLACE "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*"
        "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libg.a:*libc_a-mem*.o *libc.a:*libc_a-mem*.o *libm.a:${RAM_EXCLUDES}) .text*"
        MEMMAP_NEW "${MEMMAP}")
if (MEMMAP_NEW STREQUAL MEMMAP)
    message(FATAL_ERROR "SDK linker script not as expected")
endif()
# The app RAM area (src/common/app_api.h): the top APP_RAM_SIZE of the SRAM's main 512 KB, out
# of the heap's reach (the RAM region ends below it), for the app run alongside. Not in a build
# with an app built in (APP_SOURCES): that one's RAM is the firmware's own.
if (NOT APP_SOURCES)
    file(STRINGS ${COMMON_DIR}/app_api.h APP_RAM_LINE REGEX "^#define APP_RAM_SIZE \\(([0-9]+) \\* 1024\\)")
    string(REGEX REPLACE ".*\\(([0-9]+) \\* 1024\\).*" "\\1" APP_RAM_KB "${APP_RAM_LINE}")
    math(EXPR RAM_KB "512 - ${APP_RAM_KB}")
    string(REPLACE "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k" "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = ${RAM_KB}k" MEMMAP_APP "${MEMMAP_NEW}")
    if (MEMMAP_APP STREQUAL MEMMAP_NEW)
        message(FATAL_ERROR "SDK linker script or app_api.h not as expected")
    endif()
    set(MEMMAP_NEW "${MEMMAP_APP}")
    target_compile_definitions(presto_master PRIVATE APP_RAM=1)
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_presto_master.ld "${MEMMAP_NEW}")
pico_set_linker_script(presto_master ${CMAKE_CURRENT_BINARY_DIR}/memmap_presto_master.ld)

pico_enable_stdio_uart(presto_master 0)
pico_enable_stdio_usb(presto_master 1)
pico_add_extra_outputs(presto_master)
