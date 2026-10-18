// esp32-beeb: 6522 VIA. See via.h.

#include "via.h"

#ifndef VIA_API
#define VIA_API
#endif

static inline void via_check_irq(struct via *via)
{
    if (via->ifr & via->ier & 0x7f) {
        via->ifr |= 0x80;
        via->irq = 1;
    } else {
        via->ifr &= 0x7f;
        via->irq = 0;
    }
}

static inline void via_raise(struct via *via, uint8_t bits)
{
    via->ifr |= bits;
    via_check_irq(via);
}

static inline void via_clear(struct via *via, uint8_t bits)
{
    int was_ca2 = via->ifr & VIA_INT_CA2;
    via->ifr &= ~bits;
    via_check_irq(via);
    // Clearing the CA2 flag while the keyboard auto-scan has a key down:
    // the line toggles again as the scan sweeps past the key, so the flag
    // comes straight back (beebjit's via_set_IFR special case). The machine
    // re-evaluates port A, which does the toggle.
    if (was_ca2 && (bits & VIA_INT_CA2) && via->port_cb) {
        via->port_cb(via, via->ctx, 2);
    }
}

VIA_API void via_update_bus_a(struct via *via)
{
    uint8_t v = (via->ora & via->ddra) | ~via->ddra;
    via->bus_a = v & via->peripheral_a;
}

VIA_API uint8_t via_port_a_out(const struct via *via)
{
    return (via->ora & via->ddra) | ~via->ddra;
}

VIA_API uint8_t via_port_b(const struct via *via)
{
    return (via->orb & via->ddrb) | (via->peripheral_b & ~via->ddrb);
}

static void via_port_changed(struct via *via, int which)
{
    if (via->port_cb) {
        via->port_cb(via, via->ctx, which);
    }
    via_update_bus_a(via);
}

VIA_API void via_reset(struct via *via)
{
    // Matches beebjit (and a real BBC): OR/DDR zero, inputs read high.
    via->ora = via->orb = via->ddra = via->ddrb = 0;
    via->ira = via->irb = 0xff;
    via->sr = via->acr = via->pcr = via->ifr = 0;
    via->ier = 0x80;
    via->ca1 = via->ca2 = via->cb1 = via->cb2 = 0;
    via->peripheral_a = via->peripheral_b = 0xff;
    via->t1c = via->t1l = via->t2c = via->t2l = 0xffff;
    via->t1_armed = via->t2_armed = via->t1_reload = 0;
    via->pb7 = 1;  // beebjit: needed for Planetoid
    via->irq = 0;
    via_port_changed(via, 0);
    via_port_changed(via, 1);
}

VIA_API void via_tick(struct via *via)
{
    // T1
    if (via->t1_reload) {
        via->t1_reload = 0;
        via->t1c = via->t1l;
    } else {
        via->t1c--;
        if (via->t1c == 0xffffffffu) {   // past 0: reads FFFF, fires
            if (via->t1_armed) {
                via_raise(via, VIA_INT_T1);
                via->pb7 ^= 1;
                if (!(via->acr & 0x40)) {
                    via->t1_armed = 0;
                }
            }
            via->t1_reload = 1;
        }
    }
    // T2: does not count in pulse-counting mode (PB6 pulses, none here).
    if (!(via->acr & 0x20)) {
        via->t2c--;
        if (via->t2c == 0xffff && via->t2_armed) {
            via->t2_armed = 0;
            via_raise(via, VIA_INT_T2);
        }
    }
}

VIA_API uint32_t via_ticks_to_event(const struct via *via)
{
    uint32_t t1 = via->t1_reload ? 1 : (uint32_t)via->t1c + 1;
    uint32_t t2 = 0xffffffffu;
    if (via->t2_armed && !(via->acr & 0x20)) {
        t2 = (uint32_t)via->t2c + 1;
    }
    return t1 < t2 ? t1 : t2;
}

VIA_API void via_advance(struct via *via, uint32_t k)
{
    via->t1c -= k;
    if (!(via->acr & 0x20)) {
        via->t2c -= k;  // an unarmed T2 just wraps
    }
}

VIA_API uint8_t via_read(struct via *via, uint8_t reg)
{
    uint8_t v;
    switch (reg & 0xf) {
    case VIA_ORB:
        via_clear(via, VIA_INT_CB1 | VIA_INT_CB2);
        v = (via->orb & via->ddrb);
        v |= ((via->acr & 0x02) ? via->irb : via_port_b(via)) & ~via->ddrb;
        if (via->acr & 0x80) {
            v = (v & 0x7f) | (via->pb7 << 7);
        }
        return v;
    case VIA_ORA:
        via_clear(via, VIA_INT_CA1 | VIA_INT_CA2);
        // fall through
    case VIA_ORANH:
        return (via->acr & 0x01) ? via->ira : via->bus_a;
    case VIA_DDRB: return via->ddrb;
    case VIA_DDRA: return via->ddra;
    case VIA_T1CL:
        // A read on the tick T1 fired (the counter showing FFFF, reloading
        // next) leaves the flag set: beebjit's via_t1_just_fired.
        if (!via->t1_reload) {
            via_clear(via, VIA_INT_T1);
        }
        return via->t1c & 0xff;
    case VIA_T1CH: return (via->t1c >> 8) & 0xff;
    case VIA_T1LL: return via->t1l & 0xff;
    case VIA_T1LH: return via->t1l >> 8;
    case VIA_T2CL:
        // Likewise on the tick T2 fired (it fires on reaching FFFF; a later
        // wrap through FFFF has no flag to keep, so the test is enough).
        if (via->t2c != 0xffff) {
            via_clear(via, VIA_INT_T2);
        }
        return via->t2c & 0xff;
    case VIA_T2CH: return via->t2c >> 8;
    case VIA_SR: return via->sr;
    case VIA_ACR: return via->acr;
    case VIA_PCR: return via->pcr;
    case VIA_IFR: return via->ifr;
    case VIA_IER: return via->ier;
    }
    return 0xff;
}

VIA_API void via_write(struct via *via, uint8_t reg, uint8_t val)
{
    switch (reg & 0xf) {
    case VIA_ORB:
        via->orb = val;
        via_clear(via, VIA_INT_CB1 | VIA_INT_CB2);
        via_port_changed(via, 1);
        break;
    case VIA_ORA:
        via_clear(via, VIA_INT_CA1 | VIA_INT_CA2);
        // fall through
    case VIA_ORANH:
        via->ora = val;
        via_port_changed(via, 0);
        break;
    case VIA_DDRB:
        via->ddrb = val;
        via_port_changed(via, 1);
        break;
    case VIA_DDRA:
        via->ddra = val;
        via_port_changed(via, 0);
        break;
    case VIA_T1CL:
    case VIA_T1LL:
        via->t1l = (via->t1l & 0xff00) | val;
        break;
    case VIA_T1CH:
        via->t1l = (via->t1l & 0x00ff) | (val << 8);
        via->t1c = via->t1l + 1;
        via->t1_reload = 0;
        via->t1_armed = 1;
        via->pb7 = 0;
        via_clear(via, VIA_INT_T1);
        break;
    case VIA_T1LH:
        via->t1l = (via->t1l & 0x00ff) | (val << 8);
        via_clear(via, VIA_INT_T1);
        break;
    case VIA_T2CL:
        via->t2l = (via->t2l & 0xff00) | val;
        break;
    case VIA_T2CH:
        via->t2l = (via->t2l & 0x00ff) | (val << 8);
        via->t2c = via->t2l + ((via->acr & 0x20) ? 0 : 1);
        via->t2_armed = 1;
        via_clear(via, VIA_INT_T2);
        break;
    case VIA_SR:
        via->sr = val;
        break;
    case VIA_ACR:
        via->acr = val;
        break;
    case VIA_PCR:
        via->pcr = val;
        // CA2/CB2 in output modes: fixed levels, no interrupts.
        if ((val & 0x0e) == 0x0c) {
            via->ca2 = 0;
        } else if ((val & 0x0e) == 0x0e) {
            via->ca2 = 1;
        }
        if ((val & 0xe0) == 0xc0) {
            via->cb2 = 0;
        } else if ((val & 0xe0) == 0xe0) {
            via->cb2 = 1;
        }
        break;
    case VIA_IFR:
        via_clear(via, val & 0x7f);
        break;
    case VIA_IER:
        if (val & 0x80) {
            via->ier |= val;
        } else {
            via->ier &= ~val;
        }
        via->ier |= 0x80;
        via_check_irq(via);
        break;
    }
}

VIA_API void via_set_ca1(struct via *via, int level)
{
    if (level == via->ca1) {
        return;
    }
    via->ca1 = level;
    if (level == ((via->pcr >> 0) & 1)) {
        via->ira = via->bus_a;
        via_raise(via, VIA_INT_CA1);
    }
}

VIA_API void via_set_ca2(struct via *via, int level)
{
    if (level == via->ca2) {
        return;
    }
    via->ca2 = level;
    if (via->pcr & 0x08) {
        return;  // output mode
    }
    if (level == ((via->pcr >> 2) & 1)) {
        via_raise(via, VIA_INT_CA2);
    }
}

VIA_API void via_set_cb1(struct via *via, int level)
{
    if (level == via->cb1) {
        return;
    }
    via->cb1 = level;
    if (level == ((via->pcr >> 4) & 1)) {
        via->irb = via->peripheral_b;
        via_raise(via, VIA_INT_CB1);
    }
}

VIA_API void via_set_cb2(struct via *via, int level)
{
    if (level == via->cb2) {
        return;
    }
    via->cb2 = level;
    if (via->pcr & 0x80) {
        return;
    }
    if (level == ((via->pcr >> 6) & 1)) {
        via_raise(via, VIA_INT_CB2);
    }
}
