#!/usr/bin/env python3
"""Regenerate the checked-in logo header (Pillow required only for regeneration)."""
from pathlib import Path
from PIL import Image, ImageOps
root = Path(__file__).resolve().parent.parent
src = Image.open(root / 'assets/boot-owl.png').convert('RGB')
logo = ImageOps.pad(src, (48, 48), method=Image.Resampling.NEAREST, color='black')
values = [7 if max(pixel) >= 128 else 0 for pixel in logo.getdata()]
(root / 'src/common/boot_logo.h').write_text(
    '// Generated from the supplied owl artwork by tools/embed_boot_logo.py.\n'
    'static const unsigned char boot_logo[48 * 48] = {\n' +
    '\n'.join('    ' + ','.join(map(str, values[y:y+48])) + ',' for y in range(0,2304,48)) + '\n};\n')
