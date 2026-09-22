#define _GNU_SOURCE

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
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define B1_BOOTSTRAP_CODE_START 0x7000000000ULL
#define B1_BOOTSTRAP_CODE_END   0x7000001000ULL
#define B1_BOOTSTRAP_STACK_START 0x7000001000ULL
#define B1_BOOTSTRAP_STACK_END   0x7000003000ULL
#define B1_SIGFRAME_BOOTSTRAP_SP  (B1_BOOTSTRAP_STACK_START + 0x100ULL)

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#if defined(__aarch64__) && defined(__linux__)
extern const unsigned char b1_restore_bootstrap_start[];
extern const unsigned char b1_restore_bootstrap_end[];
extern const unsigned char b1_restore_probe_entry[];

#endif

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s --images DIR [--dry-run]\n", argv0);
}

static enum b1_restore_status build_validate_plan(const struct b1_restore_image *image,
						  const struct b1_staging_plan *staging,
						  uint64_t sigframe_staging_sp,
						  uint64_t sigframe_final_sp,
						  struct criu_restore_plan_v1 *plan)
{
	memset(plan, 0, sizeof(*plan));
	plan->version = CRIU_RESTORE_ABI_VERSION;
	plan->size = sizeof(*plan);
	plan->vma_count = (uint32_t)staging->vma_count;
	plan->target_pid = image->target_pid;
	plan->vmas_user_ptr = (uint64_t)(uintptr_t)staging->restore_vmas;
	plan->bootstrap_code_start = B1_BOOTSTRAP_CODE_START;
	plan->bootstrap_code_end = B1_BOOTSTRAP_CODE_END;
	plan->bootstrap_pc = plan->bootstrap_code_start;
	plan->bootstrap_stack_start = B1_BOOTSTRAP_STACK_START;
	plan->bootstrap_stack_end = B1_BOOTSTRAP_STACK_END;
	plan->bootstrap_sp = B1_BOOTSTRAP_STACK_END - 16U;
	plan->sigframe_staging_sp = sigframe_staging_sp;
	plan->sigframe_final_sp = sigframe_final_sp;
	plan->tls = image->tls;
	return B1_RESTORE_OK;
}

#if defined(__aarch64__) && defined(__linux__)
static enum b1_restore_status map_bootstrap(struct b1_restore_image *image,
						    void **code, void **stack)
{
	size_t code_len = (size_t)(b1_restore_bootstrap_end -
					  b1_restore_bootstrap_start);
	void *mapped_code;
	void *mapped_stack;

	if (!code_len || code_len > B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START) {
		b1_restore_set_diag(image, B1_RESTORE_FORMAT,
					"bootstrap code exceeds reserved mapping");
		return B1_RESTORE_FORMAT;
	}
	mapped_code = mmap((void *)(uintptr_t)B1_BOOTSTRAP_CODE_START,
				   B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				   -1, 0);
	if (mapped_code == MAP_FAILED) {
		b1_restore_set_diag(image, B1_RESTORE_IO,
					"mapping fixed bootstrap code");
		return B1_RESTORE_IO;
	}
	mapped_stack = mmap((void *)(uintptr_t)B1_BOOTSTRAP_STACK_START,
				    B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START,
				    PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				    -1, 0);
	if (mapped_stack == MAP_FAILED) {
		munmap(mapped_code, B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START);
		b1_restore_set_diag(image, B1_RESTORE_IO,
					"mapping fixed bootstrap stack");
		return B1_RESTORE_IO;
	}
	memcpy(mapped_code, b1_restore_bootstrap_start, code_len);
	if (mprotect(mapped_code, B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START,
			     PROT_READ | PROT_EXEC) < 0) {
		munmap(mapped_stack, B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START);
		munmap(mapped_code, B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START);
		b1_restore_set_diag(image, B1_RESTORE_IO,
					"protecting fixed bootstrap code");
		return B1_RESTORE_IO;
	}
	*code = mapped_code;
	*stack = mapped_stack;
	return B1_RESTORE_OK;
}
#endif

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
	uint64_t sigframe_staging_sp = 0;
	uint64_t sigframe_final_sp = 0;
	uint64_t target_pc_word = 0;
#if defined(__aarch64__) && defined(__linux__)
	void *bootstrap_code = NULL;
	void *bootstrap_stack = NULL;
#endif
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
	if (st == B1_RESTORE_OK && !dry_run) {
		size_t j;
		for (j = 0; j < image.vma_count; j++)
			fprintf(stderr, "B1_VMA_DETAIL[%zu]: start=0x%llx len=0x%llx pgoff=0x%llx prot=%u fd=%d kind=%d\\n", j,
				(unsigned long long)image.vmas[j].start,
				(unsigned long long)image.vmas[j].length,
				(unsigned long long)image.vmas[j].pgoff,
				image.vmas[j].prot, image.vmas[j].backing_fd,
				image.vmas[j].kind);
		for (j = 0; j < image.page_run_count; j++)
			fprintf(stderr, "B1_RUN[%zu]: addr=0x%llx pages=%llu off=%llu\\n", j,
				(unsigned long long)image.page_runs[j].addr,
				(unsigned long long)image.page_runs[j].pages,
				(unsigned long long)image.page_runs[j].image_offset);
	}
	if (st == B1_RESTORE_OK)
		st = b1_stage_image(&image, images, &staging);
	if (st == B1_RESTORE_OK && !dry_run) {
		size_t j;
		for (j = 0; j < staging.vma_count; j++)
			fprintf(stderr, "B1_STAGE[%zu]: src=0x%llx dst=0x%llx len=0x%llx\n", j,
				(unsigned long long)staging.restore_vmas[j].staging_start,
				(unsigned long long)staging.restore_vmas[j].target_start,
				(unsigned long long)staging.restore_vmas[j].length);
		fprintf(stderr, "B1_CORE_STATE: sp=0x%llx pc=0x%llx pstate=0x%llx tls=0x%llx\n",
			(unsigned long long)image.sp,
			(unsigned long long)image.pc,
			(unsigned long long)image.pstate,
			(unsigned long long)image.tls);
		fprintf(stderr, "B1_CORE_REGS: x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x8=0x%llx x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx x29=0x%llx x30=0x%llx\n",
			(unsigned long long)image.regs[0], (unsigned long long)image.regs[1],
			(unsigned long long)image.regs[2], (unsigned long long)image.regs[3],
			(unsigned long long)image.regs[8],
			(unsigned long long)image.regs[19], (unsigned long long)image.regs[20],
			(unsigned long long)image.regs[21], (unsigned long long)image.regs[22],
			(unsigned long long)image.regs[29], (unsigned long long)image.regs[30]);
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
	if (st == B1_RESTORE_OK && !dry_run) {
#if defined(__aarch64__) && defined(__linux__)
		st = map_bootstrap(&image, &bootstrap_code, &bootstrap_stack);
		if (st == B1_RESTORE_OK) {
			sigframe_staging_sp = B1_SIGFRAME_BOOTSTRAP_SP;
			sigframe_final_sp = B1_SIGFRAME_BOOTSTRAP_SP;
			memcpy((void *)(uintptr_t)sigframe_final_sp,
			       &sigframe, sizeof(sigframe));
		}
#else
		b1_restore_set_diag(&image, B1_RESTORE_UNSUPPORTED,
				"live bootstrap handoff requires an aarch64 guest");
		st = B1_RESTORE_UNSUPPORTED;
#endif
	}
	if (st == B1_RESTORE_OK && !dry_run) {
		size_t j;
		for (j = 0; j < image.vma_count; j++) {
			uint64_t start = image.vmas[j].start;
			uint64_t end = start + image.vmas[j].length;
			if (image.pc >= start && image.pc < end) {
				fprintf(stderr, "B1_PC_OFFSET: pc=0x%llx start=0x%llx offset=0x%llx src=0x%llx\n",
					(unsigned long long)image.pc,
					(unsigned long long)start,
					(unsigned long long)(image.pc - start),
					(unsigned long long)staging.restore_vmas[j].staging_start);
				const unsigned char *p = (const unsigned char *)(uintptr_t)
					(staging.restore_vmas[j].staging_start + image.pc - start);
				fprintf(stderr, "B1_PC_BYTES:");
				for (size_t k = 0; k < 16; k++)
					fprintf(stderr, " %02x", p[k]);
				fprintf(stderr, "\\n");
				memcpy(&target_pc_word, p, sizeof(target_pc_word));
				break;
			}
		}
	}
	if (st == B1_RESTORE_OK && !dry_run)
		st = build_validate_plan(&image, &staging, sigframe_staging_sp,
					 sigframe_final_sp, &validate_plan);
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", image.diagnostic);
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 1;
	}

	if (dry_run) {
		size_t j;

		for (j = 0; j < image.vma_count; j++)
			fprintf(stderr, "B1_VMA[%zu]: start=0x%llx len=0x%llx kind=%d\n",
				j, (unsigned long long)image.vmas[j].start,
				(unsigned long long)image.vmas[j].length,
				image.vmas[j].kind);
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
	#if !defined(__aarch64__) || !defined(__linux__)
	b1_restore_set_diag(&image, B1_RESTORE_UNSUPPORTED,
				"live bootstrap handoff requires an aarch64 guest");
	fprintf(stderr, "%s\n", image.diagnostic);
	b1_cleanup_run(&cleanup);
	b1_restore_image_free(&image);
	return 1;
	#else
	{
		struct b1_bootstrap_args *bootstrap_args;
		struct criu_restore_commit_v1 *commit;

		bootstrap_args = (struct b1_bootstrap_args *)
			((uintptr_t)bootstrap_stack +
			 (B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START) - 256U);
		commit = (struct criu_restore_commit_v1 *)
			((uintptr_t)bootstrap_stack +
			 (B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START) - 128U);
		memset(bootstrap_args, 0, sizeof(*bootstrap_args));
		memset(commit, 0, sizeof(*commit));
		commit->version = CRIU_RESTORE_ABI_VERSION;
		commit->size = sizeof(*commit);
		bootstrap_args->restore_fd = (uint64_t)restore_fd;
		bootstrap_args->commit_user_ptr = (uint64_t)(uintptr_t)commit;
		bootstrap_args->sigframe_final_sp = sigframe_final_sp;
		bootstrap_args->tls = image.tls;
		bootstrap_args->bootstrap_sp = B1_BOOTSTRAP_STACK_END - 16U;
		bootstrap_args->target_sp = image.sp;
		bootstrap_args->target_pc = image.pc;
		bootstrap_args->target_pc_word = target_pc_word;
		bootstrap_args->target_stop = image.regs[2];
		st = b1_create_exact_pid_carrier(
			&cleanup.carriers, (pid_t)image.target_pid,
			image.tls,
			(b1_carrier_entry_fn)(uintptr_t)bootstrap_code,
			bootstrap_args, &image);
	}
	#endif
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", image.diagnostic);
		b1_cleanup_run(&cleanup);
		b1_restore_image_free(&image);
		return 1;
	}

	/* Ownership of the carrier is transferred to the restored task. */
	for (i = 0; i < 100; i++) {
		int status = 0;
		pid_t got;
		siginfo_t info;

		memset(&info, 0, sizeof(info));
		(void)waitid(P_PID, (id_t)image.target_pid, &info,
			     WEXITED | WNOHANG | WNOWAIT);
		got = waitpid((pid_t)image.target_pid, &status, WNOHANG);
		if (got > 0) {
			if (info.si_pid) {
				fprintf(stderr, "B1_CARRIER_SIGINFO: code=%d status=%d addr=%p\n",
					info.si_code, info.si_status, info.si_addr);
			}
			fprintf(stderr, "B1_CARRIER_EXIT: status=%d signal=%d exit=%d\n",
				status, WIFSIGNALED(status) ? WTERMSIG(status) : 0,
				WIFEXITED(status) ? WEXITSTATUS(status) : -1);
			cleanup.carriers.pids[0] = -1;
			b1_cleanup_run(&cleanup);
			b1_restore_image_free(&image);
			return 1;
		}
		usleep(10000);
	}
	b1_carrier_manager_free(&cleanup.carriers);
	if (cleanup.staging)
		b1_staging_plan_free(cleanup.staging);
	if (cleanup.restore_fd >= 0) {
		close(cleanup.restore_fd);
		cleanup.restore_fd = -1;
	}
	b1_restore_image_free(&image);
	return 0;
}
