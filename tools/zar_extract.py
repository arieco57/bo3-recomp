#!/usr/bin/env python3
"""
Minimal extractor for ZArchive (.zar) files (format by Exzap, used by Cemu/Xenia packs).

Layout (all big-endian):
  [compressed data: 64 KiB blocks, each zstd-compressed or stored raw]
  [offset records: { u64 baseOffset; u16 sizeMinus1[16]; } per 16 blocks]
  [names table]  [file tree: 16-byte entries]  [meta dir] [meta data]
  [footer: 6 x {u64 offset,u64 size}, u8 sha256[32], u64 totalSize, u32 version, u32 magic]

Usage:
  zar_extract.py ARCHIVE.zar            -> list files
  zar_extract.py ARCHIVE.zar OUTDIR     -> extract everything
  zar_extract.py ARCHIVE.zar --verify   -> check SHA-256 integrity hash
"""
import hashlib
import os
import struct
import sys

import zstandard

MAGIC = 0x169F52D6
VERSION = 0x61BF3A01
BLOCK = 64 * 1024
PER_REC = 16
FOOTER_SIZE = 6 * 16 + 32 + 8 + 4 + 4  # 144


class ZArchive:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.file_size = self.f.tell()
        self.f.seek(self.file_size - FOOTER_SIZE)
        ft = self.f.read(FOOTER_SIZE)
        secs = [struct.unpack_from(">QQ", ft, i * 16) for i in range(6)]
        self.hash = ft[96:128]
        self.total_size, self.version, self.magic = struct.unpack_from(">QII", ft, 128)
        if self.magic != MAGIC or self.version != VERSION:
            raise ValueError("not a ZArchive (bad magic/version)")
        if self.total_size != self.file_size:
            raise ValueError(f"size mismatch: footer says {self.total_size}, file is {self.file_size}")
        (self.data_off, self.data_size), recs, names, tree = secs[0], secs[1], secs[2], secs[3]

        self.f.seek(recs[0]); raw = self.f.read(recs[1])
        self.records = []
        for i in range(0, len(raw), 8 + 2 * PER_REC):
            base = struct.unpack_from(">Q", raw, i)[0]
            sizes = struct.unpack_from(">16H", raw, i + 8)
            self.records.append((base, sizes))

        self.f.seek(names[0]); self.names = self.f.read(names[1])
        self.f.seek(tree[0]); traw = self.f.read(tree[1])
        self.tree = [traw[i:i + 16] for i in range(0, len(traw), 16)]
        self.dctx = zstandard.ZstdDecompressor()
        self._cache_idx, self._cache = -1, b""

    def _name(self, off):
        if off == 0x7FFFFFFF:
            return ""
        b0 = self.names[off]
        if b0 & 0x80:
            ln = (b0 & 0x7F) | (self.names[off + 1] << 7)
            off += 2
        else:
            ln = b0
            off += 1
        return self.names[off:off + ln].decode("utf-8", "replace")

    def _block(self, idx):
        if idx == self._cache_idx:
            return self._cache
        base, sizes = self.records[idx // PER_REC]
        sub = idx % PER_REC
        off = base + sum(s + 1 for s in sizes[:sub])
        csize = sizes[sub] + 1
        self.f.seek(self.data_off + off)
        cdata = self.f.read(csize)
        data = cdata if csize == BLOCK else self.dctx.decompress(cdata, max_output_size=BLOCK)
        self._cache_idx, self._cache = idx, data
        return data

    def read_range(self, offset, size, out):
        while size > 0:
            idx, inner = divmod(offset, BLOCK)
            blk = self._block(idx)
            n = min(size, len(blk) - inner)
            if n <= 0:
                raise IOError(f"short block {idx}")
            out.write(blk[inner:inner + n])
            offset += n
            size -= n

    def walk(self, idx=0, prefix=""):
        e = self.tree[idx]
        tn = struct.unpack_from(">I", e, 0)[0]
        is_file = bool(tn & 0x80000000)
        name = self._name(tn & 0x7FFFFFFF)
        path = f"{prefix}/{name}" if prefix else name
        if is_file:
            off_lo, size_lo, size_hi, off_hi = struct.unpack_from(">IIHH", e, 4)
            yield path, False, (off_hi << 32) | off_lo, (size_hi << 32) | size_lo
        else:
            start, count = struct.unpack_from(">II", e, 4)
            if idx != 0:
                yield path, True, 0, 0
            for c in range(start, start + count):
                yield from self.walk(c, path)

    def verify(self):
        h = hashlib.sha256()
        self.f.seek(0)
        remaining = self.file_size - FOOTER_SIZE
        while remaining:
            chunk = self.f.read(min(remaining, 16 << 20))
            h.update(chunk)
            remaining -= len(chunk)
        ft = bytearray(self.f.read(FOOTER_SIZE))
        ft[96:128] = b"\0" * 32
        h.update(ft)
        return h.digest() == self.hash


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    z = ZArchive(sys.argv[1])
    if len(sys.argv) == 2:
        for p, d, o, s in z.walk():
            print(f"{'<DIR>' if d else s:>12}  {p}")
        return
    if sys.argv[2] == "--verify":
        ok = z.verify()
        print("integrity OK" if ok else "integrity hash MISMATCH")
        sys.exit(0 if ok else 2)
    out = sys.argv[2]
    total = 0
    for p, d, o, s in z.walk():
        dst = os.path.join(out, p)
        if d:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fo:
            z.read_range(o, s, fo)
        total += s
        print(f"{s:>12}  {p}", flush=True)
    print(f"done: {total} bytes extracted to {out}")


if __name__ == "__main__":
    main()
