#!/usr/bin/env python3
"""Small fixtures for B1 restore UAPI contract tests.

The production restore validator is introduced by later B1 tasks.  Task 1
still needs executable ABI contracts, so this script models the invariants
that must be expressible by the fixed-width UAPI records.
"""

from __future__ import annotations

import argparse
import dataclasses
import enum
import json
import sys


PAGE_SIZE = 4096
ABI_VERSION = 1
MAX_VMAS = 4096
KNOWN_PLAN_FLAGS = 1 << 0
KNOWN_VMA_FLAGS = (1 << 0) | (1 << 1) | (1 << 2)


class VmaKind(enum.IntEnum):
    ANON_PRIVATE = 1
    FILE_PRIVATE = 2
    STACK = 3
    VDSO = 4


@dataclasses.dataclass(frozen=True)
class Vma:
    staging_start: int
    target_start: int
    length: int
    prot: int = 3
    map_flags: int = 2
    kind: int = VmaKind.ANON_PRIVATE
    flags: int = 0


@dataclasses.dataclass(frozen=True)
class Plan:
    version: int = ABI_VERSION
    size: int = 104
    vma_count: int = 1
    flags: int = 0
    target_pid: int = 4242
    reserved0: int = 0
    vmas_user_ptr: int = 0x100000
    bootstrap_code_start: int = 0x7000000000
    bootstrap_code_end: int = 0x7000001000
    bootstrap_pc: int = 0x7000000000
    bootstrap_stack_start: int = 0x7000001000
    bootstrap_stack_end: int = 0x7000003000
    bootstrap_sp: int = 0x7000003000
    sigframe_staging_sp: int = 0x6000002F00
    sigframe_final_sp: int = 0x4000002F00
    tls: int = 0x5000000000


def valid_fixture() -> tuple[Plan, list[Vma]]:
    return Plan(), [Vma(staging_start=0x6000000000, target_start=0x4000000000, length=PAGE_SIZE * 4)]


def validate(plan: Plan, vmas: list[Vma]) -> list[str]:
    errors: list[str] = []
    if plan.version != ABI_VERSION:
        errors.append("bad version")
    if plan.size != 104:
        errors.append("bad plan size")
    if not plan.target_pid:
        errors.append("missing target_pid")
    if plan.vma_count != len(vmas):
        errors.append("vma_count mismatch")
    if plan.vma_count == 0:
        errors.append("zero vma_count")
    if plan.vma_count > MAX_VMAS:
        errors.append("too many vmas")
    if plan.reserved0:
        errors.append("reserved0 must be zero")
    if plan.flags & ~KNOWN_PLAN_FLAGS:
        errors.append("unsupported plan flags")
    if not plan.vmas_user_ptr:
        errors.append("missing vmas_user_ptr")
    if plan.bootstrap_code_start % PAGE_SIZE or plan.bootstrap_code_end % PAGE_SIZE:
        errors.append("bootstrap code not page aligned")
    if plan.bootstrap_stack_start % PAGE_SIZE or plan.bootstrap_stack_end % PAGE_SIZE:
        errors.append("bootstrap stack not page aligned")
    if not plan.bootstrap_code_start < plan.bootstrap_code_end:
        errors.append("bad bootstrap code range")
    if not plan.bootstrap_stack_start < plan.bootstrap_stack_end:
        errors.append("bad bootstrap stack range")
    if not plan.bootstrap_code_start <= plan.bootstrap_pc < plan.bootstrap_code_end:
        errors.append("bootstrap_pc outside bootstrap code")
    if not plan.bootstrap_stack_start < plan.bootstrap_sp <= plan.bootstrap_stack_end:
        errors.append("bootstrap_sp outside bootstrap stack")

    seen: list[tuple[int, int]] = []
    for i, vma in enumerate(vmas):
        if vma.length == 0:
            errors.append(f"vma {i}: zero length")
        for name, value in (
            ("staging_start", vma.staging_start),
            ("target_start", vma.target_start),
            ("length", vma.length),
        ):
            if value % PAGE_SIZE:
                errors.append(f"vma {i}: {name} not page aligned")
        staging_end = vma.staging_start + vma.length
        target_end = vma.target_start + vma.length
        if staging_end > (1 << 64) - 1 or staging_end <= vma.staging_start:
            errors.append(f"vma {i}: staging overflow")
        if target_end > (1 << 64) - 1 or target_end <= vma.target_start:
            errors.append(f"vma {i}: target overflow")
        if vma.kind not in set(item.value for item in VmaKind):
            errors.append(f"vma {i}: unsupported kind")
        if vma.flags & ~KNOWN_VMA_FLAGS:
            errors.append(f"vma {i}: unsupported flags")
        interval = (vma.target_start, target_end)
        for old_start, old_end in seen:
            if interval[0] < old_end and old_start < interval[1]:
                errors.append(f"vma {i}: duplicate target interval")
        seen.append(interval)
    if vmas:
        if not any(v.staging_start < plan.sigframe_staging_sp <= v.staging_start + v.length for v in vmas):
            errors.append("sigframe_staging_sp outside staging VMAs")
        if not any(v.target_start < plan.sigframe_final_sp <= v.target_start + v.length for v in vmas):
            errors.append("sigframe_final_sp outside target VMAs")
    return errors


def emit(name: str) -> int:
    plan, vmas = valid_fixture()
    if name == "valid":
        pass
    elif name == "bad-version":
        plan = dataclasses.replace(plan, version=2)
    elif name == "bad-size":
        plan = dataclasses.replace(plan, size=96)
    elif name == "zero-vmas":
        plan = dataclasses.replace(plan, vma_count=0)
        vmas = []
    elif name == "zero-target-pid":
        plan = dataclasses.replace(plan, target_pid=0)
    elif name == "bad-plan-flags":
        plan = dataclasses.replace(plan, flags=0x80000000)
    elif name == "bad-vma-flags":
        vmas = [dataclasses.replace(vmas[0], flags=0x80000000)]
    elif name == "bad-bootstrap-pc":
        plan = dataclasses.replace(plan, bootstrap_pc=plan.bootstrap_code_end)
    elif name == "bad-bootstrap-sp":
        plan = dataclasses.replace(plan, bootstrap_sp=plan.bootstrap_stack_start)
    elif name == "bad-sigframe-staging":
        plan = dataclasses.replace(plan, sigframe_staging_sp=0x12345000)
    elif name == "bad-sigframe-final":
        plan = dataclasses.replace(plan, sigframe_final_sp=0x12345000)
    elif name == "too-many-vmas":
        plan = dataclasses.replace(plan, vma_count=MAX_VMAS + 1)
        vmas = [
            Vma(
                staging_start=0x6000000000 + i * PAGE_SIZE,
                target_start=0x4000000000 + i * PAGE_SIZE,
                length=PAGE_SIZE,
            )
            for i in range(MAX_VMAS + 1)
        ]
    elif name == "unaligned":
        vmas = [dataclasses.replace(vmas[0], target_start=vmas[0].target_start + 1)]
    elif name == "duplicate-target":
        vmas = [vmas[0], dataclasses.replace(vmas[0], staging_start=0x6000001000)]
        plan = dataclasses.replace(plan, vma_count=2)
    elif name == "overflow":
        vmas = [dataclasses.replace(vmas[0], target_start=(1 << 64) - PAGE_SIZE + 1)]
    elif name == "bad-kind":
        vmas = [dataclasses.replace(vmas[0], kind=99)]
    else:
        raise SystemExit(f"unknown fixture: {name}")

    payload = {
        "plan": dataclasses.asdict(plan),
        "vmas": [dataclasses.asdict(vma) for vma in vmas],
        "errors": validate(plan, vmas),
    }
    json.dump(payload, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture")
    args = parser.parse_args()
    return emit(args.fixture)


if __name__ == "__main__":
    raise SystemExit(main())
