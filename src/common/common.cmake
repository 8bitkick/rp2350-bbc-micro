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
            ${COMMON_DIR}/presto_boot.c
            ${COMMON_DIR}/app.c             # a program built in alongside (machine_hook.h)
            )
    target_include_directories(${TARGET} PRIVATE ${COMMON_DIR} ${COMMON_DIR}/api)
    target_link_libraries(${TARGET} PRIVATE pico_stdlib pico_multicore hardware_adc hardware_flash pico_flash)
    # -DSOUND=ON: the BBC's SN76489, made on core 1 (presto_audio.c), from the bytes the machine
    # hands it (each machine wires its core's sound writes to presto_audio_sn_write)
    if (SOUND)
        target_sources(${TARGET} PRIVATE ${COMMON_DIR}/presto_audio.c)
        target_compile_definitions(${TARGET} PRIVATE WITH_SOUND=1)
    endif()
    # -DAPP_SOURCES=... (and _INCLUDES, _DEFINES): a program built in to run alongside the machine
    if (APP_SOURCES)
        target_sources(${TARGET} PRIVATE ${APP_SOURCES})
        target_include_directories(${TARGET} PRIVATE ${APP_INCLUDES})
        target_compile_definitions(${TARGET} PRIVATE ${APP_DEFINES})
    endif()
endfunction()
