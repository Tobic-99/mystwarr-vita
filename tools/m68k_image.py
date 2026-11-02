#!/usr/bin/env python3
"""m68k_image.py: interleave the Mystic Warriors 68000 ROMs (byte pairs, even/odd) into rom/m68k.bin (2 MB,
big-endian, as the CPU sees it): 128eaa01/02 at 0x000000 (program), 128a03/04 at 0x100000 (data)."""
import os
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
img = bytearray(0x200000)
for even, odd, base in (('128eaa01.20f', '128eaa02.20g', 0), ('128a03.19f', '128a04.19g', 0x100000)):
    e = open(os.path.join(P, 'rom', even), 'rb').read(); o = open(os.path.join(P, 'rom', odd), 'rb').read()
    for i in range(len(e)):
        img[base + 2 * i] = e[i]; img[base + 2 * i + 1] = o[i]
open(os.path.join(P, 'rom', 'm68k.bin'), 'wb').write(img)
print('wrote rom/m68k.bin; SSP %08x PC %08x' % (int.from_bytes(img[0:4], 'big'), int.from_bytes(img[4:8], 'big')))
