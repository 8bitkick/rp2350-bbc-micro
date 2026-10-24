# The code every machine shares, on any RP2350 board (it uses the RP2350's cores, DMA, ADC and
# flash, but no board's hardware directly): the command layer (api/), the USB channel and the
# front end (frontend.c), drive 0 and the save disc (drive.c), the flash (mem.c), the boot
# guard, the sound. A machine calls common_setup(<target>).

function(common_setup TARGET)
    target_sources(${TARGET} PRIVATE
            ${COMMON_DIR}/api/api.c
            ${COMMON_DIR}/frontend.c
            ${COMMON_DIR}/drive.c           # the disc in drive 0, the save disc
            ${COMMON_DIR}/mem.c             # the flash, by the board's layout
            ${COMMON_DIR}/system.c          # both machines in one flash: which boots (docs/dual-boot.md)
            ${COMMON_DIR}/boot_guard.c
            ${COMMON_DIR}/app.c             # a program built in alongside (machine_hook.h)
            ${COMMON_DIR}/splash/splash_default.c   # the loading screen's picture, unless the app has one
            )
    target_include_directories(${TARGET} PRIVATE ${COMMON_DIR} ${COMMON_DIR}/api)
    # An allocation that fails comes back NULL, for the caller to deal with (a web reply dropped, an install
    # refused), not the SDK's panic: that stopped the board for good, USB still there but silent (the freezes:
    # the heap run out by the web server's replies and a plugin install's buffers)
    target_compile_definitions(${TARGET} PRIVATE PICO_MALLOC_PANIC=0)
    target_link_libraries(${TARGET} PRIVATE pico_stdlib pico_multicore hardware_adc hardware_flash pico_flash)
    # -DSOUND=ON: the BBC's SN76489, made on core 1 (sound.c), from the bytes the machine
    # hands it (each machine wires its core's sound writes to audio_sn_write)
    if (SOUND)
        target_sources(${TARGET} PRIVATE ${COMMON_DIR}/sound.c)
        target_compile_definitions(${TARGET} PRIVATE WITH_SOUND=1)
        if (NOT SOUND_PIN STREQUAL "")
            target_compile_definitions(${TARGET} PRIVATE BOARD_AUDIO_PWM_PIN=${SOUND_PIN})
        endif()
    endif()
    # -DAPP_SOURCES=... (and _INCLUDES, _DEFINES): a program built in to run alongside the machine
    if (APP_SOURCES)
        target_sources(${TARGET} PRIVATE ${APP_SOURCES})
        target_include_directories(${TARGET} PRIVATE ${APP_INCLUDES})
        target_compile_definitions(${TARGET} PRIVATE ${APP_DEFINES})
    endif()
endfunction()
