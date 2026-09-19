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
	.global b1_restore_bootstrap_entry
	.type b1_restore_bootstrap_entry, %function
b1_restore_bootstrap_entry:
	/* x0 = struct b1_bootstrap_args * */
	ldr	x19, [x0, #0]		/* restore fd */
	ldr	x20, [x0, #8]		/* commit struct pointer */
	ldr	x21, [x0, #16]		/* final rt_sigframe sp */
	ldr	x22, [x0, #24]		/* TLS / TPIDR_EL0 */

	mov	x0, x19
	mov	x1, #CRIU_RESTORE_COMMIT_V1
	mov	x2, x20
	mov	x8, #__NR_ioctl
	svc #0

	mov sp, x21
	msr tpidr_el0, x22
	mov	x8, #__NR_rt_sigreturn
	svc #0

1:	b	1b
