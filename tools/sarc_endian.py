#!/usr/bin/env python3
"""Rewrites a SARC archive (optionally zstd-compressed, .zs) the way ACNH's own are: big-endian headers, and a zstd
frame that records its decompressed size, without a checksum. The game sizes its buffer from that field: archives
compressed from a stream (no size in the frame) crashed it at boot (a null read), as a translation tool wrote them.
The files inside are copied as they are, at the same alignment; only the archive's headers and compression change.
usage: tools/sarc_endian.py <in.sarc[.zs]> <out.sarc[.zs]> [--check]   (--check: only print the byte order)"""
import os
import struct
import subprocess
import sys


def read(path):
    data = open(path, "rb").read()
    if path.endswith(".zs"):
        data = subprocess.run(["zstd", "-d", "-q", "-c", path], capture_output=True, check=True).stdout
    return data


def parse(data):
    if data[:4] != b"SARC":
        raise ValueError("not a SARC")
    e = "<" if data[6:8] == b"\xff\xfe" else ">"
    hlen, _, size, data_off, ver = struct.unpack_from(e + "HHIIH", data, 4)
    m, sfat_len, n, hash_key = struct.unpack_from(e + "4sHHI", data, hlen)
    nodes = [struct.unpack_from(e + "IIII", data, hlen + sfat_len + 16 * i) for i in range(n)]
    sfnt = hlen + sfat_len + 16 * n
    names = data[sfnt + 8:data_off]
    return e, ver, hash_key, nodes, names, data_off, data


def big_endian(data):
    e, ver, hash_key, nodes, names, data_off, raw = parse(data)
    if e == ">":
        return data
    out = bytearray()
    sfat = bytearray(b"SFAT" + struct.pack(">HHI", 0x0C, len(nodes), hash_key))
    for h, attr, start, end in nodes:
        sfat += struct.pack(">IIII", h, attr, start, end)
    sfnt = b"SFNT" + struct.pack(">HH", 0x08, 0) + names
    header_len = 0x14
    body = raw[data_off:]
    total = data_off + len(body)
    head = b"SARC" + struct.pack(">H", header_len) + b"\xfe\xff" + struct.pack(">IIHH", total, data_off, ver, 0)
    out += head + sfat + sfnt
    if len(out) != data_off:  # the names section keeps its length: the data starts where it did
        raise ValueError(f"layout changed ({len(out)} != {data_off})")
    out += body
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else None
    data = read(src)
    if "--check" in sys.argv:
        print(src, "little-endian" if parse(data)[0] == "<" else "big-endian")
        return
    out = big_endian(data)
    if dst.endswith(".zs"):
        # From a file, not a pipe: zstd records the content size only when it knows it.
        raw = dst + ".raw"
        open(raw, "wb").write(out)
        try:
            subprocess.run(["zstd", "-19", "-q", "-f", "--no-check", raw, "-o", dst], check=True)
        finally:
            os.remove(raw)
    else:
        open(dst, "wb").write(out)


if __name__ == "__main__":
    main()
