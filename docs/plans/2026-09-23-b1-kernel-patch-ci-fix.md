# B1 kernel patch CI fix

Date: 2026-09-23

## Root cause

`patches/linux-5.10.29/0004-criu-restore-mm-helper.patch` declared the
`kernel/criu_restore.c` add hunk as `@@ -0,0 +1,510 @@`, while the hunk
contained 513 added lines. GNU patch therefore produced a truncated
`criu_restore.c` ending at the opening brace of `criu_restore_init()`.
Linux compilation then failed with:

```text
kernel/criu_restore.c:525:1: error: expected declaration or statement at end of input
```

The same failure was present on the pre-B2 `main` baseline and was unrelated
to B2 process-tree restore code.

## Fix

- Correct the hunk count from 510 to 513.
- Add `tests/b1-patch-integrity.sh`, which checks the declared hunk size and
  requires the complete init tail.
- Run that check from `tests/b1-patch-build.sh`.

## Verification

The following evidence is required:

- the integrity test fails against the old 510-line hunk;
- the integrity test and existing B1 patch contracts pass after the fix;
- a clean Linux 5.10.29 tree extracted from the pinned tarball accepts all
  project patches;
- `kernel/criu_restore.o` compiles in that clean tree;
- the patched source ends with
  `device_initcall(criu_restore_init);`.

The fix does not change B2 behavior or the B1 restore ABI.
