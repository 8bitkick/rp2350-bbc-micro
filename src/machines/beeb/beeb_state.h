#ifndef PRESTO_STATE_H
#define PRESTO_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

// "!state <size>": receive a BBC Micro Bot snapshot over USB (see presto_state.c)
void presto_state_receive(const char *args);
// load a received snapshot; call between CPU slices
void presto_state_apply_pending(void);
// back to our own ROM sockets (BASIC 15, DFS 14, no GXR), for running discs; takes effect at reset
void presto_rom_layout_default(void);

#ifdef __cplusplus
}
#endif

#endif
