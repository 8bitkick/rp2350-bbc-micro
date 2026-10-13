#!/usr/bin/env python3
"""beebjit_hillclimb.py: draw beebjit's optimization steps, from the measurements in STEPS below.

Writes docs/presto/beebjit-hillclimb.svg: two panels on one step axis, each on its own scale,
throughput (turbo) and per-core load at real speed. Only measured values are drawn (unmeasured
stretches are dotted).
"""
import os

ROOT = os.path.join(os.path.dirname(__file__), '..')
OUT = os.path.join(ROOT, 'docs', 'presto')

THEMES = {
    'light': dict(surface='#fcfcfb', ink='#0b0b0b', ink2='#52514e', grid='#e4e3de', mark='#bdbcb6', c0='#2a78d6', c1='#eb6834', one='#2a78d6'),
    'dark': dict(surface='#1a1a19', ink='#f0efec', ink2='#c3c2b7', grid='#33322f', mark='#5e5d58', c0='#3987e5', c1='#d95926', one='#3987e5'),
}


# Each step (measured on the board, from the JIT's first run on Elite): core 0 and
# core 1 load at real speed (%), turbo (× real time), free RAM (KB); None where not measured.
# Before these, beebjit's C interpreter ran the BASIC prompt at 0.57× (not Elite).
STEPS = [
    (1, 'The Thumb-2 JIT runs Elite; drawing on the same core', 69, None, 1.45, None),
    (2, 'Cheaper video: blank runs, packed tables on first use, whole scanlines', 41, None, 2.4, None),
    (3, 'The render wire: core 1 draws', None, None, 3.75, None),
    (4, 'Fast lines take in R0\'s tick; one wire command a line', 24, 19, 4.2, 126),
    (5, 'Hot code into SRAM', 20, 19, 5.0, 75),
    (6, 'Bursts of register accesses stay in the interpreter', None, None, 5.0, None),
    (7, 'Thumb-2 inturbo: dynamic opcodes and sub-instructions', 20, 19, 4.95, 74),
    (8, 'No steady-state recompiles', 16, 19, 5.17, 73),
    (9, 'Core 1 skips black over black; 640→480 on PIO', 16, 13, 6.1, 71),
    (10, 'Whole character rows at a time; 4-byte screen fetch', 13, 13, 7.3, 68),
    (11, 'One-row bitmap runs, lazier row clears', 13, 12, 7.3, 68),
    (12, 'WiFi web panel, SD card, snapshots; leaner PSRAM', 13, 11, 7.6, 35),
    (13, 'JIT: register accesses compiled as encoded callbacks', 11, 12, 8.0, 32),
    (14, 'No interpreter look-ahead for register accesses', 10, 11, 8.4, 32),
    (15, 'Smaller JIT code: subs/bpl countdown checks, JSR push through a stub', 10, 11, 8.5, 32),
    (16, 'MODE 7 under load: a cache of teletext runs', 9, 11, 8.4, 29),
    (17, 'A MODE 7 line as one wire command', 9, 11, 8.1, 27),
    # (an honest dip: the panel locked to 50 Hz and frames drawn in slices, for smooth motion)
    (18, 'Smooth motion: the panel locked to the BBC at 50 Hz, frames drawn in slices', 11, 13, 7.4, 24),
    (19, 'Cheaper slices', 10, 12, 7.5, 24),
    (20, 'RTS and JMP (ind) through stubs', 10, 13, 7.5, 24),
    (21, 'The panel DMA reads the renderer\'s row: no copy on core 1', 10, 11, 8.2, 23),
    (22, 'Moving teletext: undrawn runs skip the cache, a leaner miss', 11, 11, 8.4, 23),
    (23, 'newlib\'s mem* functions in RAM; set-up code in flash', 10, 11, 8.6, 25),
    (24, 'A branchless teletext drawing loop', 10, 11, 8.4, 25),
    (25, 'Palette tables two pixels at a time', 11, 10, 9.0, 25),
    (26, 'A cheaper callback for the JIT\'s bounces into the interpreter', 10, 10, 9.1, 25),
    (27, 'Teletext: the beam to the window without the renderer', 10, 10, 9.1, 24),
    (28, 'The interpreter\'s masked-IRQ check inline', 10, 10, 8.8, 25),
]

# Marked steps: (graph step, label). The 50 Hz lock's dip in throughput is the price of smooth
# motion (frames shown once and whole), not a regression.
MARKS = [(18, '50 Hz lock')]


def read_table():
    return [dict(n=n, step=step, c0=c0, c1=c1, turbo=turbo, ram=ram)
            for n, step, c0, c1, turbo, ram in STEPS]


def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def chart(rows, t):
    W, L, R = 820, 56, 110
    n = len(rows)
    col = (W - L - R) / n
    cx = lambda i: L + col * (i + 0.5)          # a step's column: its centre, and edges
    x0 = lambda i: L + col * i
    x1 = lambda i: L + col * (i + 1)
    out = []

    panels = [('Throughput, unthrottled: emulated speed vs. a real BBC Micro', 'turbo', 0, 10, [0, 2, 4, 6, 8, 10], '×', 190),
              ('CPU load per core at real speed', 'load', 0, 80, [0, 20, 40, 60, 80], '%', 150)]
    H = 40 + sum(p[6] + 58 for p in panels) + 30
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
               f'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif" font-size="12">')
    out.append('<title>Performance and CPU load by optimization step</title>')
    out.append(f'<rect width="{W}" height="{H}" rx="10" fill="{t["surface"]}"/>')
    out.append(f'<text x="{L}" y="26" font-size="15" font-weight="600" fill="{t["ink"]}">Performance and CPU load by optimization step</text>')
    out.append(f'<text x="{W - R}" y="26" text-anchor="end" fill="{t["ink2"]}">Elite title screen · RP2350 at 250 MHz</text>')
    top = 44
    span = []
    for title, key, lo, hi, ticks, unit, ph in panels:
        out.append(f'<text x="{L}" y="{top + 14}" font-weight="600" fill="{t["ink"]}">{esc(title)}</text>')
        py0, py1 = top + 32, top + 32 + ph
        span.append((py0, py1))
        y = lambda v: py1 - (py1 - py0) * (v - lo) / (hi - lo)
        for tk in ticks:
            out.append(f'<line x1="{L}" x2="{W - R}" y1="{y(tk):.1f}" y2="{y(tk):.1f}" stroke="{t["grid"]}"/>')
            out.append(f'<text x="{L - 8}" y="{y(tk) + 4:.1f}" text-anchor="end" fill="{t["ink2"]}">{tk}{unit}</text>')
        series = [('c1', 'core 1: drawing', t['c1']), ('c0', 'core 0: emulator', t['c0'])] if key == 'load' else [(key, None, t['one'])]
        if key == 'load':
            for k, (field, lab, c) in enumerate(reversed(series)):
                lx = W - R - 250 + k * 130
                out.append(f'<line x1="{lx}" x2="{lx + 16}" y1="{top + 10}" y2="{top + 10}" stroke="{c}" stroke-width="2.5"/>')
                out.append(f'<text x="{lx + 22}" y="{top + 14}" fill="{t["ink2"]}">{lab}</text>')
        ends = {f: [r[f] for r in rows if r[f] is not None][-1] for f, _, _ in series}
        # the end labels: the higher value's above its line, the other's below (on a tie, the
        # first series' above)
        above = max(ends, key=lambda f: (ends[f], -[s[0] for s in series].index(f)))
        for field, label, c in series:
            # steps: each value held across its column, a riser to the next; nothing where unmeasured
            d, last = '', None
            for i, r in enumerate(rows):
                v = r[field]
                if v is None:
                    last = None
                    continue
                d += (f'L{x0(i):.1f},{y(v):.1f}' if last is not None else f'M{x0(i):.1f},{y(v):.1f}') + f'H{x1(i):.1f}'
                last = i
            out.append(f'<path d="{d}" fill="none" stroke="{c}" stroke-width="2.5" stroke-linejoin="round"/>')
            # across an unmeasured stretch: dotted, from the last value to the next
            for i in range(1, n):
                if rows[i][field] is None and rows[i - 1][field] is not None:
                    j = next((k for k in range(i, n) if rows[k][field] is not None), None)
                    if j is not None:
                        out.append(f'<path d="M{x0(i):.1f},{y(rows[i - 1][field]):.1f}L{x0(j):.1f},{y(rows[j][field]):.1f}" '
                                   f'fill="none" stroke="{c}" stroke-width="1.5" stroke-dasharray="2 4" stroke-linecap="round"/>')
            v = ends[field]
            dy = 4 if len(ends) == 1 else (-3 if field == above else 13)
            text = f'{label.split(":")[0]} {v:g}{unit}' if label else f'{v:g}{unit}'
            out.append(f'<text x="{x1(n - 1) + 8:.1f}" y="{y(v) + dy:.1f}" fill="{t["ink"]}" font-weight="600">{esc(text)}</text>')
        if key == 'turbo':
            out.append(f'<text x="{x0(0):.1f}" y="{y(rows[0]["turbo"]) + 18:.1f}" fill="{t["ink2"]}">{rows[0]["turbo"]:g}×</text>')
        top = py1 + 26
    for step, label in MARKS:
        i = next(k for k, r in enumerate(rows) if r['n'] == step)
        # through the middle of the step's column (clear of the risers), in each panel (clear of
        # the legend between them)
        for k, (py0, py1) in enumerate(span):
            out.append(f'<line x1="{cx(i):.1f}" x2="{cx(i):.1f}" y1="{py0 - (12 if k == 0 else 0):.1f}" y2="{py1:.1f}" '
                       f'stroke="{t["mark"]}" stroke-width="1" stroke-dasharray="1.5 3" stroke-linecap="round"/>')
        out.append(f'<text x="{cx(i) - 5:.1f}" y="{span[0][0] - 3:.1f}" text-anchor="end" font-size="11" '
                   f'fill="{t["ink2"]}">{esc(label)}</text>')
    measured = sum(1 for r in rows if r['c0'] is not None)
    out.append(f'<text x="{(L + W - R) / 2:.0f}" y="{top - 6}" text-anchor="middle" fill="{t["ink2"]}">{measured} measured steps, in three days →</text>')
    out.append('</svg>')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    rows = read_table()
    path = os.path.join(OUT, 'beebjit-hillclimb.svg')
    open(path, 'w').write(chart(rows, THEMES['dark']))
    print('wrote', path, len(rows), 'steps')
