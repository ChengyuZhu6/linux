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
    for name in ("meta.erofs", "blob-0", "blob-1"):
        with (out / (name + ".pmem")).open("wb") as pmem:
            pmem.write((out / name).read_bytes())
            pmem.truncate(128 * 1024 * 1024)
    print(f"Prepared {bs}-byte block fixtures in {out}")
