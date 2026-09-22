#define _GNU_SOURCE

#include "task_restore.h"
#include "rst_cleanup.h"
#include "rst_fork.h"
#include "rst_pstree.h"
#include "rst_shared.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s --images DIR [--dry-run]\n", argv0);
}

static void print_image_details(const struct b2_task_restore *task)
{
	const struct b1_restore_image *image = &task->image;
	size_t i;

	for (i = 0; i < image->vma_count; i++)
		fprintf(stderr,
			"B1_VMA_DETAIL[%zu]: start=0x%llx len=0x%llx "
			"pgoff=0x%llx prot=%u fd=%d kind=%d\n",
			i, (unsigned long long)image->vmas[i].start,
			(unsigned long long)image->vmas[i].length,
			(unsigned long long)image->vmas[i].pgoff,
			image->vmas[i].prot, image->vmas[i].backing_fd,
			image->vmas[i].kind);
	for (i = 0; i < image->page_run_count; i++)
		fprintf(stderr,
			"B1_RUN[%zu]: addr=0x%llx pages=%llu off=%llu\n",
			i, (unsigned long long)image->page_runs[i].addr,
			(unsigned long long)image->page_runs[i].pages,
			(unsigned long long)image->page_runs[i].image_offset);
	for (i = 0; i < task->staging.vma_count; i++)
		fprintf(stderr,
			"B1_STAGE[%zu]: src=0x%llx dst=0x%llx len=0x%llx\n",
			i, (unsigned long long)task->staging.restore_vmas[i].staging_start,
			(unsigned long long)task->staging.restore_vmas[i].target_start,
			(unsigned long long)task->staging.restore_vmas[i].length);
	fprintf(stderr,
		"B1_CORE_STATE: sp=0x%llx pc=0x%llx pstate=0x%llx tls=0x%llx\n",
		(unsigned long long)image->sp, (unsigned long long)image->pc,
		(unsigned long long)image->pstate, (unsigned long long)image->tls);
	fprintf(stderr,
		"B1_CORE_REGS: x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
		"x8=0x%llx x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx "
		"x29=0x%llx x30=0x%llx\n",
		(unsigned long long)image->regs[0],
		(unsigned long long)image->regs[1],
		(unsigned long long)image->regs[2],
		(unsigned long long)image->regs[3],
		(unsigned long long)image->regs[8],
		(unsigned long long)image->regs[19],
		(unsigned long long)image->regs[20],
		(unsigned long long)image->regs[21],
		(unsigned long long)image->regs[22],
		(unsigned long long)image->regs[29],
		(unsigned long long)image->regs[30]);
}

static int report_carrier_exit(struct b2_task_restore *task)
{
	int status = 0;
	siginfo_t info;
	pid_t got;

	memset(&info, 0, sizeof(info));
	(void)waitid(P_PID, (id_t)task->target_pid, &info,
		     WEXITED | WNOHANG | WNOWAIT);
	got = waitpid(task->target_pid, &status, WNOHANG);
	if (got <= 0)
		return 0;
	if (info.si_pid)
		fprintf(stderr,
			"B1_CARRIER_SIGINFO: code=%d status=%d addr=%p\n",
			info.si_code, info.si_status, info.si_addr);
	fprintf(stderr,
		"B1_CARRIER_EXIT: status=%d signal=%d exit=%d\n",
		status, WIFSIGNALED(status) ? WTERMSIG(status) : 0,
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	return 1;
}

static int run_tree_restore(const char *images, int dry_run,
			    int restore_sibling)
{
	struct rst_pstree tree;
	struct rst_shared *shared = NULL;
	struct b2_task_restore *tasks = NULL;
	struct rst_tree_context *contexts = NULL;
	struct rst_tree_runtime runtime;
	struct b1_restore_image diag;
	enum b1_restore_status st;
	size_t i;
	int rc = 1;

	memset(&tree, 0, sizeof(tree));
	b1_restore_image_init(&diag);
	st = rst_read_pstree(images, &tree);
	if (st == B1_RESTORE_OK)
		st = rst_validate_pstree(&tree, &diag);
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", diag.diagnostic);
		goto out;
	}
	tasks = calloc(tree.count, sizeof(*tasks));
	contexts = calloc(tree.count, sizeof(*contexts));
	if (!tasks || !contexts) {
		fprintf(stderr, "IO: allocating B2 task state\n");
		goto out;
	}
	for (i = 0; i < tree.count; i++) {
		b1_task_restore_init(&tasks[i]);
		st = b1_task_restore_prepare_tree(images, tree.items[i].pid,
						  &tasks[i]);
		if (st != B1_RESTORE_OK) {
			fprintf(stderr, "pid %d: %s\n", tree.items[i].pid,
				tasks[i].image.diagnostic);
			goto out;
		}
		contexts[i].runtime = &runtime;
		contexts[i].index = i;
	}
	if (dry_run) {
		printf("B2_RESTORE: DRY_RUN_OK tasks=%zu root=%d\n",
		       tree.count, tree.root->pid);
		rc = 0;
		goto out;
	}
	if (rst_shared_init(tree.count, &shared) != 0) {
		fprintf(stderr, "IO: allocating B2 shared scratch\n");
		goto out;
	}
	for (i = 0; i < tree.count; i++) {
		st = b1_task_restore_validate(&tasks[i]);
		if (st != B1_RESTORE_OK) {
			fprintf(stderr, "pid %d: %s\n", tree.items[i].pid,
				tasks[i].image.diagnostic);
			goto fail_tree;
		}
	}
	memset(&runtime, 0, sizeof(runtime));
	runtime.tree = &tree;
	runtime.shared = shared;
	runtime.tasks = tasks;
	runtime.contexts = contexts;
	runtime.task_count = tree.count;
	runtime.timeout_ms = 10000;
	{
		int start_rc = rst_tree_restore_start(
			&runtime,
			restore_sibling ? B1_CARRIER_F_CLONE_PARENT : 0);
		if (start_rc != 0) {
			fprintf(stderr, "IO: creating B2 process tree rc=%d: %s\n",
				start_rc, tasks[tree.root->index].image.diagnostic);
			goto fail_tree;
		}
	}
	if (rst_shared_wait_count(shared, (unsigned)tree.count,
				  runtime.timeout_ms) != 0) {
		fprintf(stderr, "IO: B2 ready barrier timed out\n");
		goto fail_tree;
	}
	if (rst_wait_tree_root(tree.root->pid, runtime.timeout_ms) != 0) {
		fprintf(stderr, "IO: B2 root did not remain alive\n");
		goto fail_tree;
	}
	b1_task_restore_detach_carriers(&tasks[tree.root->index]);
	printf("B2_RESTORE: OK tasks=%zu root=%d\n",
	       tree.count, tree.root->pid);
	rc = 0;
	goto out;

fail_tree:
	(void)rst_cleanup_all(shared, &tree, runtime.timeout_ms);
out:
	if (tasks) {
		for (i = 0; i < tree.count; i++)
			b1_task_restore_destroy(&tasks[i]);
	}
	if (shared)
		rst_shared_destroy(shared);
	free(contexts);
	free(tasks);
	rst_free_pstree(&tree);
	b1_restore_image_free(&diag);
	return rc;
}

int main(int argc, char **argv)
{
	const char *images = NULL;
	const char *pstree = NULL;
	int dry_run = 0;
	int restore_sibling = 0;
	struct b2_task_restore task;
	enum b1_restore_status st;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--images") == 0 && i + 1 < argc)
			images = argv[++i];
		else if (strcmp(argv[i], "--pstree") == 0 && i + 1 < argc)
			pstree = argv[++i];
		else if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "--restore-sibling") == 0)
			restore_sibling = 1;
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (pstree) {
		if (images || run_tree_restore(pstree, dry_run, restore_sibling))
			return images ? 2 : 1;
		return 0;
	}
	if (!images || restore_sibling) {
		usage(argv[0]);
		return 2;
	}

	b1_task_restore_init(&task);
	/*
	 * The one-node B1 CLI uses the PID declared by core.img.  The reusable
	 * API then performs the authoritative read and validates that identity.
	 */
	{
		struct b1_restore_image probe;

		b1_restore_image_init(&probe);
		st = b1_read_images(images, &probe);
		if (st != B1_RESTORE_OK) {
			fprintf(stderr, "%s\n", probe.diagnostic);
			b1_restore_image_free(&probe);
			b1_task_restore_destroy(&task);
			return 1;
		}
		task.target_pid = (pid_t)probe.target_pid;
		b1_restore_image_free(&probe);
	}
	st = b1_task_restore_prepare(images, task.target_pid, &task);
	if (st == B1_RESTORE_OK && !dry_run)
		print_image_details(&task);
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", task.image.diagnostic);
		b1_task_restore_destroy(&task);
		return 1;
	}

	if (dry_run) {
		for (i = 0; i < (int)task.image.vma_count; i++)
			fprintf(stderr,
				"B1_VMA[%d]: start=0x%llx len=0x%llx kind=%d\n",
				i, (unsigned long long)task.image.vmas[i].start,
				(unsigned long long)task.image.vmas[i].length,
				task.image.vmas[i].kind);
		printf("B1_RESTORE: DRY_RUN_OK pid=%u vmas=%zu\n",
		       task.image.target_pid, task.staging.vma_count);
		b1_task_restore_destroy(&task);
		return 0;
	}

	st = b1_task_restore_validate(&task);
	if (st == B1_RESTORE_OK)
		st = b1_task_restore_commit(&task);
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", task.image.diagnostic);
		b1_task_restore_destroy(&task);
		return 1;
	}

	for (i = 0; i < 100; i++) {
		if (report_carrier_exit(&task)) {
			b1_task_restore_detach_carriers(&task);
			b1_task_restore_destroy(&task);
			return 1;
		}
		usleep(10000);
	}
	b1_task_restore_detach_carriers(&task);
	b1_task_restore_destroy(&task);
	return 0;
}
