/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/hrtimer.h>
#include <linux/sched/signal.h>
#include <linux/posix-timers.h>
#include <linux/sched/cputime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "criu_kernel.h"
#include "dump_timers.h"
#include "posix-timers.h"
#include "snapshot_writer.h"

static u64 remaining_real_ns(struct signal_struct *signal)
{
	ktime_t remaining = __hrtimer_get_remaining(&signal->real_timer, true);

	if (!hrtimer_active(&signal->real_timer))
		return 0;
	if (remaining <= 0)
		return NSEC_PER_USEC;
	return ktime_to_ns(remaining);
}

static void capture_itimers(struct task_struct *task,
			    struct criu_snapshot_itimer_entry *entries)
{
	struct signal_struct *signal = task->signal;
	unsigned long irq_flags;
	u64 samples[CPUCLOCK_MAX];
	u64 value;
	unsigned int i;

	spin_lock_irqsave(&task->sighand->siglock, irq_flags);
	entries[0].kind = CRIU_SNAPSHOT_ITIMER_REAL;
	entries[0].interval_ns = ktime_to_ns(signal->it_real_incr);
	entries[0].remaining_ns = remaining_real_ns(signal);
	for (i = 0; i < 2; i++) {
		unsigned int source = i == 0 ? CPUCLOCK_VIRT : CPUCLOCK_PROF;
		struct cpu_itimer *timer = &signal->it[source];
		unsigned int kind = source == CPUCLOCK_VIRT ?
			CRIU_SNAPSHOT_ITIMER_VIRTUAL : CRIU_SNAPSHOT_ITIMER_PROF;

		entries[i + 1].kind = kind;
		entries[i + 1].interval_ns = timer->incr;
		value = timer->expires;
		entries[i + 1].remaining_ns = 0;
		if (value) {
			samples[CPUCLOCK_PROF] =
				atomic64_read(&signal->cputimer.cputime_atomic.utime) +
				atomic64_read(&signal->cputimer.cputime_atomic.stime);
			samples[CPUCLOCK_VIRT] =
				atomic64_read(&signal->cputimer.cputime_atomic.utime);
			if (value <= samples[source])
				value = TICK_NSEC;
			else
				value -= samples[source];
			entries[i + 1].remaining_ns = value;
		}
	}
	spin_unlock_irqrestore(&task->sighand->siglock, irq_flags);
}

static u64 timespec_ns(const struct timespec64 *value)
{
	s64 ns = timespec64_to_ns(value);

	return ns > 0 ? (u64)ns : 0;
}

static int capture_posix_timer(struct task_struct *leader,
			       struct k_itimer *timer,
			       struct criu_snapshot_posix_timer_entry *entry)
{
	struct itimerspec64 setting;
	unsigned long irq_flags;
	int notify_tid = 0;
	u32 signo = 0;
	u64 value = 0;
	int ret = 0;

	memset(&setting, 0, sizeof(setting));
	spin_lock_irqsave(&timer->it_lock, irq_flags);
	if (!timer->kclock || !timer->kclock->timer_get) {
		ret = -EOPNOTSUPP;
		goto unlock_timer;
	}
	timer->kclock->timer_get(timer, &setting);
	if (timer->sigq) {
		signo = timer->sigq->info.si_signo;
		value = (u64)timer->sigq->info.si_value.sival_ptr;
	}
	if (timer->it_sigev_notify == SIGEV_THREAD_ID && timer->it_pid)
		notify_tid = pid_vnr(timer->it_pid);
	spin_lock(&leader->sighand->siglock);
	if (timer->it_signal != leader->signal)
		ret = -EAGAIN;
	spin_unlock(&leader->sighand->siglock);
	if (ret)
		goto unlock_timer;
	memset(entry, 0, sizeof(*entry));
	entry->timer_id = (u32)timer->it_id;
	entry->clock_id = (u32)timer->it_clock;
	entry->signo = signo;
	entry->sigev_notify = (u32)timer->it_sigev_notify;
	entry->flags = timer->it_active ? CRIU_SNAPSHOT_POSIX_TIMER_F_ARMED : 0;
	if (notify_tid) {
		entry->flags |= CRIU_SNAPSHOT_POSIX_TIMER_F_HAS_NOTIFY_TID;
		entry->notify_tid = notify_tid;
	}
	entry->overrun = timer->it_overrun < 0 ? 0 : (u32)timer->it_overrun;
	entry->sival_ptr = value;
	entry->interval_ns = timespec_ns(&setting.it_interval);
	entry->remaining_ns = timespec_ns(&setting.it_value);
unlock_timer:
	spin_unlock_irqrestore(&timer->it_lock, irq_flags);
	return ret;
}

static bool timer_notify_tid_in_scope(struct criu_freeze_ctx *ctx,
				      unsigned int process_index,
				      bool process_scoped,
				      int notify_tid)
{
	unsigned int count, i;
	int ret;

	if (!notify_tid)
		return true;
	ret = process_scoped ?
		criu_freeze_process_task_count(ctx, process_index, &count) :
		criu_freeze_task_count(ctx, &count);
	if (ret)
		return false;
	for (i = 0; i < count; i++) {
		struct criu_freeze_task_view view;

		ret = process_scoped ?
			criu_freeze_process_task_get(ctx, process_index, i, &view) :
			criu_freeze_task_get(ctx, i, &view);
		if (ret)
			return false;
		if (view.tid == notify_tid)
			return true;
	}
	return false;
}

static int collect_timers_for_process(struct criu_freeze_ctx *ctx,
				      unsigned int process_index,
				      bool process_scoped,
				      struct criu_timer_capture *capture)
{
	struct criu_freeze_task_view view;
	struct k_itimer **timers = NULL;
	struct k_itimer *timer;
	unsigned long irq_flags;
	unsigned int count = 0, i;
	int ret;

	if (!ctx || !capture)
		return -EINVAL;
	memset(capture, 0, sizeof(*capture));
	ret = process_scoped ?
		criu_freeze_process_task_get(ctx, process_index, 0, &view) :
		criu_freeze_task_get(ctx, 0, &view);
	if (ret)
		return ret;
	capture_itimers(view.task, capture->itimers);
	spin_lock_irqsave(&view.task->sighand->siglock, irq_flags);
	list_for_each_entry(timer, &view.task->signal->posix_timers, list)
		count++;
	spin_unlock_irqrestore(&view.task->sighand->siglock, irq_flags);
	if (!count)
		return 0;
	timers = kcalloc(count, sizeof(*timers), GFP_KERNEL);
	if (!timers)
		return -ENOMEM;
	spin_lock_irqsave(&view.task->sighand->siglock, irq_flags);
	i = 0;
	list_for_each_entry(timer, &view.task->signal->posix_timers, list) {
		if (i == count) {
			ret = -EAGAIN;
			goto unlock_list;
		}
		timers[i++] = timer;
	}
	spin_unlock_irqrestore(&view.task->sighand->siglock, irq_flags);
	capture->posix = kcalloc(count, sizeof(*capture->posix), GFP_KERNEL);
	if (!capture->posix) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < count; i++) {
		ret = capture_posix_timer(view.task, timers[i],
					  &capture->posix[capture->posix_count]);
		if (ret)
			goto out;
		if (!timer_notify_tid_in_scope(ctx, process_index,
				process_scoped,
				capture->posix[capture->posix_count].notify_tid)) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		capture->posix_count++;
	}
	for (i = 1; i < capture->posix_count; i++) {
		struct criu_snapshot_posix_timer_entry item = capture->posix[i];
		unsigned int j = i;

		while (j && capture->posix[j - 1].timer_id > item.timer_id) {
			capture->posix[j] = capture->posix[j - 1];
			j--;
		}
		capture->posix[j] = item;
	}
	ret = 0;
out:
	kfree(timers);
	if (ret)
		criu_release_timers(capture);
	return ret;
unlock_list:
	spin_unlock_irqrestore(&view.task->sighand->siglock, irq_flags);
	kfree(timers);
	return ret;
}

int criu_collect_timers(struct criu_freeze_ctx *ctx,
			struct criu_timer_capture *capture)
{
	return collect_timers_for_process(ctx, 0, false, capture);
}

int criu_collect_process_timers(struct criu_freeze_ctx *ctx,
				unsigned int process_index,
				struct criu_timer_capture *capture)
{
	return collect_timers_for_process(ctx, process_index, true, capture);
}

int criu_emit_timers(const struct criu_timer_capture *capture,
		     struct criu_snapshot_writer *writer)
{
	struct criu_snapshot_itimers_header itimer_header;
	struct criu_snapshot_posix_timers_header posix_header;
	u8 *payload;
	size_t length;
	int ret;

	if (!capture || !writer)
		return -EINVAL;
	memset(&itimer_header, 0, sizeof(itimer_header));
	itimer_header.version = CRIU_SNAPSHOT_ITIMERS_VERSION;
	itimer_header.entry_count = CRIU_SNAPSHOT_ITIMER_COUNT;
	itimer_header.entry_size = CRIU_SNAPSHOT_ITIMER_ENTRY_SIZE;
	length = sizeof(itimer_header) + sizeof(capture->itimers);
	payload = kmalloc(length, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	memcpy(payload, &itimer_header, sizeof(itimer_header));
	memcpy(payload + sizeof(itimer_header), capture->itimers,
	       sizeof(capture->itimers));
	ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_ITIMERS,
					  0, payload, length);
	kfree(payload);
	if (ret)
		return ret;
	memset(&posix_header, 0, sizeof(posix_header));
	posix_header.version = CRIU_SNAPSHOT_POSIX_TIMERS_VERSION;
	posix_header.entry_count = capture->posix_count;
	posix_header.entry_size = CRIU_SNAPSHOT_POSIX_TIMER_ENTRY_SIZE;
	length = sizeof(posix_header) +
		(size_t)capture->posix_count * sizeof(*capture->posix);
	payload = kmalloc(length, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	memcpy(payload, &posix_header, sizeof(posix_header));
	if (capture->posix_count)
		memcpy(payload + sizeof(posix_header), capture->posix,
		       (size_t)capture->posix_count * sizeof(*capture->posix));
	ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_POSIX_TIMERS,
					  0, payload, length);
	kfree(payload);
	return ret;
}

void criu_release_timers(struct criu_timer_capture *capture)
{
	if (!capture)
		return;
	kfree(capture->posix);
	memset(capture, 0, sizeof(*capture));
}
