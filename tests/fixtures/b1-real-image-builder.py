#!/usr/bin/env python3
"""Create small CRIU-v1.1 framed protobuf image fixtures.

The fixture deliberately uses the same little-endian framing and image magic
as CRIU.  It is not a second image format: it exercises the reader against
the wire format emitted by CRIU without requiring protobuf-c on the host.
"""

from __future__ import annotations

import argparse
import shutil
import struct
from pathlib import Path

PAGE = 4096
IMG_COMMON_MAGIC = 0x54564319
INVENTORY_MAGIC = 0x58313116
PSTREE_MAGIC = 0x50273030
CORE_MAGIC = 0x55053847
MM_MAGIC = 0x57492820
PAGEMAP_MAGIC = 0x56084025
FILES_MAGIC = 0x56303138


def varint(value: int) -> bytes:
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def field(number: int, value: int) -> bytes:
    return varint(number << 3) + varint(value)


def message_field(number: int, payload: bytes) -> bytes:
    return varint((number << 3) | 2) + varint(len(payload)) + payload


def frame(*records: bytes) -> bytes:
    return b"".join(struct.pack("<I", len(record)) + record for record in records)


def image(path: Path, magic: int, records: list[bytes], inventory: bool = False) -> None:
    head = struct.pack("<I", magic) if inventory else struct.pack(
        "<II", IMG_COMMON_MAGIC, magic
    )
    path.write_bytes(head + frame(*records))


def build(out: Path, case: str) -> None:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    pid = 4242
    regs = b"".join(varint(i) for i in range(31))
    gpregs = (
        message_field(1, regs)
        + field(2, 0x4000008000)
        + field(3, 0x4000000100)
        + field(4, 0x4000000000)
    )
    fpsimd = message_field(1, b"".join(varint(0) for _ in range(64))) + field(2, 0) + field(3, 0)
    ti_aarch64 = (
        field(1, 0)
        + field(2, 0x5000000000)
        + message_field(3, gpregs)
        + message_field(4, fpsimd)
    )
    core = field(1, 3) + message_field(8, ti_aarch64)
    core += message_field(3, field(1, 1) + field(2, 0) + field(3, 0) + field(4, 0) + field(5, 0))

    vma_anon = (
        field(1, 0x4000000000)
        + field(2, 0x4000001000)
        + field(3, 0)
        + field(4, 0)
        + field(5, 3)
        + field(6, 0x22)
        + field(7, (1 << 0) | (1 << 9))
        + field(8, 0)
    )
    vma_file = (
        field(1, 0x4000002000)
        + field(2, 0x4000003000)
        + field(3, 0)
        + field(4, 99)
        + field(5, 5)
        + field(6, 0x12)
        + field(7, (1 << 0) | (1 << 6))
        + field(8, 0)
    )
    mm = (
        field(1, 0)
        + field(2, 0)
        + field(3, 0)
        + field(4, 0)
        + field(5, 0x4000002000)
        + field(6, 0)
        + field(7, 0)
        + field(8, 0)
        + field(9, 0)
        + field(10, 0)
        + field(11, 99)
        + message_field(14, vma_anon)
        + message_field(14, vma_file)
    )
    pagemap_head = field(1, 1)
    pagemap_entry = field(1, 0x4000000000) + field(2, 1)
    if case == "real-compressed-pages":
        pagemap_entry += message_field(6, field(1, 128) + field(2, 128) + field(3, 1))
    pstree = field(1, pid) + field(2, 1) + field(3, pid) + field(4, pid)
    inventory = field(1, 1)

    image(out / "inventory.img", INVENTORY_MAGIC, [inventory], inventory=True)
    image(out / "pstree.img", PSTREE_MAGIC, [pstree])
    if case != "real-missing-core":
        image(out / f"core-{pid}.img", CORE_MAGIC, [core])
    image(out / f"mm-{pid}.img", MM_MAGIC, [mm])
    image(out / f"pagemap-{pid}.img", PAGEMAP_MAGIC, [pagemap_head, pagemap_entry])
    (out / "pages-1.img").write_bytes(b"\0" * PAGE)

    # Modern CRIU embeds reg_file_entry in files.img (the legacy standalone
    # reg-files.img stream is no longer emitted by current dumps).
    backing = out / "backing.bin"
    backing.write_bytes(b"\0" * PAGE)
    reg = (
        field(1, 99)
        + field(2, 0)
        + field(3, 0)
        + message_field(5, field(1, 0))
        + message_field(6, str(backing).encode())
        + field(8, 0)
    )
    files = field(1, 1) + field(2, 99) + message_field(3, reg)
    image(out / "files.img", FILES_MAGIC, [files])

    if case == "real-wrong-arch":
        image(out / f"core-{pid}.img", CORE_MAGIC, [field(1, 1)])
    elif case == "real-shared-mapping":
        shared = vma_anon.replace(field(7, (1 << 0) | (1 << 9)), field(7, (1 << 0) | (1 << 8)))
        image(out / f"mm-{pid}.img", MM_MAGIC, [mm.replace(message_field(14, vma_anon), message_field(14, shared))])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case")
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    build(args.out, args.case)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
