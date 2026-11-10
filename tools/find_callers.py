#!/usr/bin/env python3
"""Find `bl` callers of an import thunk in a bo3prep image dump and map them to .pdata functions.

usage: find_callers.py <private/xx dir> <import name>
"""
import struct
import sys

d, name = sys.argv[1], sys.argv[2]
info = open(f"{d}/info.txt", encoding="latin-1").read().split()
base = int(info[info.index("base") + 1], 16)
img = open(f"{d}/image.bin", "rb").read()

thunk = None
for line in open(f"{d}/imports.txt"):
    a, n = line.split()
    if n.endswith(name):
        thunk = int(a, 16)
if thunk is None:
    sys.exit(f"{name} not imported")

secs = {}
for line in open(f"{d}/info.txt", encoding="latin-1"):
    p = line.split()
    if len(p) == 5 and p[2] == "size":
        secs[p[0]] = (int(p[1], 16), int(p[3], 16))
pb, ps = secs[".pdata"][0], secs[".pdata"][1]
funcs = []
for off in range(pb - base, pb - base + ps, 8):
    begin, fl = struct.unpack_from(">II", img, off)
    if begin == 0:
        continue
    funcs.append((begin, ((fl >> 8) & 0x3FFFFF) * 4))
funcs.sort()


def owner(addr):
    lo, hi = 0, len(funcs) - 1
    best = None
    while lo <= hi:
        m = (lo + hi) // 2
        if funcs[m][0] <= addr:
            best = funcs[m]; lo = m + 1
        else:
            hi = m - 1
    if best and addr < best[0] + best[1]:
        return best
    return None


tb, ts = secs[".text"][0], secs[".text"][1]
print(f"{name} thunk @ 0x{thunk:08X}; {len(funcs)} .pdata functions")
for off in range(tb - base, tb - base + ts, 4):
    ins = struct.unpack_from(">I", img, off)[0]
    if (ins >> 26) == 18 and (ins & 3) == 1:  # bl
        li = ins & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        addr = base + off
        if addr + li == thunk:
            f = owner(addr)
            fs = f"func 0x{f[0]:08X} size 0x{f[1]:X}" if f else "no .pdata entry"
            print(f"  call @ 0x{addr:08X}  ({fs})")
