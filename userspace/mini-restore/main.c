#include "bootstrap.h"
#include "carrier.h"
#include "cleanup.h"
#include "criu_restore_abi.h"
#include "image_reader.h"
#include "sigframe.h"
#include "staging.h"
#include "validator.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s --images DIR [--dry-run]\n", argv0);
}

static int child_noop(void *arg)
{
	(void)arg;
	return 0;
}

static enum b1_restore_status build_validate_plan(const struct b1_restore_image *image,
						  const struct b1_staging_plan *staging,
						  struct criu_restore_plan_v1 *plan)
{
	memset(plan, 0, sizeof(*plan));
	plan->version = CRIU_RESTORE_ABI_VERSION;
	plan->size = sizeof(*plan);
	plan->vma_count = (uint32_t)staging->vma_count;
	plan->target_pid = image->target_pid;
	plan->vmas_user_ptr = (uint64_t)(uintptr_t)staging->restore_vmas;
	plan->bootstrap_code_start = 0x7000000000ULL;
	plan->bootstrap_code_end = 0x7000001000ULL;
	plan->bootstrap_pc = plan->bootstrap_code_start;
	plan->bootstrap_stack_start = 0x7000001000ULL;
	plan->bootstrap_stack_end = 0x7000003000ULL;
	plan->bootstrap_sp = plan->bootstrap_stack_end;
	plan->sigframe_staging_sp = staging->restore_vmas[0].staging_start +
				    staging->restore_vmas[0].length;
	plan->sigframe_final_sp = staging->restore_vmas[0].target_start +
				  staging->restore_vmas[0].length;
	plan->tls = image->tls;
	return B1_RESTORE_OK;
}

int main(int argc, char **argv)
{
	const char *images = NULL;
	int dry_run = 0;
	struct b1_restore_image image;
	struct b1_staging_plan staging;
	struct b1_cleanup cleanup;
	struct criu_restore_plan_v1 validate_plan;
	struct b1_aarch64_thread_state thread = {0};
	struct b1_aarch64_rt_sigframe sigframe;
	enum b1_restore_status st;
	int restore_fd = -1;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--images") == 0 && i + 1 < argc)
			images = argv[++i];
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!images) {
		usage(argv[0]);
		return 2;
	}

	b1_restore_image_init(&image);
	b1_staging_plan_init(&staging);
	b1_cleanup_init(&cleanup);
	b1_cleanup_set_staging(&cleanup, &staging);

	st = b1_read_images(images, &image);
	if (st == B1_RESTORE_OK)
		st = b1_validate_supported(&image);
	if (st == B1_RESTORE_OK)
		st = b1_stage_image(&image, images, &staging);
	if (st == B1_RESTORE_OK)
		st = build_validate_plan(&image, &staging, &validate_plan);
	if (st == B1_RESTORE_OK) {
		memcpy(thread.regs, image.regs, sizeof(thread.regs));
		thread.sp = image.sp;
		thread.pc = image.pc;
		thread.pstate = image.pstate;
		thread.sigmask = image.sigmask;
		thread.tls = image.tls;
		memcpy(thread.vregs, image.vregs, sizeof(thread.vregs));
		thread.fpsr = image.fpsr;
		thread.fpcr = image.fpcr;
		if (b1_sigframe_build(&thread, &sigframe)) {
			b1_restore_set_diag(&image, B1_RESTORE_FORMAT, "building sigframe");
			st = B1_RESTORE_FORMAT;
		}
	}
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", image.diagnostic);
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 1;
	}

	if (dry_run) {
		printf("B1_RESTORE: DRY_RUN_OK pid=%u vmas=%zu\n",
		       image.target_pid, staging.vma_count);
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 0;
	}

	restore_fd = open("/dev/criu_restore", O_RDWR | O_CLOEXEC);
	if (restore_fd < 0) {
		fprintf(stderr, "IO: open /dev/criu_restore: %s\n", strerror(errno));
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 1;
	}
	b1_cleanup_set_restore_fd(&cleanup, restore_fd);
	if (ioctl(restore_fd, CRIU_RESTORE_VALIDATE_V1, &validate_plan) < 0) {
		fprintf(stderr, "IO: CRIU_RESTORE_VALIDATE_V1: %s\n", strerror(errno));
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 1;
	}
	st = b1_create_exact_pid_carrier(&cleanup.carriers, (pid_t)image.target_pid,
					 child_noop, NULL, &image);
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", image.diagnostic);
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 1;
	}

	fprintf(stderr, "UNSUPPORTED: live bootstrap handoff is gated by Task 10 guest validation\n");
	b1_cleanup_run(&cleanup);
	b1_restore_image_free(&image);
	return 1;
}
