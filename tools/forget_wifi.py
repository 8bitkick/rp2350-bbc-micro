#!/usr/bin/env python3
"""forget_wifi.py OUT.uf2: a UF2 that blanks the Presto's saved WiFi settings (the last 4 KB sector of
the flash, board_config.h's BOARD_FLASH_WIFI_OFFSET: the network to join, and the setup network's
password), and nothing else: not the firmware, the machine chosen or the save discs. Copied onto the
RP2350 drive (hold BOOT, tap RESET), the firmware already there restarts with no network saved, so
into WiFi setup. The Acorn Archimedes for the Presto keeps its WiFi settings in the same sector, the
same way, so this works for it too.

Its blocks are 'absolute' (family 0xe48bff57), so they land whatever the flash's partition table says.
"""
import struct, sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
FLASH_END = 0x10000000 + 16 * 1024 * 1024
SECTOR = 4096
FAMILY = 0xe48bff57     # absolute

blocks = []
n = SECTOR // 256
for i in range(n):
    addr = FLASH_END - SECTOR + 256 * i
    data = b'\xff' * 256 + b'\x00' * (476 - 256)
    blocks.append(struct.pack('<IIIIIIII', 0x0A324655, 0x9E5D5157, 0x2000, addr, 256, i, n, FAMILY) + data +
                  struct.pack('<I', 0x0AB16F30))
open(sys.argv[1], 'wb').write(b''.join(blocks))
print('wrote', sys.argv[1], len(blocks), 'blocks')
