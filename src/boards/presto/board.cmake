# The Pimoroni Presto (RP2350B, 8 MB PSRAM, 480x480 ST7701 panel, WiFi, SD card, piezo, seven
# LEDs on its back): what a machine's firmware needs of it. Included by the top-level
# CMakeLists.txt (-DBOARD=presto); a machine calls board_setup(<target>). See
# docs/porting-a-board.md for what a board provides.

set(PICO_BOARD pimoroni_presto CACHE STRING "Board")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")
list(APPEND PICO_BOARD_HEADER_DIRS ${BOARD_DIR})
set(BOARD_HAS_NET ON)       # WiFi and the web page (presto_net.c)
# -DBOARD_FLASH_SAVE_SHIFT=<bytes>: the save disc that much lower in the flash (board_config.h), to
# test saving without touching the real one
set(BOARD_FLASH_SAVE_SHIFT 0 CACHE STRING "Move the save disc down the flash by this much (testing)")

function(board_setup TARGET)
    target_sources(${TARGET} PRIVATE
            ${BOARD_DIR}/presto_lcd.c       # the panel: 4-bit rows, scanned out by PIO and DMA
            ${BOARD_DIR}/presto_psram.c
            ${BOARD_DIR}/presto_sd.c        # the SD card and the disc in drive 0
            ${BOARD_DIR}/fatfs/ff.c
            ${BOARD_DIR}/fatfs/ffunicode.c
            ${BOARD_DIR}/presto_net.c       # WiFi, the HTTP server for /api/<name> and the page
            )
    pico_generate_pio_header(${TARGET} ${BOARD_DIR}/st7701_parallel.pio)
    pico_generate_pio_header(${TARGET} ${BOARD_DIR}/st7701_timing.pio)
    target_include_directories(${TARGET} PRIVATE ${BOARD_DIR} ${BOARD_DIR}/fatfs)
    target_compile_definitions(${TARGET} PRIVATE
            BOARD_NAME="presto"
            BOARD_HAS_NET=1
            BOARD_FLASH_SAVE_SHIFT=${BOARD_FLASH_SAVE_SHIFT}
            CYW43_HOST_NAME="beeb")     # its DHCP name; it also answers as beeb.local (mDNS)
    target_link_libraries(${TARGET} PRIVATE
            hardware_pio hardware_dma hardware_spi hardware_pwm hardware_adc hardware_flash pico_flash
            pico_cyw43_arch_lwip_poll   # WiFi, polled from core 0 (presto_net.c)
            pico_lwip_mdns)

    # the web page (src/common/api/web), gzipped into flash
    set(WEB_C ${CMAKE_CURRENT_BINARY_DIR}/presto_web_page.c)
    set(WEB_DIR ${COMMON_DIR}/api/web)
    add_custom_command(OUTPUT ${WEB_C}
            COMMAND python3 ${TOP_DIR}/tools/embed_web.py ${WEB_DIR}/index.html ${WEB_C}
            DEPENDS ${WEB_DIR}/index.html ${WEB_DIR}/fonts/OpenGorton-Regular.otf
                    ${WEB_DIR}/fonts/OpenGorton-Bold.otf ${TOP_DIR}/tools/embed_web.py)
    target_sources(${TARGET} PRIVATE ${WEB_C})

    # -DAMBIENT=ON: the seven LEDs on the back lit with what's on screen (presto_ambient.c), on
    # core 1 when it's idle
    if (AMBIENT)
        target_sources(${TARGET} PRIVATE ${BOARD_DIR}/presto_ambient.c)
        pico_generate_pio_header(${TARGET} ${BOARD_DIR}/ws2812.pio)
        target_compile_definitions(${TARGET} PRIVATE WITH_AMBIENT=1)
    endif()
endfunction()
