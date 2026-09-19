/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "criu_kernel.h"
#include "dump_signals.h"
#include "snapshot_writer.h"

#define CRIU_SIGNAL_MAX_ENTRIES \
	(CRIU_SNAPSHOT_MAX_RECORD_SIZE / CRIU_SNAPSHOT_SIGNAL_QUEUE_ENTRY_SIZE)

static unsigned long pending_mask(const struct sigpending *pending)
{
	return pending->signal.sig[0];
}

static u32 count_pending_locked(const struct sigpending *pending,
				unsigned long *seen)
{
	struct sigqueue *queue;
	u32 count = 0;
	unsigned int signo;

	*seen = 0;
	list_for_each_entry(queue, &pending->list, list) {
		signo = queue->info.si_signo;
		if (signo >= 1 && signo <= 64)
			*seen |= 1UL << (signo - 1);
		count++;
	}
	for (signo = 1; signo <= 64; signo++)
		if ((pending_mask(pending) & (1UL << (signo - 1))) &&
		    !(*seen & (1UL << (signo - 1))))
			count++;
	return count;
}

static int preflight_queue(struct task_struct *task,
			   struct criu_signal_queue_chunk *chunk)
{
	unsigned long irq_flags, seen;
	struct sighand_struct *sighand;
	struct sigpending *pending;
	u32 count;

	if (!task || !task->sighand || !task->signal)
		return -EINVAL;
	sighand = task->sighand;
	pending = chunk->scope == CRIU_SNAPSHOT_SIGNAL_SCOPE_SHARED ?
		&task->signal->shared_pending : &task->pending;
	spin_lock_irqsave(&sighand->siglock, irq_flags);
	count = count_pending_locked(pending, &seen);
	chunk->pending_mask = pending_mask(pending);
	spin_unlock_irqrestore(&sighand->siglock, irq_flags);
	if (count > CRIU_SIGNAL_MAX_ENTRIES)
		return -E2BIG;
	chunk->total_count = count;
	chunk->first_index = 0;
	chunk->entry_count = count;
	return 0;
}

static void fill_synthetic(struct criu_snapshot_signal_queue_entry *entry,
			   unsigned int signo)
{
	siginfo_t info;

	memset(&info, 0, sizeof(info));
	info.si_signo = signo;
	info.si_code = SI_USER;
	memcpy(entry->siginfo, &info, sizeof(entry->siginfo));
	entry->signo = signo;
}

static int copy_queue_locked(struct sigpending *pending,
			     struct criu_signal_queue_chunk *chunk)
{
	struct sigqueue *queue;
	unsigned long seen = 0;
	u32 index = 0;
	unsigned int signo;

	list_for_each_entry(queue, &pending->list, list) {
		if (index >= chunk->entry_count ||
		    queue->info.si_signo < 1 || queue->info.si_signo > 64)
			return -EAGAIN;
		signo = queue->info.si_signo;
		chunk->entries[index].signo = signo;
		copy_siginfo_to_external((siginfo_t *)chunk->entries[index].siginfo,
					 &queue->info);
		seen |= 1UL << (signo - 1);
		index++;
	}
	for (signo = 1; signo <= 64; signo++)
		if ((pending_mask(pending) & (1UL << (signo - 1))) &&
		    !(seen & (1UL << (signo - 1)))) {
			if (index >= chunk->entry_count)
				return -EAGAIN;
			fill_synthetic(&chunk->entries[index], signo);
			index++;
		}
	return index == chunk->entry_count ? 0 : -EAGAIN;
}

static int copy_queue(struct task_struct *task,
		      struct criu_signal_queue_chunk *chunk)
{
	unsigned long irq_flags;
	struct sighand_struct *sighand;
	struct sigpending *pending;
	int ret;

	sighand = task->sighand;
	pending = chunk->scope == CRIU_SNAPSHOT_SIGNAL_SCOPE_SHARED ?
		&task->signal->shared_pending : &task->pending;
	spin_lock_irqsave(&sighand->siglock, irq_flags);
	ret = copy_queue_locked(pending, chunk);
	spin_unlock_irqrestore(&sighand->siglock, irq_flags);
	return ret;
}

static int copy_actions(struct task_struct *task,
			struct criu_snapshot_sigaction_entry *actions)
{
	unsigned long irq_flags;
	struct sighand_struct *sighand;
	unsigned int i;

	if (!task || !task->sighand)
		return -EINVAL;
	sighand = task->sighand;
	spin_lock_irqsave(&sighand->siglock, irq_flags);
	for (i = 0; i < CRIU_SNAPSHOT_SIGACTION_COUNT; i++) {
		const struct k_sigaction *action = &sighand->action[i];

		actions[i].signo = i + 1;
		actions[i].handler = (u64)action->sa.sa_handler;
		actions[i].flags = action->sa.sa_flags;
		actions[i].restorer = (u64)action->sa.sa_restorer;
		memcpy(&actions[i].mask, &action->sa.sa_mask,
		       sizeof(actions[i].mask));
		actions[i].mask_extended = 0;
	}
	spin_unlock_irqrestore(&sighand->siglock, irq_flags);
	return 0;
}

static int collect_signals_for_process(struct criu_freeze_ctx *ctx,
				       unsigned int process_index,
				       bool process_scoped,
				       struct criu_signal_capture *capture)
{
	struct criu_freeze_task_view view;
	unsigned int task_count, i;
	int ret;

	if (!ctx || !capture)
		return -EINVAL;
	BUILD_BUG_ON(sizeof(siginfo_t) != CRIU_SNAPSHOT_SIGINFO_SIZE);
	memset(capture, 0, sizeof(*capture));
	ret = process_scoped ?
		criu_freeze_process_task_count(ctx, process_index, &task_count) :
		criu_freeze_task_count(ctx, &task_count);
	if (ret || !task_count)
		return ret ? ret : -ESRCH;
	capture->queue_count = task_count + 1;
	capture->queues = kcalloc(capture->queue_count,
				 sizeof(*capture->queues), GFP_KERNEL);
	if (!capture->queues)
		return -ENOMEM;
	ret = process_scoped ?
		criu_freeze_process_task_get(ctx, process_index, 0, &view) :
		criu_freeze_task_get(ctx, 0, &view);
	if (ret)
		goto fail;
	ret = copy_actions(view.task, capture->actions);
	if (ret)
		goto fail;
	capture->queues[0].scope = CRIU_SNAPSHOT_SIGNAL_SCOPE_SHARED;
	capture->queues[0].owner_tid = 0;
	ret = preflight_queue(view.task, &capture->queues[0]);
	if (ret)
		goto fail;
	for (i = 0; i < task_count; i++) {
		struct criu_signal_queue_chunk *chunk = &capture->queues[i + 1];

		ret = process_scoped ?
			criu_freeze_process_task_get(ctx, process_index, i, &view) :
			criu_freeze_task_get(ctx, i, &view);
		if (ret)
			goto fail;
		chunk->scope = CRIU_SNAPSHOT_SIGNAL_SCOPE_PRIVATE;
		chunk->owner_tid = view.tid;
		ret = preflight_queue(view.task, chunk);
		if (ret)
			goto fail;
	}
	for (i = 0; i < capture->queue_count; i++) {
		struct criu_signal_queue_chunk *chunk = &capture->queues[i];

		if (chunk->entry_count) {
			chunk->entries = kcalloc(chunk->entry_count,
						 sizeof(*chunk->entries),
						 GFP_KERNEL);
			if (!chunk->entries) {
				ret = -ENOMEM;
				goto fail;
			}
		}
	}
	ret = process_scoped ?
		criu_freeze_process_task_get(ctx, process_index, 0, &view) :
		criu_freeze_task_get(ctx, 0, &view);
	if (ret)
		goto fail;
	ret = copy_queue(view.task, &capture->queues[0]);
	if (ret)
		goto fail;
	for (i = 0; i < task_count; i++) {
		ret = process_scoped ?
			criu_freeze_process_task_get(ctx, process_index, i, &view) :
			criu_freeze_task_get(ctx, i, &view);
		if (ret)
			goto fail;
		ret = copy_queue(view.task, &capture->queues[i + 1]);
		if (ret)
			goto fail;
	}
	for (i = 2; i < capture->queue_count; i++) {
		struct criu_signal_queue_chunk item = capture->queues[i];
		unsigned int j = i;

		while (j > 1 &&
		       capture->queues[j - 1].owner_tid > item.owner_tid) {
			capture->queues[j] = capture->queues[j - 1];
			j--;
		}
		capture->queues[j] = item;
	}
	return 0;
fail:
	criu_release_signals(capture);
	return ret;
}

int criu_collect_signals(struct criu_freeze_ctx *ctx,
			 struct criu_signal_capture *capture)
{
	return collect_signals_for_process(ctx, 0, false, capture);
}

int criu_collect_process_signals(struct criu_freeze_ctx *ctx,
				 unsigned int process_index,
				 struct criu_signal_capture *capture)
{
	return collect_signals_for_process(ctx, process_index, true, capture);
}

int criu_emit_signals(const struct criu_signal_capture *capture,
		      struct criu_snapshot_writer *writer)
{
	struct criu_snapshot_sigaction_header action_header;
	struct criu_snapshot_signal_queue_header queue_header;
	u8 *action_payload;
	size_t action_payload_len;
	unsigned int i;
	int ret;

	if (!capture || !writer)
		return -EINVAL;
	action_header.version = CRIU_SNAPSHOT_SIGACTION_VERSION;
	action_header.entry_count = CRIU_SNAPSHOT_SIGACTION_COUNT;
	action_header.entry_size = CRIU_SNAPSHOT_SIGACTION_ENTRY_SIZE;
	action_header.reserved = 0;
	action_payload_len = sizeof(action_header) + sizeof(capture->actions);
	action_payload = kmalloc(action_payload_len, GFP_KERNEL);
	if (!action_payload)
		return -ENOMEM;
	memcpy(action_payload, &action_header, sizeof(action_header));
	memcpy(action_payload + sizeof(action_header), capture->actions,
	       sizeof(capture->actions));
	ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_SIGACTION,
					  0, action_payload, action_payload_len);
	kfree(action_payload);
	if (ret)
		return ret;
	for (i = 0; i < capture->queue_count; i++) {
		const struct criu_signal_queue_chunk *chunk = &capture->queues[i];
		u8 *payload;
		size_t payload_len = sizeof(queue_header) +
			(size_t)chunk->entry_count * sizeof(*chunk->entries);

		payload = kmalloc(payload_len, GFP_KERNEL);
		if (!payload)
			return -ENOMEM;
		memset(&queue_header, 0, sizeof(queue_header));
		queue_header.version = CRIU_SNAPSHOT_SIGNAL_QUEUE_VERSION;
		queue_header.scope = chunk->scope;
		queue_header.owner_tid = chunk->owner_tid;
		queue_header.total_count = chunk->total_count;
		queue_header.first_index = chunk->first_index;
		queue_header.entry_count = chunk->entry_count;
		queue_header.entry_size = CRIU_SNAPSHOT_SIGNAL_QUEUE_ENTRY_SIZE;
		queue_header.siginfo_size = CRIU_SNAPSHOT_SIGINFO_SIZE;
		queue_header.pending_mask = chunk->pending_mask;
		memcpy(payload, &queue_header, sizeof(queue_header));
		if (chunk->entry_count)
			memcpy(payload + sizeof(queue_header), chunk->entries,
			       (size_t)chunk->entry_count * sizeof(*chunk->entries));
		ret = criu_snapshot_writer_record(writer,
				CRIU_SNAPSHOT_REC_SIGNAL_QUEUE, 0,
				payload, payload_len);
		kfree(payload);
		if (ret)
			return ret;
	}
	return 0;
}

void criu_release_signals(struct criu_signal_capture *capture)
{
	unsigned int i;

	if (!capture)
		return;
	for (i = 0; i < capture->queue_count; i++)
		kfree(capture->queues[i].entries);
	kfree(capture->queues);
	memset(capture, 0, sizeof(*capture));
}
