#!/usr/bin/env python3
import hashlib
import pathlib
import struct
import sys

MAGIC = 0x43524955534E5033
VERSION = 1
HEADER = 64
FOOTER = 24
PSTREE_FLAG = 1 << 1
SCOPE_FLAG = 1
PSTREE = 19
TASK = 1
END = 0xFFFF

ROOT = 1
EXTERNAL_PARENT = 2
SESSION_LEADER = 4
PGRP_LEADER = 8


def digest(blob):
    return int.from_bytes(hashlib.sha256(blob).digest()[:8], "little")


def tlv(kind, payload, flags=0):
    return struct.pack("<HHIQ", kind, flags, 0, len(payload)) + payload


def pstree(pid, ppid, pgid, sid, flags, born_sid=-1, namespace=1):
    return struct.pack(
        "<7Ii4I",
        1, flags, pid, pid, ppid, pgid, sid, born_sid,
        pid, 1, namespace, 0,
    )


def task(pid):
    return struct.pack("<3I", pid, pid, 0)


def build(records, flags=PSTREE_FLAG):
    body = b"".join(tlv(*record) for record in records) + tlv(END, b"")
    total = HEADER + len(body) + FOOTER
    header0 = struct.pack(
        "<QIHHIIIIQIIQQ",
        MAGIC, VERSION, HEADER, flags, 0xB7, 4096, 100, 100,
        1, len(records) + 1, 0, total, 0,
    )
    checksum = digest(header0 + body)
    header = header0[:-8] + struct.pack("<Q", checksum)
    footer = struct.pack("<QIIQ", MAGIC, VERSION, len(records) + 1, checksum)
    return header + body + footer


def tree_simple():
    return [
        (PSTREE, pstree(100, 0, 100, 100, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(101, 100, 100, 100, 0)),
    ]


def tree_session():
    return [
        (PSTREE, pstree(200, 0, 200, 200, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(201, 200, 201, 201, SESSION_LEADER | PGRP_LEADER)),
    ]


def main(out_dir):
    out = pathlib.Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "a7-simple.bin").write_bytes(build(tree_simple()))
    (out / "a7-session.bin").write_bytes(build(tree_session()))
    (out / "a7-pgid.bin").write_bytes(build([
        (PSTREE, pstree(300, 0, 300, 300, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(301, 300, 300, 300, 0)),
        (PSTREE, pstree(302, 300, 300, 300, 0)),
    ]))
    records = tree_simple()
    (out / "a7-duplicate-pid.bin").write_bytes(build(
        records + [(PSTREE, pstree(101, 100, 100, 100, 0))]
    ))
    (out / "a7-missing-parent.bin").write_bytes(build([
        (PSTREE, pstree(110, 0, 110, 110, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(111, 999, 110, 110, 0)),
    ]))
    (out / "a7-missing-leader.bin").write_bytes(build([
        (PSTREE, pstree(120, 0, 120, 120, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(121, 120, 999, 120, 0)),
    ]))
    (out / "a7-cross-namespace.bin").write_bytes(build([
        (PSTREE, pstree(130, 0, 130, 130,
                         ROOT | SESSION_LEADER | PGRP_LEADER, namespace=2)),
    ]))
    (out / "a7-born-sid-conflict.bin").write_bytes(build([
        (PSTREE, pstree(140, 0, 140, 140, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(141, 140, 141, 141, SESSION_LEADER | PGRP_LEADER,
                         born_sid=999)),
    ]))
    scoped = struct.pack("<II", 999, 0) + task(100)
    (out / "a7-owner-mismatch.bin").write_bytes(build([
        (PSTREE, pstree(150, 0, 150, 150, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (TASK, scoped, SCOPE_FLAG),
    ]))


if __name__ == "__main__":
    main(sys.argv[1])
