#!/usr/bin/env python3
"""embed.py out.c name=file ...: C arrays (const, so they stay in flash) of
the files, and <name>_size for each."""
import sys
out = open(sys.argv[1], 'w')
out.write('#include <stdint.h>\n')
for arg in sys.argv[2:]:
    name, path = arg.split('=', 1)
    data = open(path, 'rb').read()
    out.write('const uint8_t %s[%d] __attribute__((aligned(4))) = {\n' % (name, len(data)))
    for i in range(0, len(data), 16):
        out.write(','.join('%d' % b for b in data[i:i + 16]) + ',\n')
    out.write('};\nconst uint32_t %s_size = %d;\n' % (name, len(data)))
