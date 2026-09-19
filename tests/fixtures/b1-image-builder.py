#!/usr/bin/env python3
"""Build small B1 mini-restore image fixtures.

These fixtures are intentionally user-space only.  They model the CRIU image
set needed by B1 without teaching the kernel about protobuf or paths.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil


PAGE = 4096


def write_kv(path: Path, values: dict[str, object]) -> None:
    lines = [f"{key}={value}\n" for key, value in values.items()]
    path.write_text("".join(lines), encoding="utf-8")


def build(out: Path, case: str) -> None:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    inventory = {
        "magic": "B1CRIU",
        "arch": "aarch64",
        "tasks": 1,
        "threads": 1,
        "children": 0,
        "namespaces": 0,
        "shared_mm": 0,
        "shared_mappings": 0,
        "vdso_reloc": 0,
        "pac": 0,
        "sve": 0,
        "gcs": 0,
    }
    core = {
        "pid": 4242,
        "tls": "0x5000000000",
        "gpregs": "present",
        "fpsimd": "present",
        "sigmask": "0x0",
    }
    mm = {
        "vma_count": 2,
        "vma0.start": "0x4000000000",
        "vma0.length": str(PAGE),
        "vma0.kind": "anon-private",
        "vma0.shared": 0,
        "vma0.dirty_file_private": 0,
        "vma0.file_stable": 1,
        "vma0.file_size": str(PAGE),
        "vma1.start": "0x4000002000",
        "vma1.length": str(PAGE),
        "vma1.kind": "file-private",
        "vma1.shared": 0,
        "vma1.dirty_file_private": 0,
        "vma1.file_stable": 1,
        "vma1.file_size": str(PAGE * 2),
    }
    pagemap = {
        "runs": 1,
        "run0.addr": "0x4000000000",
        "run0.pages": 1,
        "run0.image": "pages-1.img",
    }

    if case == "valid":
        pass
    elif case == "missing-mm":
        write_kv(out / "inventory.img", inventory)
        write_kv(out / "core.img", core)
        write_kv(out / "pagemap.img", pagemap)
        (out / "pages-1.img").write_bytes(b"\0" * PAGE)
        return
    elif case == "wrong-arch":
        inventory["arch"] = "x86_64"
    elif case == "threads":
        inventory["threads"] = 2
    elif case == "children":
        inventory["children"] = 1
    elif case == "shared-mm":
        inventory["shared_mm"] = 1
    elif case == "namespaces":
        inventory["namespaces"] = 1
    elif case == "shared-mapping":
        inventory["shared_mappings"] = 1
        mm["vma0.shared"] = 1
    elif case == "dirty-file-private":
        mm["vma1.dirty_file_private"] = 1
    elif case == "vdso-reloc":
        inventory["vdso_reloc"] = 1
    elif case == "pac":
        inventory["pac"] = 1
    elif case == "sve":
        inventory["sve"] = 1
    elif case == "gcs":
        inventory["gcs"] = 1
    elif case == "overlap":
        mm["vma1.start"] = "0x4000000800"
    elif case == "unaligned":
        mm["vma0.start"] = "0x4000000001"
    elif case == "page-overflow":
        pagemap["run0.addr"] = "0xfffffffffffff000"
        pagemap["run0.pages"] = 2
    elif case == "bad-field":
        mm["vma_count"] = "not-a-number"
    elif case == "unstable-file":
        mm["vma1.file_stable"] = 0
    elif case == "short-file":
        mm["vma1.file_size"] = 128
    else:
        raise SystemExit(f"unknown case: {case}")

    write_kv(out / "inventory.img", inventory)
    write_kv(out / "core.img", core)
    write_kv(out / "mm.img", mm)
    write_kv(out / "pagemap.img", pagemap)
    (out / "pages-1.img").write_bytes(b"\0" * PAGE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case")
    parser.add_argument("out")
    args = parser.parse_args()
    build(Path(args.out), args.case)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
