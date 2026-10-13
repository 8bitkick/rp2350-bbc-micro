#!/usr/bin/env python3
"""Profile a core on the Presto: collect samples over USB and rank functions.

  prof.py <core 0|1> [elf]      (elf defaults to build/src/pico/beebjit/presto_beebjit.elf)

PROF_SAVE=file also saves the raw samples (little-endian 32-bit PCs).

Build with -DPRESTO_BEEBJIT_PROF=ON (samples core 0) or -DPRESTO_BEEBJIT_PROF_CORE=ON as well
(core 1); samples in the JIT's code are ranked by 6502 address.
"""
import collections, os, struct, subprocess, sys, time
import serial

core = int(sys.argv[1]) if len(sys.argv) > 1 else 1
here = os.path.dirname(os.path.abspath(__file__))
elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', 'build', 'src', 'pico', 'beebjit', 'presto_beebjit.elf')
# arm-none-eabi-* from PICO_TOOLCHAIN_PATH if set, else from the PATH
TOOLS = os.path.join(os.environ['PICO_TOOLCHAIN_PATH'], 'bin', '') if 'PICO_TOOLCHAIN_PATH' in os.environ else ''

s = serial.Serial(os.environ.get('PRESTO_PORT', '/dev/ttyACM0'), 115200, timeout=10)
time.sleep(0.1); s.reset_input_buffer()
s.write(f'!prof {core}\n'.encode())
time.sleep(1.0)    # 4096 samples at ~10kHz
s.reset_input_buffer()
s.write(b'!profdump\n')
while True:
    line = s.readline().decode(errors='replace')
    if line.startswith('PROF'):
        n = int(line.split()[1]); break
data = b''
while len(data) < n * 4:
    chunk = s.read(n * 4 - len(data))
    if not chunk: raise SystemExit('short read')
    data += chunk
pcs = struct.unpack(f'<{n}I', data)
if os.environ.get('PROF_SAVE'):
    # the raw samples, for a closer look (harness/profile.py reads the same format)
    open(os.environ['PROF_SAVE'], 'wb').write(data)

# map each address to its innermost (possibly inlined) function and source line
uniq = sorted(set(pc & ~1 for pc in pcs))
out = subprocess.run([TOOLS + 'arm-none-eabi-addr2line', '-f', '-i', '-C', '-e', elf] + [hex(a) for a in uniq],
                     capture_output=True, text=True).stdout.splitlines()
# with -i each address yields one or more (function, file:line) pairs; split them up again by
# asking for each address on its own when the counts don't line up
info = {}
if len(out) == 2 * len(uniq):
    for i, a in enumerate(uniq):
        info[a] = (out[2 * i], out[2 * i + 1].split('/')[-1].split(' ')[0])
else:
    for a in uniq:
        o = subprocess.run([TOOLS + 'arm-none-eabi-addr2line', '-f', '-i', '-C', '-e', elf, hex(a)],
                           capture_output=True, text=True).stdout.splitlines()
        info[a] = (o[0], o[1].split('/')[-1].split(' ')[0]) if len(o) >= 2 else ('?', '?')
# presto_beebjit: JIT code (64 bytes per 6502 byte from 0x11000000), the stubs
# after it and inturbo's handlers, all in PSRAM
JIT_BASE, JIT_END = 0x11000000, 0x11400000
for a in uniq:
    if JIT_BASE <= a < JIT_END:
        info[a] = ('[JIT code]', '6502 &%04X' % ((a - JIT_BASE) >> 6))
    elif JIT_END <= a < JIT_END + 0x8000:
        info[a] = ('[JIT stubs]', '+%x' % (a - JIT_END))
    elif JIT_END + 0x8000 <= a < JIT_END + 0x10000:
        info[a] = ('[inturbo]', 'opcode %02X' % ((a - JIT_END - 0x8000) >> 7))
funcs = collections.Counter(info[pc & ~1][0] for pc in pcs)
lines = collections.Counter(f'{info[pc & ~1][0]}  {info[pc & ~1][1]}' for pc in pcs)
print(f'core {core}: {n} samples\n-- by (innermost) function')
for fn, c in funcs.most_common(int(os.environ.get('PROF_FUNCS', '20'))):
    print(f'{100 * c / n:5.1f}%  {fn}')
print('-- by source line')
for fn, c in lines.most_common(int(os.environ.get('PROF_LINES', '20'))):
    print(f'{100 * c / n:5.1f}%  {fn}')

jit = [pc & ~1 for pc in pcs if JIT_BASE <= (pc & ~1) < JIT_END]
if jit:
    print(f'-- JIT code by 6502 address (64-byte slot; a block runs on past its first)')
    for a, c in collections.Counter((pc - JIT_BASE) >> 6 for pc in jit).most_common(int(os.environ.get('PROF_JIT', '15'))):
        print(f'{100 * c / n:5.1f}%  &{a:04X}')

# code run from flash (through the 16KB XIP cache) rather than RAM: candidates for RAM
flash = [pc for pc in pcs if 0x10000000 <= (pc & ~1) < 0x11000000]
print(f'-- from flash: {100.0 * len(flash) / n:.1f}% of samples')
for fn, c in collections.Counter(info[pc & ~1][0] for pc in flash).most_common(25):
    print(f'{100.0 * c / n:5.1f}%  {fn}')
