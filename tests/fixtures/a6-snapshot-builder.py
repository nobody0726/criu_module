#!/usr/bin/env python3
import hashlib
import pathlib
import struct
import sys

MAGIC = 0x43524955534E5033
VERSION = 1
HEADER = 64
FOOTER = 24
A6_FLAG = 1

SIGACTION = 15
SIGNAL_QUEUE = 16
ITIMERS = 17
POSIX_TIMERS = 18
END = 0xFFFF


def checksum(blob):
    return int.from_bytes(hashlib.sha256(blob).digest()[:8], "little")


def tlv(kind, payload):
    return struct.pack("<HHIQ", kind, 0, 0, len(payload)) + payload


def sigactions():
    header = struct.pack("<IIII", 1, 64, 48, 0)
    entries = b"".join(
        struct.pack("<IIQQQQQ", signo, 0, 0, 0, 0, 0, 0)
        for signo in range(1, 65)
    )
    return header + entries


def queue(scope, owner, total=1, first=0, count=1, mask=1):
    header = struct.pack(
        "<IIIIIIIIQQ", 1, scope, owner, total, first, count, 136, 128, mask, 0
    )
    entry = struct.pack("<II", 10, 0) + struct.pack("<i", 10) + bytes(124)
    return header + entry * count


def itimers():
    header = struct.pack("<IIII", 1, 3, 24, 0)
    entries = b"".join(struct.pack("<IIQQ", kind, 0, 0, 0) for kind in (1, 2, 3))
    return header + entries


def posix_timers():
    header = struct.pack("<IIII", 1, 1, 56, 0)
    entry = struct.pack(
        "<IIIIIIIIQQQ", 1, 1, 10, 4, 1, 2, 0, 0, 0x1234, 1000000, 500000
    )
    return header + entry


def posix_timer_with_target(tid):
    header = struct.pack("<IIII", 1, 1, 56, 0)
    entry = struct.pack(
        "<IIIIIIIIQQQ", 1, 1, 10, 4, 2, 0, tid, 0, 0, 0, 0
    )
    return header + entry


def fixed_path(path, size=512):
    raw = path.encode()
    assert len(raw) < size
    return raw + b"\0" * (size - len(raw))


def base_records():
    """Build the smallest complete process model accepted by the converter."""
    records = []
    task = struct.pack("<7I2QI", 1234, 1234, 1, 1000, 1000, 1000, 1000,
                       0x40000000, 0, 16)
    task += b"\0" * 24
    task += b"".join(struct.pack("<2Q", 0xffffffffffffffff,
                                 0xffffffffffffffff) for _ in range(16))
    task += fixed_path("a6-full", 16)
    records.append((1, task))

    regs = bytearray(336)
    for i in range(31):
        struct.pack_into("<Q", regs, i * 8, 0x1000 + i)
    struct.pack_into("<Q", regs, 248, 0x700000)
    struct.pack_into("<Q", regs, 256, 0x400120)
    struct.pack_into("<Q", regs, 264, 0x60001000)
    records.append((4, struct.pack("<I", len(regs)) + regs +
                    struct.pack("<Q", 0x12345000)))
    records.append((2, struct.pack("<2I12Q2I", 1234, 1234,
                                   *([0] * 12), 1, 0)))
    records.append((3, struct.pack(
        "<3Q5I6Q512s", 0x400000, 0x401000, 0, 5, 0, 0, 2, 0,
        0, 0, 1, 0, 0, 0, fixed_path("[heap]"))))
    records.append((5, struct.pack(
        "<2I5Q512s", 0, 0o20666, 0, 0, 1, 20, 0,
        fixed_path("/dev/null"))))
    records.append((6, fixed_path("/tmp") + fixed_path("/")))
    records.append((7, struct.pack("<9I", *([1000] * 8 + [0])) +
                    struct.pack("<10I", *([0] * 10))))
    return records


def build(records, flags=A6_FLAG):
    body = b"".join(tlv(kind, payload) for kind, payload in records)
    body += tlv(END, b"")
    total = HEADER + len(body) + FOOTER
    head0 = struct.pack(
        "<QIHHIIIIQIIQQ",
        MAGIC, VERSION, HEADER, flags, 0x3E, 4096, 1234, 1234,
        7, len(records) + 1, 0, total, 0,
    )
    digest = checksum(head0 + body)
    head = head0[:-8] + struct.pack("<Q", digest)
    footer = struct.pack("<QIIQ", MAGIC, VERSION, len(records) + 1, digest)
    return head + body + footer


def valid_records():
    return base_records() + [
        (SIGACTION, sigactions()),
        (SIGNAL_QUEUE, queue(1, 0)),
        (SIGNAL_QUEUE, queue(2, 1234)),
        (ITIMERS, itimers()),
        (POSIX_TIMERS, posix_timers()),
    ]


def main(out_dir):
    out = pathlib.Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "a6-valid.bin").write_bytes(build(valid_records()))
    records = valid_records()
    (out / "a6-missing.bin").write_bytes(build([r for r in records if r[0] != ITIMERS]))
    (out / "a6-duplicate.bin").write_bytes(build(records + [(ITIMERS, itimers())]))
    bad_queue = queue(2, 1234, total=3, first=2, count=1)
    (out / "a6-queue-gap.bin").write_bytes(
        build([r for r in records if r[0] != SIGNAL_QUEUE]
              + [(SIGNAL_QUEUE, queue(2, 1234, total=3, first=0)),
                 (SIGNAL_QUEUE, bad_queue)])
    )
    bad_siginfo = bytearray(queue(1, 0))
    struct.pack_into("<I", bad_siginfo, 28, 127)
    (out / "a6-bad-siginfo.bin").write_bytes(
        build([r if r[0] != SIGNAL_QUEUE else (r[0], bytes(bad_siginfo))
               for r in records])
    )
    (out / "a6-unknown-mandatory.bin").write_bytes(
        build([(99, b"unknown")] + records, flags=0)
    )
    (out / "a6-unknown-header-flag.bin").write_bytes(build(valid_records(), flags=2))
    (out / "a6-record-without-flag.bin").write_bytes(build(valid_records(), flags=0))
    (out / "a6-bad-notify-tid.bin").write_bytes(
        build([r if r[0] != POSIX_TIMERS else
               (r[0], posix_timer_with_target(9999)) for r in records])
    )


if __name__ == "__main__":
    main(sys.argv[1])
