#define _GNU_SOURCE

#include "task_restore.h"

#include "carrier.h"
#include "validator.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define B1_BOOTSTRAP_CODE_START 0x7000000000ULL
#define B1_BOOTSTRAP_CODE_END   0x7000001000ULL
#define B1_BOOTSTRAP_STACK_START 0x7000001000ULL
#define B1_BOOTSTRAP_STACK_END   0x7000003000ULL
#define B1_SIGFRAME_BOOTSTRAP_SP (B1_BOOTSTRAP_STACK_START + 0x100ULL)

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#if defined(__aarch64__) && defined(__linux__)
extern const unsigned char b1_restore_bootstrap_start[];
extern const unsigned char b1_restore_bootstrap_end[];
#endif

static enum b1_restore_status build_validate_plan(
	const struct b1_restore_image *image,
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
static enum b1_restore_status map_bootstrap(struct b2_task_restore *task)
{
	size_t code_len = (size_t)(b1_restore_bootstrap_end -
				   b1_restore_bootstrap_start);
	void *mapped_code;
	void *mapped_stack;

	if (!code_len || code_len > B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START) {
		b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
				    "bootstrap code exceeds reserved mapping");
		return B1_RESTORE_FORMAT;
	}
	mapped_code = mmap((void *)(uintptr_t)B1_BOOTSTRAP_CODE_START,
			   B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	if (mapped_code == MAP_FAILED) {
		if (errno == EEXIST)
			mapped_code = (void *)(uintptr_t)B1_BOOTSTRAP_CODE_START;
		else {
			b1_restore_set_diag(&task->image, B1_RESTORE_IO,
					    "mapping fixed bootstrap code");
			return B1_RESTORE_IO;
		}
	} else {
		task->bootstrap_code_owner = 1;
	}
	mapped_stack = mmap((void *)(uintptr_t)B1_BOOTSTRAP_STACK_START,
			    B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			    -1, 0);
	if (mapped_stack == MAP_FAILED) {
		if (errno == EEXIST)
			mapped_stack = (void *)(uintptr_t)B1_BOOTSTRAP_STACK_START;
		else {
			if (task->bootstrap_code_owner) {
				munmap(mapped_code,
				       B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START);
				task->bootstrap_code_owner = 0;
			}
			b1_restore_set_diag(&task->image, B1_RESTORE_IO,
					    "mapping fixed bootstrap stack");
			return B1_RESTORE_IO;
		}
	} else {
		task->bootstrap_stack_owner = 1;
	}
	if (task->bootstrap_code_owner) {
		memcpy(mapped_code, b1_restore_bootstrap_start, code_len);
		if (mprotect(mapped_code,
			    B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START,
			    PROT_READ | PROT_EXEC) < 0) {
			if (task->bootstrap_stack_owner) {
				munmap(mapped_stack,
				       B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START);
				task->bootstrap_stack_owner = 0;
			}
			munmap(mapped_code,
			       B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START);
			task->bootstrap_code_owner = 0;
			b1_restore_set_diag(&task->image, B1_RESTORE_IO,
					    "protecting fixed bootstrap code");
			return B1_RESTORE_IO;
		}
	}
	task->bootstrap_code = mapped_code;
	task->bootstrap_stack = mapped_stack;
	return B1_RESTORE_OK;
}
#endif

static enum b1_restore_status prepare_sigframe(struct b2_task_restore *task)
{
	struct b1_aarch64_thread_state thread;

	memset(&thread, 0, sizeof(thread));
	memcpy(thread.regs, task->image.regs, sizeof(thread.regs));
	thread.sp = task->image.sp;
	thread.pc = task->image.pc;
	thread.pstate = task->image.pstate;
	thread.sigmask = task->image.sigmask;
	thread.tls = task->image.tls;
	memcpy(thread.vregs, task->image.vregs, sizeof(thread.vregs));
	thread.fpsr = task->image.fpsr;
	thread.fpcr = task->image.fpcr;
	if (b1_sigframe_build(&thread, &task->sigframe)) {
		b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
				    "building sigframe");
		return B1_RESTORE_FORMAT;
	}
	return B1_RESTORE_OK;
}

#if defined(__aarch64__) && defined(__linux__)
static enum b1_restore_status capture_target_pc_word(
	struct b2_task_restore *task)
{
	size_t i;

	for (i = 0; i < task->image.vma_count; i++) {
		uint64_t start = task->image.vmas[i].start;
		uint64_t end = start + task->image.vmas[i].length;

		if (task->image.pc < start || task->image.pc >= end)
			continue;
		if (end - task->image.pc < sizeof(task->target_pc_word)) {
			b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
					    "target pc is too close to VMA end");
			return B1_RESTORE_FORMAT;
		}
		{
			const unsigned char *p = (const unsigned char *)(uintptr_t)
				(task->staging.restore_vmas[i].staging_start +
				 task->image.pc - start);

			memcpy(&task->target_pc_word, p, sizeof(task->target_pc_word));
		}
		return B1_RESTORE_OK;
	}
	b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
			    "target pc is outside restored VMAs");
	return B1_RESTORE_FORMAT;
}
#endif

void b1_task_restore_init(struct b2_task_restore *task)
{
	memset(task, 0, sizeof(*task));
	b1_restore_image_init(&task->image);
	b1_staging_plan_init(&task->staging);
	b1_cleanup_init(&task->cleanup);
	b1_cleanup_set_staging(&task->cleanup, &task->staging);
	task->restore_fd = -1;
	task->target_pid = -1;
}

static enum b1_restore_status b1_task_restore_prepare_mode(
	const char *images, pid_t target_pid, struct b2_task_restore *task,
	int tree_mode)
{
	enum b1_restore_status st;

	if (!images || !task || target_pid <= 0) {
		if (task)
			b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
					    "bad task restore request");
		return B1_RESTORE_FORMAT;
	}
	task->target_pid = target_pid;
	st = b1_read_images_for_pid(images, target_pid, &task->image);
	if (st != B1_RESTORE_OK)
		return st;
	if (task->image.target_pid != (uint32_t)target_pid) {
		b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
				    "image target pid does not match task");
		return B1_RESTORE_FORMAT;
	}
	st = tree_mode ? b1_validate_task_supported(&task->image) :
		b1_validate_supported(&task->image);
	if (st != B1_RESTORE_OK)
		return st;
	st = b1_stage_image(&task->image, images, &task->staging);
	if (st != B1_RESTORE_OK)
		return st;
	st = prepare_sigframe(task);
	if (st != B1_RESTORE_OK)
		return st;
	task->prepared = 1;
	return B1_RESTORE_OK;
}

enum b1_restore_status b1_task_restore_prepare(
	const char *images, pid_t target_pid, struct b2_task_restore *task)
{
	return b1_task_restore_prepare_mode(images, target_pid, task, 0);
}

enum b1_restore_status b1_task_restore_prepare_tree(
	const char *images, pid_t target_pid, struct b2_task_restore *task)
{
	return b1_task_restore_prepare_mode(images, target_pid, task, 1);
}

enum b1_restore_status b1_task_restore_validate(
	struct b2_task_restore *task)
{
	if (!task || !task->prepared) {
		if (task)
			b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
					    "task restore was not prepared");
		return B1_RESTORE_FORMAT;
	}
#if defined(__aarch64__) && defined(__linux__)
	if (capture_target_pc_word(task) != B1_RESTORE_OK)
		return B1_RESTORE_FORMAT;
	task->sigframe_staging_sp = B1_SIGFRAME_BOOTSTRAP_SP;
	task->sigframe_final_sp = B1_SIGFRAME_BOOTSTRAP_SP;
	{
		enum b1_restore_status map_st;

		map_st = map_bootstrap(task);
		if (map_st != B1_RESTORE_OK)
			return map_st;
	}
	memcpy((void *)(uintptr_t)task->sigframe_final_sp,
	       &task->sigframe, sizeof(task->sigframe));
#else
	b1_restore_set_diag(&task->image, B1_RESTORE_UNSUPPORTED,
			    "live bootstrap handoff requires an aarch64 guest");
	return B1_RESTORE_UNSUPPORTED;
#endif
	if (build_validate_plan(&task->image, &task->staging,
				task->sigframe_staging_sp,
				task->sigframe_final_sp,
				&task->validate_plan) != B1_RESTORE_OK)
		return B1_RESTORE_FORMAT;
	task->restore_fd = open("/dev/criu_restore", O_RDWR | O_CLOEXEC);
	if (task->restore_fd < 0) {
		b1_restore_set_diag(&task->image, B1_RESTORE_IO,
				    "open /dev/criu_restore");
		return B1_RESTORE_IO;
	}
	b1_cleanup_set_restore_fd(&task->cleanup, task->restore_fd);
	if (ioctl(task->restore_fd, CRIU_RESTORE_VALIDATE_V1,
		  &task->validate_plan) < 0) {
		b1_restore_set_diag(&task->image, B1_RESTORE_IO,
				    "CRIU_RESTORE_VALIDATE_V1 failed");
		return B1_RESTORE_IO;
	}
	task->validated = 1;
	return B1_RESTORE_OK;
}

enum b1_restore_status b1_task_restore_commit(
	struct b2_task_restore *task)
{
#if defined(__aarch64__) && defined(__linux__)
	struct criu_restore_commit_v1 *commit;

	if (!task || !task->validated || !task->bootstrap_stack) {
		if (task)
			b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
					    "task restore was not validated");
		return B1_RESTORE_FORMAT;
	}
	task->bootstrap_args_ptr = (struct b1_bootstrap_args *)
		((uintptr_t)task->bootstrap_stack +
		 (B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START) - 256U);
	commit = (struct criu_restore_commit_v1 *)
		((uintptr_t)task->bootstrap_stack +
		 (B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START) - 128U);
	memset(task->bootstrap_args_ptr, 0, sizeof(task->bootstrap));
	memset(commit, 0, sizeof(*commit));
	commit->version = CRIU_RESTORE_ABI_VERSION;
	commit->size = sizeof(*commit);
	task->bootstrap.restore_fd = (uint64_t)task->restore_fd;
	task->bootstrap.commit_user_ptr = (uint64_t)(uintptr_t)commit;
	task->bootstrap.sigframe_final_sp = task->sigframe_final_sp;
	task->bootstrap.tls = task->image.tls;
	task->bootstrap.bootstrap_sp = B1_BOOTSTRAP_STACK_END - 16U;
	task->bootstrap.target_sp = task->image.sp;
	task->bootstrap.target_pc = task->image.pc;
	task->bootstrap.target_pc_word = task->target_pc_word;
	task->bootstrap.target_stop = task->image.regs[2];
	memcpy(task->bootstrap_args_ptr, &task->bootstrap,
	       sizeof(task->bootstrap));
	return b1_task_restore_spawn(
		task, 0, (b1_carrier_entry_fn)(uintptr_t)task->bootstrap_code,
		task->bootstrap_args_ptr);
#else
	if (task)
		b1_restore_set_diag(&task->image, B1_RESTORE_UNSUPPORTED,
				    "live bootstrap handoff requires an aarch64 guest");
	return B1_RESTORE_UNSUPPORTED;
#endif
}

enum b1_restore_status b1_task_restore_spawn(
	struct b2_task_restore *task, unsigned carrier_flags,
	b1_carrier_entry_fn entry, void *arg)
{
	if (!task || !task->validated || !entry) {
		if (task)
			b1_restore_set_diag(&task->image, B1_RESTORE_FORMAT,
					    "task restore was not validated");
		return B1_RESTORE_FORMAT;
	}
	return b1_create_exact_pid_carrier_flags(
		&task->cleanup.carriers, task->target_pid, task->image.tls,
		carrier_flags, entry, arg, &task->image);
}

int b1_task_restore_bootstrap_now(struct b2_task_restore *task)
{
#if defined(__aarch64__) && defined(__linux__)
	struct b1_bootstrap_args bootstrap;
	struct criu_restore_commit_v1 commit;
	void (*entry)(const struct b1_bootstrap_args *);

	if (!task || !task->validated || !task->bootstrap_code ||
	    !task->bootstrap_stack)
		return -EINVAL;
	memset(&bootstrap, 0, sizeof(bootstrap));
	memset(&commit, 0, sizeof(commit));
	commit.version = CRIU_RESTORE_ABI_VERSION;
	commit.size = sizeof(commit);
	memcpy((void *)(uintptr_t)task->sigframe_final_sp,
	       &task->sigframe, sizeof(task->sigframe));
	bootstrap.restore_fd = (uint64_t)task->restore_fd;
	bootstrap.commit_user_ptr = (uint64_t)(uintptr_t)&commit;
	bootstrap.sigframe_final_sp = task->sigframe_final_sp;
	bootstrap.tls = task->image.tls;
	bootstrap.bootstrap_sp = B1_BOOTSTRAP_STACK_END - 16U;
	bootstrap.target_sp = task->image.sp;
	bootstrap.target_pc = task->image.pc;
	bootstrap.target_pc_word = task->target_pc_word;
	bootstrap.target_stop = task->image.regs[2];
	entry = (void (*)(const struct b1_bootstrap_args *))
		(uintptr_t)task->bootstrap_code;
	entry(&bootstrap);
	return -EIO;
#else
	(void)task;
	return -EOPNOTSUPP;
#endif
}

void b1_task_restore_detach_carriers(struct b2_task_restore *task)
{
	size_t i;

	if (!task)
		return;
	for (i = 0; i < task->cleanup.carriers.count; i++)
		task->cleanup.carriers.pids[i] = -1;
	b1_carrier_manager_free(&task->cleanup.carriers);
}

void b1_task_restore_destroy(struct b2_task_restore *task)
{
	if (!task)
		return;
	b1_cleanup_run(&task->cleanup);
#if defined(__aarch64__) && defined(__linux__)
	if (task->bootstrap_stack && task->bootstrap_stack_owner) {
		munmap(task->bootstrap_stack,
		       B1_BOOTSTRAP_STACK_END - B1_BOOTSTRAP_STACK_START);
	}
	if (task->bootstrap_code && task->bootstrap_code_owner) {
		munmap(task->bootstrap_code,
		       B1_BOOTSTRAP_CODE_END - B1_BOOTSTRAP_CODE_START);
	}
	task->bootstrap_stack = NULL;
	task->bootstrap_code = NULL;
	task->bootstrap_stack_owner = 0;
	task->bootstrap_code_owner = 0;
#endif
	b1_restore_image_free(&task->image);
	task->prepared = 0;
	task->validated = 0;
	task->restore_fd = -1;
	task->target_pid = -1;
}
