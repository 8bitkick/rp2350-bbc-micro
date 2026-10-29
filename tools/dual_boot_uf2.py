#!/usr/bin/env python3
"""Join the launcher and the two machines into one .uf2 (docs/dual-boot.md):

  dual_boot_uf2.py <partitions.json> <launcher.uf2> <master.uf2> <beeb.uf2> [--app <app.bin>]
                   [--save <built-in disc.ssd> <save disc.ssd>] <out.uf2>

Each machine's .uf2 is a plain build, linked at 0x10000000; its blocks go to its slot's place in
the flash (the partitions' order: master, beeb), where the boot ROM's address translation runs
it as if it were at 0x10000000. The launcher stays at the bottom. --app puts an app's image (its
.bin, as its build packs it: src/common/app_api.h) at the start of the app slot. --save gives the Master
a save disc to start from: <save disc.ssd>, a copy of its built-in disc (<built-in disc.ssd>, the one
the Master build embeds) with games saved on it, written where src/common/drive.c keeps the save disc,
with the header that ties it to that built-in disc (it overwrites the Master's saves on the board). All of it is written in the
'absolute' family, so the boot ROM puts each block where it says, whatever partition table the
flash has now. The flash above the slots (WiFi, the settings, the save discs) isn't touched, but for --save.

  dual_boot_uf2.py --plain <machine.uf2> <out.uf2>

makes a plain machine .uf2 'absolute' too: flashed over the combined firmware, it replaces the
launcher (and so the partition table) and leaves one machine, as before.
"""
import json, struct, sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY, FLAG_EXTENSIONS = 0x2000, 0x8000
ABSOLUTE = 0xe48bff57
FLASH = 0x10000000

def blocks(path):
    """(address, 256-byte payload) of each flash block, skipping the RP2350-E10 'ignore' block"""
    data = open(path, 'rb').read()
    out = []
    for i in range(0, len(data), 512):
        m0, m1, flags, addr, size, _, _, _ = struct.unpack('<8I', data[i:i + 32])
        if m0 != MAGIC0 or m1 != MAGIC1 or struct.unpack('<I', data[i + 508:i + 512])[0] != MAGIC_END:
            sys.exit('%s: not a UF2' % path)
        if flags & FLAG_EXTENSIONS and data[i + 32:i + 32 + size] == b'\xef' * size:
            continue
        if size != 256 or not FLASH <= addr < FLASH + 0x1000000:
            sys.exit('%s: a block at %08x (%d bytes) that isn\'t flash' % (path, addr, size))
        out.append((addr, data[i + 32:i + 32 + 256]))
    return out

# the Master's save disc (src/boards/presto/board_config.h, src/common/drive.c): the data, 80 tracks x 10
# sectors x 256, below the system sector and the WiFi sector at the top of the 16 MB, its header a sector below
SECTOR, SAVE_SIZE = 4096, 80 * 10 * 256
SAVE_DATA = 0x1000000 - 2 * SECTOR - SAVE_SIZE
SAVE_HEAD = SAVE_DATA - SECTOR
SAVE_MAGIC = 0x53415645                      # "SAVE"
def fnv(b):
    h = 2166136261
    for c in b: h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h
def save_blocks(golden_path, save_path):
    golden, save = open(golden_path, 'rb').read(), open(save_path, 'rb').read()
    if len(save) > SAVE_SIZE: sys.exit('%s: more than a save disc holds' % save_path)
    if save[:8] != golden[:8] or save[256:260] != golden[256:260]:
        sys.exit('%s: not a copy of %s (another title)' % (save_path, golden_path))
    data = save + bytes(SAVE_SIZE - len(save))
    head = struct.pack('<4I', SAVE_MAGIC, fnv(golden), len(golden), 1) + b'\xff' * (SECTOR - 16)
    return [(FLASH + SAVE_HEAD + i, head[i:i + 256]) for i in range(0, SECTOR, 256)] + \
           [(FLASH + SAVE_DATA + i, data[i:i + 256]) for i in range(0, SAVE_SIZE, 256)]

def size_k(v):
    return int(v[:-1]) * 1024 if isinstance(v, str) else v * 4096

def write(path, placed):
    placed.sort()
    if any(a+256>b for (a,_),(b,_) in zip(placed,placed[1:])):
        sys.exit("UF2 blocks overlap")
    out = bytearray()
    for n, (addr, payload) in enumerate(placed):
        out += struct.pack('<8I', MAGIC0, MAGIC1, FLAG_FAMILY, addr, 256, n, len(placed), ABSOLUTE)
        out += payload + bytes(476 - 256) + struct.pack('<I', MAGIC_END)
    open(path, 'wb').write(out)

def main():
    a = sys.argv[1:]
    c64 = None
    if "--c64" in a:
        i=a.index("--c64"); c64=a[i+1]; del a[i:i+2]
    app = None
    if '--app' in a:
        i = a.index('--app')
        app = a[i + 1]
        del a[i:i + 2]
    save = None
    if '--save' in a:
        i = a.index('--save')
        save = a[i + 1:i + 3]
        del a[i:i + 3]
    if len(a) == 3 and a[0] == '--plain':
        write(a[2], blocks(a[1]))
        print('wrote', a[2])
        return
    if len(a) != 5:
        sys.exit(__doc__)
    parts = json.load(open(a[0]))['partitions']
    if any(p['name']=='c64' for p in parts) and not c64:
        sys.exit('This partition table includes C64: supply --c64 <presto_c64.uf2>')
    placed = []
    launcher = blocks(a[1])
    first = min(size_k(p['start']) for p in parts)
    if max(addr for addr, _ in launcher) + 256 > FLASH + first:
        sys.exit('the launcher runs into the first slot')
    placed += launcher
    machines = list(zip(parts[:2], a[2:4]))
    if c64: machines.append((next(p for p in parts if p["name"]=="c64"),c64))
    for p, path in machines:
        start, size = size_k(p['start']), size_k(p['size'])
        image = blocks(path)
        end = max(addr for addr, _ in image) + 256 - FLASH
        if min(addr for addr, _ in image) != FLASH or end > size:
            sys.exit('%s: doesn\'t fit slot "%s" (%d KB of %d KB)' % (path, p['name'], end // 1024, size // 1024))
        placed += [(addr + start, payload) for addr, payload in image]
        print('%-6s %4d KB at %06x, of %d KB' % (p['name'], end // 1024, start, size // 1024))
    if app:
        p = parts[2]
        start, size = size_k(p['start']), size_k(p['size'])
        data = open(app, 'rb').read()
        if data[:4] != b'BAPP' or len(data) > size:
            sys.exit('%s: not an app image for slot "%s" (%d KB of %d KB)' % (app, p['name'], len(data) // 1024, size // 1024))
        data += b'\xff' * (-len(data) % 256)
        placed += [(FLASH + start + i, data[i:i + 256]) for i in range(0, len(data), 256)]
        print('%-6s %4d KB at %06x, of %d KB' % (p['name'], len(data) // 1024, start, size // 1024))
    if save:
        placed += save_blocks(*save)
        print('save   %4d KB at %06x (the Master\'s save disc: %s)' % (SAVE_SIZE // 1024, SAVE_DATA, save[1]))
    write(a[4], placed)
    print('wrote', a[4])

if __name__ == '__main__':
    main()
