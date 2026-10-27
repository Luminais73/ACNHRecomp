#!/usr/bin/env python3
"""Packs a spirv folder's .spv files into one spirv.pak, which the runtime reads instead of the folder
(nvn_shaders.cpp): on Android the data folder is on shared storage, where opening each of the ~9,000 small files
took milliseconds. Header "SPK1" and the count (u32 each), then per file 16 bytes of its hash (the name without .spv),
its offset and size (u64 each), then the files. Little-endian.
usage: tools/pack_spirv.py <spirv folder> <out spirv.pak>"""
import os
import struct
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    names = sorted(n for n in os.listdir(src) if n.endswith(".spv") and len(n) - 4 <= 16)
    off = 8 + 32 * len(names)
    index, sizes = [], []
    for n in names:
        size = os.path.getsize(os.path.join(src, n))
        index.append(struct.pack("<16sQQ", n[:-4].encode(), off, size))
        off += size
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(struct.pack("<II", 0x314B5053, len(names)))
        f.write(b"".join(index))
        for n in names:
            with open(os.path.join(src, n), "rb") as s:
                f.write(s.read())
    os.replace(tmp, out)
    print(f"{out}: {len(names)} shaders, {off >> 20} MiB")


if __name__ == "__main__":
    main()
