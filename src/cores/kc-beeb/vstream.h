// esp32-beeb: the video stream between the machine (core 1) and the
// renderer (core 0, or the host frame tool).
//
// The CRTC and video ULA side of the machine produces one 16-bit word per
// 2 MHz tick plus tagged events, in the order beebjit's video code calls its
// renderer, so a renderer that follows beebjit's beam model produces the same
// pixels. Words:
//
//   tick:  bit 7 clear. bits 8-15 data byte, bits 0-6 flags:
//          VS_ODD     the second half of a 1 MHz character
//          VS_DISPEN  display enable after the R8 skew
//          VS_MA13    MA13 was set for this fetch (teletext addressing)
//          VS_PAIR    both ticks of a 1 MHz character in one word (VS_ODD
//                     clear): the CRTC's fast path emits these when no
//                     event falls between the two ticks, which halves the
//                     stream in modes 4 to 7. A consumer treats it as the
//                     even tick followed by the odd one.
//   event: bit 7 set. bits 0-6 type, bits 8-15 payload:
//          VS_HSYNC   payload = hsync pulse width in ticks
//          VS_VSYNC   payload = new level (1 raise, 0 lower)
//          VS_RA      payload = row address | (ISV << 7)
//          VS_CURSOR  cursor starts at the next tick
//          VS_ULACTRL payload = video ULA control register
//          VS_ULAPAL  payload = video ULA palette write byte
//          VS_FRAME   frame boundary marker from the CRTC (debug aid)
//          VS_FRAMING a framing change (CRTC R0-R9 write, ULA clock or
//                     teletext bit change); beebjit clears its canvas at
//                     the next flyback after one, and the host renderer
//                     copies that for the frame diff
//          VS_SKIP    payload = a run of 1 to 255 ticks with DISPEN low,
//                     from the CRTC's fast path in place of that many
//                     tick words (a third of a line is blanking): the
//                     beam moves, pixels inside the window are black,
//                     the bytes are not needed (the teletext pipeline
//                     forces them and decodes nothing)
//
// The producer writes into a ring of VS_RING words; the consumer reads at
// its own pace. On the board the ring is in internal SRAM and the consumer
// runs on the other core; on the host both run in one thread.

#ifndef VSTREAM_H
#define VSTREAM_H

#include <stdint.h>

enum {
    VS_ODD = 0x01,
    VS_DISPEN = 0x02,
    VS_MA13 = 0x04,
    VS_PAIR = 0x08,
};

enum {
    VS_EVENT = 0x80,
    VS_HSYNC = 0x81,
    VS_VSYNC = 0x82,
    VS_RA = 0x83,
    VS_CURSOR = 0x84,
    VS_ULACTRL = 0x85,
    VS_ULAPAL = 0x86,
    VS_FRAME = 0x87,
    VS_FRAMING = 0x88,   // a CRTC R0-R9 write or ULA clock/teletext change
    VS_SKIP = 0x89,      // payload: ticks with DISPEN low
};
#define VS_EV(type, payload) ((uint16_t)((type) | ((payload) << 8)))

#ifndef VS_RING
#define VS_RING 32768   // words; a power of two
#endif

struct vstream {
    volatile uint32_t head;   // producer index, free running
    volatile uint32_t tail;   // consumer index, free running
    uint16_t ring[VS_RING];
};

static inline void vs_put(struct vstream *vs, uint16_t w)
{
    uint32_t h = vs->head;
    vs->ring[h & (VS_RING - 1)] = w;
    vs->head = h + 1;
}

static inline int vs_available(const struct vstream *vs)
{
    return (int)(vs->head - vs->tail);
}

static inline uint16_t vs_get(struct vstream *vs)
{
    uint32_t t = vs->tail;
    uint16_t w = vs->ring[t & (VS_RING - 1)];
    vs->tail = t + 1;
    return w;
}

#endif
