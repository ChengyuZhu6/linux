#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Build two-blob fixtures: MKFS NEW_OUTPUT_DIRECTORY."""
from pathlib import Path
import struct
import subprocess
import sys

mkfs = str(Path(sys.argv[1]).resolve())
root = Path(sys.argv[2]).resolve()
root.mkdir(parents=True)


def crc32c(crc, data):
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc


for bs in (4096, 16384):
    out = root / str(bs)
    out.mkdir()
    for i in range(2):
        src = out / f"src-{i}"
        src.mkdir()
        (src / f"data-{i}").write_bytes(bytes((n * 17 + i) % 251 for n in range(65537)))
        subprocess.run([mkfs, f"-b{bs}", f"--chunksize={bs}", "--workers=1",
                        f"--blobdev={out}/blob-{i}", str(out / f"part-{i}.erofs"),
                        str(src)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([mkfs, f"-b{bs}", "--workers=1", str(out / "meta.erofs"),
                    str(out / "part-0.erofs"), str(out / "part-1.erofs")],
                   check=True, stdout=subprocess.DEVNULL)
    data = (out / "meta.erofs").read_bytes()
    assert struct.unpack_from("<H", data, 1024 + 86)[0] == 2, "need two external devices"
    # Keep the superblock valid so creation fails after all blob devices open.
    bad = bytearray(data)
    assert crc32c(0x5045b54a, bad[1032:bs]) == struct.unpack_from("<I", bad, 1028)[0]
    nid = struct.unpack_from("<H", bad, 1038)[0]
    meta = struct.unpack_from("<I", bad, 1064)[0]
    mode = meta * bs + nid * 32 + 4
    assert struct.unpack_from("<H", bad, mode)[0] & 0xf000 == 0x4000
    struct.pack_into("<H", bad, mode, 0)
    struct.pack_into("<I", bad, 1028, crc32c(0x5045b54a, bad[1032:bs]))
    (out / "bad-meta.erofs").write_bytes(bad)
    for name in ("meta.erofs", "blob-0", "blob-1"):
        with (out / (name + ".pmem")).open("wb") as pmem:
            pmem.write((out / name).read_bytes())
            pmem.truncate(128 * 1024 * 1024)
    print(f"Prepared {bs}-byte block fixtures in {out}")
