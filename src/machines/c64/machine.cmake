# FRANK/Frodo line-based PAL C64 on Presto. No HDMI, PS/2 or foreign SD drivers.
# Fetch the pinned fork and checksum-verified Elite before configuring sources.
execute_process(COMMAND python3 ${TOP_DIR}/scripts/fetch-c64.py RESULT_VARIABLE C64_FETCH_RESULT)
if(NOT C64_FETCH_RESULT EQUAL 0)
    message(FATAL_ERROR "C64 dependencies could not be prepared; see scripts/fetch-c64.py output")
endif()
set(FRANK ${TOP_DIR}/deps/frank-c64/src)

add_executable(presto_c64 main.cpp display.cpp memory.cpp disk_file.c
    ${FRANK}/rp2350/C64_rp2350.cpp ${FRANK}/rp2350/Prefs_rp2350.cpp
    ${FRANK}/rp2350/ROM_data.cpp ${FRANK}/rp2350/SID_rp2350.cpp
    ${FRANK}/rp2350/Tape_stub.cpp ${FRANK}/CPUC64.cpp ${FRANK}/VIC.cpp
    ${FRANK}/CIA.cpp ${FRANK}/IEC.cpp ${FRANK}/Cartridge.cpp ${FRANK}/REU.cpp
    ${FRANK}/1541d64.cpp ${FRANK}/1541gcr.cpp ${FRANK}/CPU1541.cpp ${FRANK}/VIA.cpp)
# Relative sources above are relative to the top-level source directory.
get_target_property(_src presto_c64 SOURCES)
list(TRANSFORM _src PREPEND "${MACHINE_DIR}/" REGEX "^[^/].*")
set_property(TARGET presto_c64 PROPERTY SOURCES "${_src}")
common_setup(presto_c64)
board_setup(presto_c64)
target_include_directories(presto_c64 PRIVATE ${MACHINE_DIR} ${FRANK} ${FRANK}/rp2350)
target_compile_definitions(presto_c64 PRIVATE MACHINE_C64=1 FRODO_RP2350=1
    PSRAM_MAX_FREQ_MHZ=125 ENABLE_DEBUG_LOGS=0 PRESTO_DISC_STAGE_OFFSET=0
    $<$<COMPILE_LANGUAGE:CXX>:PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1>)
target_compile_options(presto_c64 PRIVATE -O2 -fwrapv -Wall
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
set_target_properties(presto_c64 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_link_libraries(presto_c64 PRIVATE pico_stdlib pico_multicore pico_flash hardware_watchdog)
pico_enable_stdio_usb(presto_c64 1)
pico_enable_stdio_uart(presto_c64 0)
pico_add_extra_outputs(presto_c64)

# IEC/D81 helpers have multi-kilobyte local structures. Reserve a 16 KiB core-0
# stack from main SRAM instead of the SDK's 2 KiB scratch-bank default.
file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld C64_LINKER)
if(NOT C64_LINKER MATCHES "ORIGIN =  0x20000000, LENGTH = 512k" OR NOT C64_LINKER MATCHES "ORIGIN = 0x20081000, LENGTH = 4k")
    message(FATAL_ERROR "C64 stack layout needs updating for this SDK linker script")
endif()
string(REPLACE "ORIGIN =  0x20000000, LENGTH = 512k" "ORIGIN =  0x20000000, LENGTH = 496k" C64_LINKER "${C64_LINKER}")
string(REPLACE "ORIGIN = 0x20081000, LENGTH = 4k" "ORIGIN = 0x2007c000, LENGTH = 16k" C64_LINKER "${C64_LINKER}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_c64.ld "${C64_LINKER}")
pico_set_linker_script(presto_c64 ${CMAKE_CURRENT_BINARY_DIR}/memmap_c64.ld)
target_compile_definitions(presto_c64 PRIVATE $<$<COMPILE_LANGUAGE:ASM>:PICO_STACK_SIZE=0x4000>)

set(C64_DISC ${TOP_DIR}/discs/elite-commodore-64-gma86-pal.d64 CACHE FILEPATH "Default C64 disk")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/c64_disc.c
    COMMAND python3 ${TOP_DIR}/src/machines/master/embed.py ${CMAKE_CURRENT_BINARY_DIR}/c64_disc.c c64_default_disc=${C64_DISC}
    DEPENDS ${C64_DISC} ${TOP_DIR}/src/machines/master/embed.py)
target_sources(presto_c64 PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/c64_disc.c)
