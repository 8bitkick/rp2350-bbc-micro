// esp32-beeb: the renderer's pixel type.
//
// On the host the renderer produces beebjit's 0xAARRGGBB words so frames can
// be diffed against it. On the board (RENDER_INDEXED) a pixel is a colour
// code resolved when the span is emitted: bits 0-3 a logical colour that
// goes through the ULA palette then, or with bit 4 set a physical 3-bit
// colour (teletext, blanking; BGR bit order as the palette register), and
// bit 5 for the cursor's inversion. The emitter resolves a span with the
// palette as it stood when the span was rendered, so a palette write only
// ends the span: the expansion tables depend on the mode alone and never
// rebuild for the palette, which a demo may rewrite every scanline.
// Teletext's anti-aliased glyph edges are the one thing lost: a blend
// becomes whichever of the two colours is nearer.

#ifndef PIXEL_H
#define PIXEL_H

#include <stdint.h>
#include <string.h>

// Placement of the renderer's per-tick code: the board puts it in IRAM
// (flash-cached code costs it more than the rendering itself, measured).
#ifndef RENDER_FN
#define RENDER_FN
#endif

#ifdef RENDER_INDEXED
typedef uint8_t render_px;
#define PX_CODES 64
#define PX_LOGICAL(l) ((render_px)(l))
#define PX_PHYS(p) ((render_px)(16 | (p)))
#define PX_BLACK PX_PHYS(0)
// r, g, b are 0 or 1.
#define PX_RGB(r, g, b) PX_PHYS((r) | ((g) << 1) | ((b) << 2))
#define PX_INVERT(p) ((render_px)((p) ^ 32))
// A code to its 3-bit physical colour under a palette: beebjit's
// display_color for the logical ones (the complemented register value,
// bit 3 the flash bit), then the cursor's complement.
static inline uint8_t px_resolve(render_px code, const uint8_t *logical_to_physical, int is_flash)
{
    uint8_t p;
    if (code & 16) {
        p = code & 7;
    } else {
        p = logical_to_physical[code & 15] ^ 7;
        if (p & 8) {
            p &= 7;
            if (is_flash) {
                p ^= 7;
            }
        }
    }
    if (code & 32) {
        p ^= 7;
    }
    return (uint8_t)(p & 7);
}
// v is 0..255 coverage of fg over bg.
#define PX_BLEND(v, fg, bg) ((v) >= 128 ? (fg) : (bg))
// Eight pixels at a time; both sides are 4-byte aligned by construction.
#define PX_COPY8(dst, src) do { \
        ((uint32_t *)(dst))[0] = ((const uint32_t *)(src))[0]; \
        ((uint32_t *)(dst))[1] = ((const uint32_t *)(src))[1]; \
    } while (0)
#define PX_COPY16(dst, src) do { \
        ((uint32_t *)(dst))[0] = ((const uint32_t *)(src))[0]; \
        ((uint32_t *)(dst))[1] = ((const uint32_t *)(src))[1]; \
        ((uint32_t *)(dst))[2] = ((const uint32_t *)(src))[2]; \
        ((uint32_t *)(dst))[3] = ((const uint32_t *)(src))[3]; \
    } while (0)
#else
typedef uint32_t render_px;
#define PX_BLACK 0xff000000u
#define PX_RGB(r, g, b) (0xff000000u | ((r) * 0xff0000u) | ((g) * 0xff00u) | ((b) * 0xffu))
#define PX_INVERT(p) ((p) ^ 0x00ffffffu)
// beebjit's teletext blend: fg and bg are one unit per channel there (see
// teletext_init), so the multiply is per channel.
#define PX_BLEND(v, fg, bg) (((v) * (fg) + (255 - (v)) * (bg)) | 0xff000000u)
#define PX_COPY8(dst, src) memcpy((dst), (src), 8 * sizeof(render_px))
#define PX_COPY16(dst, src) memcpy((dst), (src), 16 * sizeof(render_px))
#endif

#endif
