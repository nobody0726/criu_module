/* SPDX-License-Identifier: GPL-2.0 */

static bool criu_spike_task_probe_selected(void)
{
	return !strcmp(criu_spike_args.probe, "1.2") ||
	       !strcmp(criu_spike_args.probe, "1.3") ||
	       !strcmp(criu_spike_args.probe, "1.4");
}

static void criu_spike_task_report(const char *result, const char *reason,
					   pid_t task_pid, pid_t task_tgid,
					   const char *comm)
{
	scnprintf(criu_spike_status, sizeof(criu_spike_status),
		  "protocol=1\nstate=READY\nresult=%s\n", result);
	scnprintf(criu_spike_report, sizeof(criu_spike_report),
		  "protocol=1\n"
		  "probe=%s\n"
		  "target_pid=%d\n"
		  "anon_addr=0x%lx\n"
		  "shared_addr=0x%lx\n"
		  "guard_addr=0x%lx\n"
		  "insert_addr=0x%lx\n"
		  "target_valid=1\n"
		  "task_pid=%d\n"
		  "task_tgid=%d\n"
		  "task_comm=%s\n"
		  "result=%s\n"
		  "reason=%s\n",
		  criu_spike_args.probe, criu_spike_args.target_pid,
		  criu_spike_args.anon_addr, criu_spike_args.shared_addr,
		  criu_spike_args.guard_addr, criu_spike_args.insert_addr,
		  task_pid, task_tgid, comm, result, reason);
	scnprintf(criu_spike_vmas, sizeof(criu_spike_vmas),
		  "protocol=1\nprobe=%s\nresult=%s\n"
		  "task_pid=%d\ntask_tgid=%d\n"
		  "vmas=not-probed\n",
		  criu_spike_args.probe, result, task_pid, task_tgid);
}

static int criu_spike_run_task_lookup(bool with_rcu, bool with_reference)
{
	struct pid *pid;
	struct task_struct *task;
	char comm[TASK_COMM_LEN] = "";
	pid_t task_pid = 0;
	pid_t task_tgid = 0;

	if (with_rcu)
		rcu_read_lock();
	pid = find_vpid(criu_spike_args.target_pid);
	if (with_reference)
		task = get_pid_task(pid, PIDTYPE_PID);
	else
		task = pid_task(pid, PIDTYPE_PID);
	if (task) {
		task_pid = task_pid_nr(task);
		task_tgid = task_tgid_nr(task);
		get_task_comm(comm, task);
	}
	if (with_rcu)
		rcu_read_unlock();

	if (!task) {
		criu_spike_task_report("WRONG-VALUE", "task lookup returned NULL",
				       0, 0, "");
		return -ESRCH;
	}

	if (with_reference)
		put_task_struct(task);

	criu_spike_task_report("OK", "", task_pid, task_tgid, comm);
	return 0;
}

#ifdef CRIU_SPIKE_COMPILE_1_1
static int criu_spike_compile_1_1(void)
{
	struct task_struct *task;

	task = find_get_task_by_vpid(criu_spike_args.target_pid);
	if (!task)
		return -ESRCH;
	put_task_struct(task);
	return 0;
}
#endif

static int criu_spike_run_task_probe(void)
{
	if (!strcmp(criu_spike_args.probe, "1.2"))
		return criu_spike_run_task_lookup(true, false);
	if (!strcmp(criu_spike_args.probe, "1.3"))
		return criu_spike_run_task_lookup(false, true);
	if (!strcmp(criu_spike_args.probe, "1.4")) {
		(void)criu_spike_run_task_lookup(false, false);
		criu_spike_task_report("UNSAFE", "missing RCU protection", 0, 0, "");
		return 0;
	}

	return -EOPNOTSUPP;
}
