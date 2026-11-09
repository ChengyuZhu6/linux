#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Generate the EROFS block/page-size matrix: MKFS DUMP NEW_OUTPUT_DIRECTORY."""
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile

mkfs, dump = (str(Path(p).resolve()) for p in sys.argv[1:3])
root = Path(sys.argv[3]).resolve()
root.mkdir(parents=True)
src = root / "src"
src.mkdir()
sizes = (0, 1, 4096, 4097, 16384, 16385, 65536, 65537)
for seed, size in enumerate(sizes):
    (src / f"data-{size}").write_bytes(
        bytes((i * 17 + (i >> 8) + seed) % 251 for i in range(size)))
(src / "xattr-file").write_bytes(
    bytes((i * 17 + (i >> 8) + 31) % 251 for i in range(8197)))
(src / "tree").mkdir()
for i in range(300):
    (src / "tree" / (f"entry-{i:04d}-" + "n" * 210)).write_bytes(struct.pack("<I", i))
os.link(src / "data-4097", src / "hardlink")
os.symlink("data-4097", src / "symlink")
os.symlink("l" * 1000, src / "long-link")
images, rows, file_images = {}, [], []


def build(name, blocksize, options):
    image = root / (name + ".erofs")
    source = root / f"source-{blocksize}.tar"
    if not source.exists():
        def metadata(info):
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            if info.name.endswith("/xattr-file"):
                info.pax_headers["SCHILY.xattr.user.boundary"] = "x" * min(blocksize // 2 + 17, 8209)
            return info
        with tarfile.open(source, "w", format=tarfile.PAX_FORMAT) as archive:
            archive.add(src, arcname=".", filter=metadata)
    subprocess.run([mkfs, f"-b{blocksize}", "--workers=1", "--tar=f", "--sort=path",
                    "-T0", "-Uclear", "--all-root", *options, str(image), str(source)],
                   check=True, stdout=subprocess.DEVNULL)
    images[name] = image


def case(name, label, path, size, seed, dax4, dax16, mode="always", file=False):
    rows.append((name, label, path, size, seed, dax4, dax16, mode, file))


for bs in (4096, 16384, 65536):
    for mode, options in (
        ("plain", ["-E^inline_data,force-inode-extended"]),
        ("inline", ["-Eforce-inode-extended"]),
        ("chunk", [f"--chunksize={bs}", "-Eforce-inode-extended"]),
    ):
        name = f"{bs}-{mode}"
        build(name, bs, options)
        file_images.append(name)
        if mode == "plain":
            for seed, size in enumerate(sizes):
                case(name, f"{name}-{size}", f"data-{size}", size, seed, 1, int(bs >= 16384))
            for path in ("hardlink", "symlink"):
                case(name, name + "-" + path, path, 4097, 3, 1, int(bs >= 16384))
            case(name, name + "-never", "data-65537", 65537, 7, 0, 0, "never")
        else:
            case(name, name + "-data", "data-1", 1, 1,
                 int(mode == "chunk" and bs == 4096),
                 int(mode == "chunk" and bs == 16384))
        special = (("tree", 0, 2**32 - 1), ("xattr-file", 8197, 31),
                   ("long-link", 1000, 2**32 - 2))
        for path, size, seed in special:
            case(name, name + "-" + path, path, size, seed, 0, 0, "never")
        for path, size, seed in (("data-1", 1, 1), ("data-4097", 4097, 3),
                                 ("data-65537", 65537, 7), *special):
            case(name, name + "-file-" + path, path, size, seed, 0, 0, "never", True)
    name = f"{bs}-ishare"
    build(name, bs, ["-E^inline_data,force-inode-extended", "--xattr-inode-digest"])
    file_images.append(name)
    for file in (False, True):
        case(name, name + ("-file" if file else "-pmem"), "data-65537", 65537, 7,
             0, 0, "never,domain_id=erofs-lbs,inode_share", file)


def first_data(image):
    text = subprocess.check_output([dump, "-e", "--path=/data-1", str(image)], text=True)
    match = re.search(r"^\s*0:\s+0\.\.\s*\d+\s*\|\s*\d+\s*:\s*(\d+)\.\.",
                      text, re.MULTILINE)
    assert match, text
    return int(match[1])


def crc32c(crc, data):
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc


for bs in (16384, 65536):
    for mode in ("plain", "chunk"):
        data = bytearray(images[f"{bs}-{mode}"].read_bytes())
        pos = first_data(images[f"{bs}-{mode}"]) + 1
        data[pos:pos + 16] = b"NEIGHBOR_SECRET!"
        name = f"bad-{bs}-{mode}"
        images[name] = root / (name + ".erofs")
        images[name].write_bytes(data)
        file_images.append(name)
        for opt in ("always", "never"):
            case(name, name + "-" + opt, "data-1", 1, 1, -2, -2, opt)
        case(name, name + "-file", "data-1", 1, 1, -2, -2, "never", True)
    for kind in ("crc", "bits", "bounds"):
        data = bytearray(images[f"{bs}-plain"].read_bytes())
        if kind == "crc":
            data[1048] ^= 1
            expected = -3
        elif kind == "bits":
            data[1036] = 17
            expected = -1
        else:
            assert crc32c(0x5045b54a, data[1032:bs]) == struct.unpack_from("<I", data, 1028)[0]
            struct.pack_into("<I", data, 1060, 1)
            struct.pack_into("<I", data, 1028, crc32c(0x5045b54a, data[1032:bs]))
            expected = -2
        name = f"bad-{bs}-{kind}"
        images[name] = root / (name + ".erofs")
        images[name].write_bytes(data)
        case(name, name, "data-1", 1, 1, expected, expected)
for bs in (4096, 16384):
    name = f"{bs}-compressed"
    build(name, bs, ["-zlz4", "-Eforce-inode-extended"])
    case(name, name, "data-65537", 65537, 7, 0 if bs == 4096 else -4, 0, "never")

offsets, pos = {}, 0
with (root / "fixtures.pmem").open("wb") as backing:
    backing.truncate(512 * 1024 * 1024)
    for name, image in images.items():
        data = image.read_bytes()
        offsets[name] = pos
        backing.seek(pos)
        backing.write(data)
        pos = (pos + len(data) + 2097151) // 2097152 * 2097152
    assert pos <= 512 * 1024 * 1024
with (root / "cases").open("w") as cases:
    for name, label, path, size, seed, d4, d16, mode, file in rows:
        device = "/images/" + name + ".erofs" if file else "/dev/pmem0"
        opts = f"dax={mode}" if file else f"dax={mode},fsoffset={offsets[name]}"
        cases.write(f"{device} {label} /mnt/{path} {size} {seed} {d4} {d16} {opts}\n")
with tarfile.open(root / "images.tar.gz", "w:gz", compresslevel=1) as archive:
    for name in file_images:
        archive.add(images[name], arcname="images/" + name + ".erofs")
(root / "sha256.txt").write_text("".join(
    hashlib.sha256(image.read_bytes()).hexdigest() + "  " + image.name + "\n"
    for image in images.values()))
print(f"Prepared {len(rows)} cases in {root}")
