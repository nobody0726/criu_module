/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../../include/criu_snapshot.h"
#include "criu_kernel.h"
#include "dump.h"
#include "dump_files.h"
#include "dump_mm.h"
#include "dump_pstree.h"
#include "dump_shared.h"
#include "dump_signals.h"
#include "dump_task.h"
#include "dump_threads.h"
#include "dump_timers.h"
#include "snapshot_writer.h"

static int dump_revalidate(struct task_struct *task, u64 generation,
				struct mm_struct *mm, unsigned long vma_count)
{
	struct task_struct *again;
	struct mm_struct *again_mm;
	struct criu_mm_info info;
	u64 again_generation;
	int ret = 0;

	again = criu_target_get(&again_generation);
	if (!again)
		return -ESRCH;
	if (again != task || task_pid_vnr(again) != task_pid_vnr(task) ||
	    again_generation != generation)
		ret = -EAGAIN;
	again_mm = get_task_mm(again);
	if (!ret && (!again_mm || again_mm != mm))
		ret = -EAGAIN;
	if (!ret && criu_collect_mm_info(again, &info))
		ret = -EAGAIN;
	if (!ret && info.vma_count != vma_count)
		ret = -EAGAIN;
	if (again_mm)
		mmput(again_mm);
	put_task_struct(again);
	return ret;
}

static int dump_tree_validate_process_isolation(struct criu_freeze_ctx *ctx,
						unsigned int process_count)
{
	unsigned int i, j;

	for (i = 0; i < process_count; i++) {
		struct criu_freeze_process_view left;
		struct mm_struct *left_mm;
		struct files_struct *left_files;
		int ret;

		ret = criu_freeze_process_get(ctx, i, &left);
		if (ret)
			return ret;
		left_mm = get_task_mm(left.leader);
		if (!left_mm)
			return -ESRCH;
		task_lock(left.leader);
		left_files = left.leader->files;
		task_unlock(left.leader);
		if (!left_files) {
			mmput(left_mm);
			return -ESRCH;
		}
		for (j = i + 1; j < process_count; j++) {
			struct criu_freeze_process_view right;
			struct mm_struct *right_mm;
			struct files_struct *right_files;

			ret = criu_freeze_process_get(ctx, j, &right);
			if (ret) {
				mmput(left_mm);
				return ret;
			}
			right_mm = get_task_mm(right.leader);
			if (!right_mm) {
				mmput(left_mm);
				return -ESRCH;
			}
			task_lock(right.leader);
			right_files = right.leader->files;
			task_unlock(right.leader);
			if (!right_files || right_mm == left_mm ||
			    right_files == left_files) {
				mmput(right_mm);
				mmput(left_mm);
				return -EOPNOTSUPP;
			}
			mmput(right_mm);
		}
		mmput(left_mm);
	}
	return 0;
}

int criu_dump_process(pid_t vpid, const char *path)
{
	struct task_struct *task = NULL;
	struct mm_struct *mm = NULL;
	struct criu_mm_info mm_info;
	struct criu_snapshot_header header;
	struct criu_snapshot_writer writer;
	struct criu_freeze_ctx *freeze_ctx = NULL;
	struct criu_freeze_task_view frozen_view;
	struct criu_signal_capture *signal_capture = NULL;
	struct criu_timer_capture *timer_capture = NULL;
	unsigned int frozen_count;
	u64 frozen_generation;
	u64 generation;
	int ret, thaw_ret;
	bool opened = false;
	bool captured = false;

	if (vpid <= 0 || !path || !*path)
		return -EINVAL;

	/* Bind the operation to the selected target and its A2 generation. */
	task = criu_target_get(&generation);
	if (!task || task_pid_vnr(task) != vpid) {
		pr_info("criu_dump: initial target failed requested=%d task=%p task_pid=%d generation=%llu\n",
			vpid, task, task ? task_pid_vnr(task) : -1, generation);
		if (task)
			put_task_struct(task);
		return -ESRCH;
	}
	mm = get_task_mm(task);
	if (!mm) {
		pr_info("criu_dump: get_task_mm failed pid=%d\n", vpid);
		put_task_struct(task);
		return -ESRCH;
	}
	ret = criu_collect_mm_info(task, &mm_info);
	if (ret) {
		pr_info("criu_dump: collect_mm_info failed pid=%d ret=%d\n", vpid, ret);
		goto out;
	}

	ret = criu_freeze(vpid, false, &freeze_ctx);
	pr_info("criu_dump: freeze pid=%d ret=%d ctx=%p\n", vpid, ret, freeze_ctx);
	if (ret)
		goto out;
	ret = criu_freeze_task_count(freeze_ctx, &frozen_count);
	if (!ret && (!frozen_count ||
		     criu_freeze_task_get(freeze_ctx, 0, &frozen_view) ||
		     criu_freeze_generation(freeze_ctx, &frozen_generation) ||
		     frozen_generation != generation ||
		     frozen_view.tid != task_pid_vnr(task)))
		ret = -EAGAIN;
	if (ret)
		goto thaw;
	signal_capture = kzalloc(sizeof(*signal_capture), GFP_KERNEL);
	timer_capture = kzalloc(sizeof(*timer_capture), GFP_KERNEL);
	if (!signal_capture || !timer_capture) {
		ret = -ENOMEM;
		goto thaw;
	}
	ret = criu_collect_signals(freeze_ctx, signal_capture);
	if (ret)
		goto thaw;
	ret = criu_collect_timers(freeze_ctx, timer_capture);
	if (ret) {
		criu_release_signals(signal_capture);
		goto thaw;
	}
	captured = true;

	memset(&header, 0, sizeof(header));
	header.magic = CRIU_SNAPSHOT_MAGIC;
	header.version = CRIU_SNAPSHOT_VERSION;
	header.header_size = CRIU_SNAPSHOT_HEADER_SIZE;
	header.flags = CRIU_SNAPSHOT_F_SIGNAL_TIMERS;
#ifdef CONFIG_ARM64
	header.arch = 183; /* EM_AARCH64 */
#elif defined(CONFIG_X86_64)
	header.arch = 62; /* EM_X86_64 */
#else
	header.arch = 0;
#endif
	header.page_size = PAGE_SIZE;
	header.pid = task_pid_vnr(task);
	header.tgid = task_tgid_vnr(task);
	header.freeze_generation = generation;

	ret = criu_snapshot_writer_open(&writer, path, &header);
	if (ret)
		goto thaw;
	opened = true;
	ret = criu_dump_task(task, &writer);
	pr_info("criu_dump: dump_task pid=%d ret=%d\n", vpid, ret);
	if (!ret)
		ret = criu_dump_threads(task, &writer);
	if (!ret)
		ret = criu_dump_mm(task, &writer);
	pr_info("criu_dump: dump_mm pid=%d ret=%d\n", vpid, ret);
	if (!ret)
		ret = criu_dump_files(task, &writer);
	pr_info("criu_dump: dump_files pid=%d ret=%d\n", vpid, ret);
	if (!ret)
		ret = criu_emit_signals(signal_capture, &writer);
	if (!ret)
		ret = criu_emit_timers(timer_capture, &writer);
	if (!ret)
		ret = dump_revalidate(task, generation, mm, mm_info.vma_count);
	if (!ret)
		ret = criu_snapshot_writer_record(&writer, CRIU_SNAPSHOT_REC_END,
						 0, NULL, 0);
	if (!ret)
		ret = criu_snapshot_writer_finish(&writer);
	if (ret && opened)
		criu_snapshot_writer_abort(&writer);

	thaw:
	if (captured) {
		criu_release_timers(timer_capture);
		criu_release_signals(signal_capture);
	}
	kfree(timer_capture);
	kfree(signal_capture);
	/* Thaw is unconditional once freeze succeeded, including writer failures. */
	pr_info("criu_dump: thaw begin pid=%d\n", vpid);
	thaw_ret = criu_thaw(freeze_ctx);
	pr_info("criu_dump: thaw end pid=%d ret=%d\n", vpid, thaw_ret);
	if (!ret && thaw_ret)
		ret = thaw_ret;
out:
	if (mm)
		mmput(mm);
	if (task)
		put_task_struct(task);
	return ret;
}

int criu_dump_process_tree(pid_t vpid, const char *path)
{
	struct task_struct *task = NULL;
	struct criu_freeze_ctx *freeze_ctx = NULL;
	struct criu_snapshot_header header;
	struct criu_snapshot_writer writer;
	struct criu_dump_shared_ctx shared_ctx;
	struct criu_freeze_process_view root_view;
	u64 generation, frozen_generation;
	unsigned int process_count;
	int ret, thaw_ret;
	bool opened = false;
	bool shared_ctx_ready = false;

	if (vpid <= 0 || !path || !*path)
		return -EINVAL;
	task = criu_target_get(&generation);
	if (!task || task_pid_vnr(task) != vpid) {
		if (task)
			put_task_struct(task);
		return -ESRCH;
	}
	ret = criu_freeze(vpid, true, &freeze_ctx);
	if (ret)
		goto out;
	ret = criu_freeze_generation(freeze_ctx, &frozen_generation);
	if (!ret && frozen_generation != generation)
		ret = -EAGAIN;
	if (!ret)
		ret = criu_freeze_process_count(freeze_ctx, &process_count);
	if (!ret && (!process_count ||
		     criu_freeze_process_get(freeze_ctx, 0, &root_view)))
		ret = -EAGAIN;
	if (ret)
		goto thaw;
	memset(&header, 0, sizeof(header));
	header.magic = CRIU_SNAPSHOT_MAGIC;
	header.version = CRIU_SNAPSHOT_VERSION;
	header.header_size = CRIU_SNAPSHOT_HEADER_SIZE;
	header.flags = CRIU_SNAPSHOT_F_PSTREE;
#ifdef CONFIG_ARM64
	header.arch = 183;
#elif defined(CONFIG_X86_64)
	header.arch = 62;
#endif
	header.page_size = PAGE_SIZE;
	header.pid = root_view.pid;
	header.tgid = root_view.tgid;
	header.freeze_generation = generation;
	header.flags |= CRIU_SNAPSHOT_F_SIGNAL_TIMERS;
	ret = dump_tree_validate_process_isolation(freeze_ctx, process_count);
	if (ret)
		goto thaw;
	ret = criu_dump_shared_ctx_init(&shared_ctx);
	if (ret)
		goto thaw;
	shared_ctx_ready = true;
	ret = criu_snapshot_writer_open(&writer, path, &header);
	if (ret)
		goto thaw;
	opened = true;
	ret = criu_dump_pstree(freeze_ctx, &writer);
	if (!ret) {
		unsigned int i;

		for (i = 0; i < process_count && !ret; i++) {
			struct criu_freeze_process_view view;

			ret = criu_freeze_process_get(freeze_ctx, i, &view);
			if (ret)
				break;
			criu_snapshot_writer_set_process_owner(&writer, view.pid);
			ret = criu_dump_task_ids_process(&shared_ctx, &view,
							 &writer);
			if (!ret)
				ret = criu_dump_task_process(&view, &writer);
			if (!ret)
				ret = criu_dump_process_threads(freeze_ctx, i,
								&writer);
			if (!ret)
				ret = criu_dump_mm_process(&view, &writer);
			if (!ret)
				ret = criu_dump_files_process(freeze_ctx, i, &view, &writer, &shared_ctx);
			if (!ret) {
				struct criu_signal_capture *signal_capture;
				struct criu_timer_capture *timer_capture;

				signal_capture = kzalloc(sizeof(*signal_capture),
							 GFP_KERNEL);
				timer_capture = kzalloc(sizeof(*timer_capture),
							GFP_KERNEL);
				if (!signal_capture || !timer_capture) {
					kfree(signal_capture);
					kfree(timer_capture);
					ret = -ENOMEM;
					break;
				}
				ret = criu_collect_process_signals(freeze_ctx, i,
								  signal_capture);
				if (!ret)
					ret = criu_collect_process_timers(
						freeze_ctx, i, timer_capture);
				if (!ret)
					ret = criu_emit_signals(signal_capture,
								&writer);
				if (!ret)
					ret = criu_emit_timers(timer_capture,
							       &writer);
				criu_release_timers(timer_capture);
				criu_release_signals(signal_capture);
				kfree(timer_capture);
				kfree(signal_capture);
			}
		}
		criu_snapshot_writer_set_process_owner(&writer, 0);
	}
	if (!ret)
		ret = criu_snapshot_writer_record(&writer,
						  CRIU_SNAPSHOT_REC_END,
						  0, NULL, 0);
	if (!ret)
		ret = criu_snapshot_writer_finish(&writer);
	if (ret && opened)
		criu_snapshot_writer_abort(&writer);
thaw:
	if (shared_ctx_ready)
		criu_dump_shared_ctx_destroy(&shared_ctx);
	thaw_ret = criu_thaw(freeze_ctx);
	if (!ret && thaw_ret)
		ret = thaw_ret;
out:
	if (task)
		put_task_struct(task);
	return ret;
}
