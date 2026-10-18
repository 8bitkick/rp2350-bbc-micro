#!/usr/bin/env python3
"""Summarise PC samples from the QEMU harness (-profile file) by function, and
optionally by source line for one function.

  harness/profile.py build-qemu/harness profile.bin [--top 25] [--lines func]
"""
import argparse, bisect, collections, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("elf")
ap.add_argument("samples")
ap.add_argument("--top", type=int, default=25)
ap.add_argument("--lines", help="break this function down by source line")
ap.add_argument("--tools", default="arm-none-eabi-")
args = ap.parse_args()

data = open(args.samples, "rb").read()
pcs = [p & ~1 for p in struct.unpack("<%dI" % (len(data) // 4), data)]
syms = []
for line in subprocess.run([args.tools + "nm", "-n", "-S", "--defined-only", args.elf],
                           capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 4 and f[2] in "tTwW":
        syms.append((int(f[0], 16), int(f[1], 16), f[3]))
syms.sort()
starts = [s[0] for s in syms]

def func(pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and pc < syms[i][0] + max(syms[i][1], 1):
        return syms[i][2]
    return "?%08x" % pc

counts = collections.Counter(func(pc) for pc in pcs)
total = len(pcs)
print("%d samples" % total)
for name, n in counts.most_common(args.top):
    print("%6.2f%%  %7d  %s" % (100.0 * n / total, n, name))

if args.lines:
    sel = [pc for pc in pcs if func(pc) == args.lines]
    c = collections.Counter(sel)
    addrs = sorted(c)
    out = subprocess.run([args.tools + "addr2line", "-e", args.elf] + ["%x" % a for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    by_line = collections.Counter()
    for a, l in zip(addrs, out):
        by_line[l.split("/")[-1]] += c[a]
    print("\n%s by line (%d samples):" % (args.lines, len(sel)))
    for l, n in by_line.most_common(args.top):
        print("%6.2f%%  %s" % (100.0 * n / max(len(sel), 1), l))
