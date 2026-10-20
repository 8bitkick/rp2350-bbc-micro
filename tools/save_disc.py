#!/usr/bin/env python3
"""save_disc.py <disc.ssd> <D.NAME> <file> <out.ssd>: a copy of a DFS disc with one more file on it, in directory
D (a commander saved in Elite's E. directory, say), in the free space after its last file. Used to make a release's
save disc (tools/dual_boot_uf2.py --save) from the built-in disc and a commander kept in .github/release/."""
import sys

def entries(d):
    n = d[0x105] // 8
    out = []
    for i in range(n):
        e = d[0x108 + i * 8:0x108 + i * 8 + 8]
        m = e[6]
        length = e[4] | e[5] << 8 | ((m >> 4) & 3) << 16
        start = e[7] | (m & 3) << 8
        out.append((start, length))
    return out

def main():
    if len(sys.argv) != 5: sys.exit(__doc__)
    disc, dname, src, out = sys.argv[1:]
    d = bytearray(open(disc, 'rb').read())
    data = open(src, 'rb').read()
    directory, name = dname.split('.', 1)
    if len(directory) != 1 or not 1 <= len(name) <= 7: sys.exit('%s: a name is D.NAME, the name up to 7 characters' % dname)
    n = d[0x105] // 8
    if n >= 31: sys.exit('%s: the catalogue is full' % disc)
    for i in range(n):
        if d[8 + i * 8:8 + i * 8 + 7].decode('latin1').rstrip() == name and chr(d[15 + i * 8] & 0x7f) == directory:
            sys.exit('%s: already has %s' % (disc, dname))
    sectors = (d[0x106] & 3) << 8 | d[0x107]
    start = max([s + (l + 255) // 256 for s, l in entries(d)] + [2])
    need = (len(data) + 255) // 256
    if start + need > sectors: sys.exit('%s: no room' % disc)
    size = (start + need) * 256
    if len(d) < size: d += bytes(size - len(d))
    d[start * 256:start * 256 + len(data)] = data
    # the catalogue keeps the files highest start sector first: the new one leads
    names = bytes(name.ljust(7), 'latin1') + bytes([ord(directory)])
    info = bytes([0, 0, 0, 0, len(data) & 0xff, len(data) >> 8 & 0xff, ((len(data) >> 16) & 3) << 4 | (start >> 8) & 3, start & 0xff])
    d[8:8 + (n + 1) * 8] = names + bytes(d[8:8 + n * 8])
    d[0x108:0x108 + (n + 1) * 8] = info + bytes(d[0x108:0x108 + n * 8])
    d[0x105] = (n + 1) * 8
    open(out, 'wb').write(d)
    print('%s: %s, %d bytes at sector %d' % (out, dname, len(data), start))

main()
