#ifndef B2_TASK_RESTORE_H
#define B2_TASK_RESTORE_H

#include <sys/types.h>

#include "bootstrap.h"
#include "cleanup.h"
#include "criu_restore_abi.h"
#include "image_reader.h"
#include "sigframe.h"
#include "staging.h"

struct b2_task_restore {
	struct b1_restore_image image;
	struct b1_staging_plan staging;
	struct b1_cleanup cleanup;
	struct b1_aarch64_rt_sigframe sigframe;
	struct b1_bootstrap_args bootstrap;
	struct criu_restore_plan_v1 validate_plan;
	uint64_t sigframe_staging_sp;
	uint64_t sigframe_final_sp;
	uint64_t target_pc_word;
	void *bootstrap_code;
	void *bootstrap_stack;
	unsigned bootstrap_code_owner;
	unsigned bootstrap_stack_owner;
	void *bootstrap_args_ptr;
	int restore_fd;
	pid_t target_pid;
	unsigned prepared;
	unsigned validated;
};

void b1_task_restore_init(struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_prepare(
	const char *images, pid_t target_pid, struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_prepare_tree(
	const char *images, pid_t target_pid, struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_validate(
	struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_commit(
	struct b2_task_restore *task);
enum b1_restore_status b1_task_restore_spawn(
	struct b2_task_restore *task, unsigned carrier_flags,
	b1_carrier_entry_fn entry, void *arg);
int b1_task_restore_bootstrap_now(struct b2_task_restore *task);
void b1_task_restore_detach_carriers(struct b2_task_restore *task);
void b1_task_restore_destroy(struct b2_task_restore *task);

#endif
