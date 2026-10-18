#!/usr/bin/env python3
"""Checks video:frame-skip against drawing every frame, on one scene:

  harness/skipcheck.py <harness> [harness args...]

Runs the scene twice (-frame-trace, with and without -frame-skip). The 6502
(its RAM at the end) must be identical: skipping is display only. Each
frame's picture may be the previous frame's (a change shows one frame late),
never anything else. Prints how many frames the reference left unchanged
(the ones skipping saves) and how many came out a frame late. Typed scenes
want -fake-time, or their timing varies between the runs.
"""
import re, subprocess, sys

def run(args):
    out = subprocess.run(args, capture_output=True, text=True, errors='replace').stdout
    # (in terminal mode the BBC's RS423 output shares stdout with the trace)
    fb = [m.group(2) for m in re.finditer(r'frame \d+ cycles (\d+) fb ([0-9a-f]{8}) ram [0-9a-f]{8}', out)]
    cyc.extend(int(m.group(1)) for m in re.finditer(r'frame \d+ cycles (\d+) fb ', out))
    hw.extend(int(m.group(1)) for m in re.finditer(r' hw (\d+)', out))
    m = re.search(r'end cycles \d+ ram ([0-9a-f]{8})', out)
    return fb, (m.group(1) if m else None)
cyc = []
hw = []

base = sys.argv[1:] + ['-frame-trace']
fb0, ram0 = run(base)
cyc0, cyc, hw0, hw = cyc, [], hw, []
fb1, ram1 = run(base + ['-frame-skip'])
cyc1, hw1 = cyc, hw
if not fb0 or not fb1:
    sys.exit('FAIL frames: %d without skip, %d with' % (len(fb0), len(fb1)))
if ram0 != ram1:
    sys.exit('FAIL 6502 state differs at the end: %s vs %s' % (ram0, ram1))
# Every picture the skip run shows, the reference showed, in the same order; a
# picture the skip run never shows is one the reference showed for a single
# frame as the screen started changing (that frame is shown late, and the next
# change supersedes it). The frame ticks around a CRTC reprogram may not match
# the renderer's vsyncs one for one, so the counts needn't.
import difflib
def runs(fb):
    out = []
    for h in fb:
        if out and out[-1][0] == h:
            out[-1][1] += 1
        else:
            out.append([h, 1])
    return out
r0, r1 = runs(fb0), runs(fb1)
h0, h1 = [h for h, n in r0], [h for h, n in r1]
# the first frame (index) of each skip-run picture, to know whether the display hardware was being
# written in it: while the CRTC or ULA is reprogrammed (a MODE change), the frame is transitional and
# a run resuming in the middle of it draws it differently
first1 = []
i = 0
for h, n in r1:
    first1.append(i)
    i += n
transitional = 0
dropped = 0
for op, a0, a1, b0, b1 in difflib.SequenceMatcher(None, h0, h1, autojunk=False).get_opcodes():
    if op == 'equal':
        continue
    if op in ('insert', 'replace'):
        for k in range(b0, b1):
            f = first1[k]
            if r1[k][1] == 1 and hw1 and f < len(hw1) and hw1[f] != hw1[max(0, f - 2)]:
                transitional += 1
                continue
            sys.exit('FAIL skipping shows %s (picture %d), which the reference shows as %s' % (h1[k], k, h0[a0] if a0 < len(h0) else '-'))
        if op == 'replace':
            for k in range(a0, a1):
                if r0[k][1] > 1:
                    sys.exit('FAIL the reference showed %s for %d frames (picture %d), skipping never shows it' % (h0[k], r0[k][1], k))
                dropped += 1
        continue
    for k in range(a0, a1):
        if r0[k][1] > 1:
            sys.exit('FAIL the reference showed %s for %d frames (picture %d), skipping never shows it' % (h0[k], r0[k][1], k))
        dropped += 1
if fb0[-1] != fb1[-1]:
    sys.exit('FAIL the last pictures differ: %s vs %s' % (fb0[-1], fb1[-1]))
print('ok    %d frames (%d with skip), %d pictures, %d frames the reference left unchanged, %d one-frame pictures never shown, %d transitional frames drawn differently'
      % (len(fb0), len(fb1), len(r0), len(fb0) - len(r0), dropped, transitional))
