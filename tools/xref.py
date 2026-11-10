#!/usr/bin/env python3
"""Find code that references strings (lis/addi pairs) and the function called right after.

usage: xref.py <private/xx dir> <regex> [max]
Prints, for each matching string, the referencing instruction and the next `bl` target(s).
"""
import collections
import re
import struct
import sys

d, pattern = sys.argv[1], sys.argv[2].encode()
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
BASE = 0x82000000
img = open(f"{d}/image.bin", "rb").read()
TEXT_START, TEXT_END = 0x82140000, 0x82AC3EC0

# string starts matching the pattern
targets = {}
for m in re.finditer(pattern, img):
    s = m.start()
    while s > 0 and img[s - 1] != 0:
        s -= 1
    targets[BASE + s] = img[s:img.index(b"\0", s)][:90]

# index all addi rD,rA,imm preceded (within 8 instr) by lis rA
callees = collections.Counter()
shown = 0
for off in range(TEXT_START - BASE, TEXT_END - BASE, 4):
    w = struct.unpack_from(">I", img, off)[0]
    if (w >> 26) != 14:
        continue
    rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    for k in range(1, 9):
        p = struct.unpack_from(">I", img, off - 4 * k)[0]
        if (p >> 26) == 15 and ((p >> 21) & 31) == ra and ((p >> 16) & 31) == 0:
            addr = ((p & 0xFFFF) << 16) + simm & 0xFFFFFFFF
            if addr in targets:
                bl = None
                for j in range(1, 12):
                    q = struct.unpack_from(">I", img, off + 4 * j)[0]
                    if (q >> 26) == 18 and (q & 3) == 1:
                        li = q & 0x03FFFFFC
                        if li & 0x02000000:
                            li -= 0x04000000
                        bl = BASE + off + 4 * j + li
                        break
                callees[bl] += 1
                if shown < limit:
                    print(f"0x{BASE+off:08X} r{rd} -> {targets[addr]!r}  next bl 0x{bl or 0:08X}")
                    shown += 1
            break
print("\nmost common callees:")
for c, n in callees.most_common(8):
    print(f"  0x{c or 0:08X}  {n}")
