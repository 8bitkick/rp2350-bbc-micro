cmake_minimum_required(VERSION 3.13)

# The launcher (docs/dual-boot.md): the small firmware at the bottom of the flash that boots the
# machine chosen, with the partition table in it. Built on its own, by scripts/build-dual-boot.sh:
#   cmake -S src/launcher -B build-launcher -G Ninja && ninja -C build-launcher
# -> presto_launcher.uf2, joined with the two machines by tools/dual_boot_uf2.py.

set(TOP_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
set(BOARD presto CACHE STRING "The board to build for: presto (Pimoroni Presto)")
set(BOARD_DIR ${TOP_DIR}/src/boards/${BOARD})
include(${BOARD_DIR}/board.cmake)      # PICO_BOARD and its header
include(${TOP_DIR}/pico_sdk_import.cmake)

project(presto_launcher C CXX ASM)
pico_sdk_init()

add_executable(presto_launcher launcher.c)
target_include_directories(presto_launcher PRIVATE ${TOP_DIR}/src/common ${BOARD_DIR})
target_link_libraries(presto_launcher PRIVATE pico_stdlib pico_bootrom hardware_flash hardware_watchdog)
# its RAM at the top of SRAM, clear of what a machine keeps across a reset
pico_set_linker_script(presto_launcher ${CMAKE_CURRENT_LIST_DIR}/memmap_launcher.ld)
# the partition table, in its block loop
pico_embed_pt_in_binary(presto_launcher ${CMAKE_CURRENT_LIST_DIR}/partitions.json)
pico_add_extra_outputs(presto_launcher)
