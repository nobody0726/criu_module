/* SPDX-License-Identifier: MIT */
/*
 * aarch64-only B1 restore bootstrap.  The host build does not assemble this
 * file; tests inspect it and the Linux/aarch64 guest builds it later.
 */

#ifndef __NR_ioctl
#define __NR_ioctl 29
#endif
#ifndef __NR_rt_sigreturn
#define __NR_rt_sigreturn 139
#endif
#ifndef CRIU_RESTORE_COMMIT_V1
#define CRIU_RESTORE_COMMIT_V1 0x40105202
#endif

	.text
	.align 4
	.global b1_restore_bootstrap_start
	.type b1_restore_bootstrap_start, %object
b1_restore_bootstrap_start:
	.global b1_restore_bootstrap_entry
	.type b1_restore_bootstrap_entry, %function
b1_restore_bootstrap_entry:
	/* x0 = struct b1_bootstrap_args * */
	ldr	x19, [x0, #0]		/* restore fd */
	ldr	x20, [x0, #8]		/* commit struct pointer */
	ldr	x21, [x0, #16]		/* final rt_sigframe sp */
	ldr	x22, [x0, #24]		/* TLS / TPIDR_EL0 */
	ldr	x23, [x0, #32]		/* bootstrap stack sp */
	ldr	x24, [x0, #40]		/* target stack sp */
	ldr	x25, [x0, #48]		/* target pc */
	ldr	x26, [x0, #56]		/* target pc instruction */
	ldr	x27, [x0, #64]		/* target stop flag */

	mov	sp, x23
	mov x0, #2
	adr x1, .Lenter
	mov x2, #20
	mov x8, #64
	svc #0
	mov	x0, x19
	movz	x1, #(CRIU_RESTORE_COMMIT_V1 & 0xffff)
	movk	x1, #((CRIU_RESTORE_COMMIT_V1 >> 16) & 0xffff), lsl #16
	mov	x2, x20
	mov	x8, #__NR_ioctl
	svc #0
	cbnz x0, .Lcommit_failed
	mov x0, #2
	adr x1, .Lcommitted
	mov x2, #21
	mov x8, #64
	svc #0
	mov x0, #2
	adr x1, .Ltarget_before
	mov x2, #3
	mov x8, #64
	svc #0
	ldr x1, [x24, #8]
	movz x2, #0x5a7e
	movk x2, #0xb100, lsl #16
	cmp x1, x2
	b.ne .Ltarget_failed
	str xzr, [x24, #16]
	ldr w1, [x25]
	cmp w1, w26
	b.ne .Ltarget_code_failed
	mov x0, #2
	adr x1, .Ltarget_stop_before
	mov x2, #3
	mov x8, #64
	svc #0
	cbz x27, .Ltarget_stop_ok
	ldr w1, [x27]
	cbnz w1, .Ltarget_stop_failed
.Ltarget_stop_ok:
	mov x0, #2
	adr x1, .Ltarget_after
	mov x2, #3
	mov x8, #64
	svc #0
	mov sp, x21
	msr tpidr_el0, x22
	mov	x8, #__NR_rt_sigreturn
	svc #0

1:	b	1b

.Lcommit_failed:
	neg x0, x0
	mov x8, #93
	svc #0
	b 1b
.Ltarget_failed:
	mov x0, #44
	mov x8, #93
	svc #0
	b 1b
.Ltarget_code_failed:
	mov x0, #45
	mov x8, #93
	svc #0
	b 1b
.Ltarget_stop_failed:
	mov x0, #46
	mov x8, #93
	svc #0
	b 1b
.Lenter:
	.ascii "B1_BOOTSTRAP: ENTER\n\0"
.Lcommitted:
	.ascii "B1_BOOTSTRAP: COMMIT\n"
.Ltarget_before:
	.ascii "U0\n"
.Ltarget_after:
	.ascii "U1\n"
.Ltarget_stop_before:
	.ascii "U2\n"

	.global b1_restore_probe_entry
	.type b1_restore_probe_entry, %function
	.p2align 2
b1_restore_probe_entry:
	/* Validate the restored general-purpose register pointers and memory. */
	mov	x0, #2
	adr	x1, .Lprobe_heap_before
	mov	x2, #3
	mov	x8, #64
	svc	#0
	ldr	x1, [x19]
	movz	x2, #0xcafe
	movk	x2, #0xb100, lsl #16
	cmp	x1, x2
	b.ne	.Lprobe_fail
	mov	x0, #2
	adr	x1, .Lprobe_heap_after
	mov	x2, #3
	mov	x8, #64
	svc	#0
	mov	x0, #2
	adr	x1, .Lprobe_stack_before
	mov	x2, #3
	mov	x8, #64
	svc	#0
	ldr	x1, [sp, #8]
	movz	x2, #0x5a7e
	movk	x2, #0xb100, lsl #16
	cmp	x1, x2
	b.ne	.Lprobe_fail
	mov	x0, #2
	adr	x1, .Lprobe_stack_after
	mov	x2, #3
	mov	x8, #64
	svc	#0
	mov	x0, #2
	adr	x1, .Lprobe_tls_before
	mov	x2, #3
	mov	x8, #64
	svc	#0
	mrs	x1, tpidr_el0
	add	x1, x1, #0x10
	ldr	x1, [x1]
	movz	x2, #0xb1b1
	movk	x2, #0xb1b1, lsl #16
	cmp	x1, x2
	b.ne	.Lprobe_fail
	mov	x0, #2
	adr	x1, .Lprobe
	mov	x2, #22
	mov	x8, #64
	svc	#0
	mov	x0, #42
	mov	x8, #93
	svc	#0
	.Lprobe_fail:
	mov	x0, #43
	mov	x8, #93
	svc	#0
1:
	b	1b
.Lprobe:
	.ascii "B1_POST_SIGRETURN\n"
.Lprobe_heap_before:
	.ascii "H0\n"
.Lprobe_heap_after:
	.ascii "H1\n"
.Lprobe_stack_before:
	.ascii "S0\n"
.Lprobe_stack_after:
	.ascii "S1\n"
.Lprobe_tls_before:
	.ascii "T0\n"


	.global b1_restore_bootstrap_end
	.type b1_restore_bootstrap_end, %object
b1_restore_bootstrap_end:
