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
SIGNAL_TIMERS_FLAG = 1
SCOPE_FLAG = 1
PSTREE = 19
TASK = 1
MM = 2
VMA = 3
REGS = 4
FD = 5
FS = 6
CREDS = 7
PAGE = 9
THREAD = 10
SIGACTION = 15
SIGNAL_QUEUE = 16
ITIMERS = 17
POSIX_TIMERS = 18
TASK_IDS = 20
SHMEM_OBJECT = 21
SHMEM_PAGE_RUN = 22
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


def task_ids(pid, vm_id, files_id, fs_id=None, sighand_id=None, flags=0):
    if fs_id is None:
        fs_id = pid
    if sighand_id is None:
        sighand_id = pid
    return struct.pack("<8I", 1, pid, vm_id, files_id, fs_id, sighand_id, flags, 0)


def fixed_path(path, size=512):
    raw = path.encode()
    assert len(raw) < size
    return raw + b"\0" * (size - len(raw))


def scoped(owner, kind, payload):
    return (kind, struct.pack("<II", owner, 0) + payload, SCOPE_FLAG)


def full_process_records(pid, ppid):
    task_rec = struct.pack("<7I2QI", pid, pid, ppid, 1000, 1000, 1000, 1000,
                           0x40000000, 0, 16)
    task_rec += b"\0" * 24
    task_rec += b"".join(struct.pack("<2Q", 0xffffffffffffffff,
                                     0xffffffffffffffff) for _ in range(16))
    task_rec += fixed_path(f"a7-{pid}", 16)
    regs = bytes(336)
    mm = struct.pack("<2I12Q2I", pid, pid, *([0] * 12), 1, 0)
    vma = struct.pack("<3Q5I6Q512s", 0x400000, 0x401000, 0, 5, 0, 0, 2, 0,
                      0, 0, 1, 1, 0, 0, fixed_path("[heap]"))
    page = struct.pack("<Q4I", 0x400000, 1, 4096, 1, 4096) + bytes([pid & 0xff]) * 4096
    fd = struct.pack("<2I5Q512s", 0, 0o20666, 0, 0, 1, 20, 0,
                     fixed_path("/dev/null"))
    fs = fixed_path("/tmp") + fixed_path("/")
    creds = struct.pack("<9I", *([1000] * 8 + [0])) + struct.pack("<10I", *([0] * 10))
    thread = struct.pack("<4IQ8s512s", pid, pid, 8, 0, 0, bytes(8), bytes(512))
    sigactions = struct.pack("<IIII", 1, 64, 48, 0) + b"".join(
        struct.pack("<IIQQQQQ", signo, 0, 0, 0, 0, 0, 0)
        for signo in range(1, 65)
    )
    queue_header = struct.pack("<IIIIIIIIQQ", 1, 1, 0, 0, 0, 0, 136, 128, 0, 0)
    private_queue = struct.pack("<IIIIIIIIQQ", 1, 2, pid, 0, 0, 0, 136, 128, 0, 0)
    itimers = struct.pack("<IIII", 1, 3, 24, 0) + b"".join(
        struct.pack("<IIQQ", kind, 0, 0, 0) for kind in (1, 2, 3)
    )
    posix = struct.pack("<IIII", 1, 0, 56, 0)
    return [
        scoped(pid, TASK, task_rec),
        scoped(pid, REGS, struct.pack("<I", len(regs)) + regs + struct.pack("<Q", 0)),
        scoped(pid, MM, mm),
        scoped(pid, VMA, vma),
        scoped(pid, PAGE, page),
        scoped(pid, FD, fd),
        scoped(pid, FS, fs),
        scoped(pid, CREDS, creds),
        scoped(pid, THREAD, thread),
        scoped(pid, SIGACTION, sigactions),
        scoped(pid, SIGNAL_QUEUE, queue_header),
        scoped(pid, SIGNAL_QUEUE, private_queue),
        scoped(pid, ITIMERS, itimers),
        scoped(pid, POSIX_TIMERS, posix),
    ]


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


def a8_tree():
    return [
        (PSTREE, pstree(400, 0, 400, 400, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (PSTREE, pstree(401, 400, 400, 400, 0)),
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
    scoped_payload = struct.pack("<II", 999, 0) + task(100)
    (out / "a7-owner-mismatch.bin").write_bytes(build([
        (PSTREE, pstree(150, 0, 150, 150, ROOT | SESSION_LEADER | PGRP_LEADER)),
        (TASK, scoped_payload, SCOPE_FLAG),
    ]))
    full = tree_simple() + full_process_records(100, 0) + full_process_records(101, 100)
    (out / "a7-full-multi.bin").write_bytes(
        build(full, flags=PSTREE_FLAG | SIGNAL_TIMERS_FLAG)
    )
    a8_full = tree_simple() + [
        scoped(100, TASK_IDS, task_ids(100, 501, 77)),
        scoped(101, TASK_IDS, task_ids(101, 502, 77)),
    ] + full_process_records(100, 0) + full_process_records(101, 100)
    (out / "a8-full-task-ids.bin").write_bytes(
        build(a8_full, flags=PSTREE_FLAG | SIGNAL_TIMERS_FLAG)
    )
    a8_valid = a8_tree() + [
        scoped(400, TASK_IDS, task_ids(400, 400, 77)),
        scoped(401, TASK_IDS, task_ids(401, 401, 77)),
    ]
    (out / "a8-valid-shared-files.bin").write_bytes(build(a8_valid))
    (out / "a8-duplicate-task-ids.bin").write_bytes(build(
        a8_valid + [scoped(400, TASK_IDS, task_ids(400, 402, 78))]
    ))
    (out / "a8-missing-task-ids.bin").write_bytes(build(
        a8_tree() + [scoped(400, TASK_IDS, task_ids(400, 400, 77))]
    ))
    (out / "a8-nonthread-shared-vm.bin").write_bytes(build(
        a8_tree() + [
            scoped(400, TASK_IDS, task_ids(400, 99, 400)),
            scoped(401, TASK_IDS, task_ids(401, 99, 401)),
        ]
    ))


if __name__ == "__main__":
    main(sys.argv[1])
