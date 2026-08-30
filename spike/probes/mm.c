/* SPDX-License-Identifier: GPL-2.0 */

struct criu_spike_vma_snapshot {
	unsigned long start;
	unsigned long end;
	unsigned long flags;
	bool found;
};

static bool criu_spike_mm_probe_selected(void)
{
	return !strcmp(criu_spike_args.probe, "2.2") ||
	       !strcmp(criu_spike_args.probe, "2.3") ||
	       !strcmp(criu_spike_args.probe, "2.4") ||
	       !strcmp(criu_spike_args.probe, "2.5");
}

static void criu_spike_mm_report_base(const char *result, const char *reason)
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
		  "result=%s\n"
		  "reason=%s\n",
		  criu_spike_args.probe, criu_spike_args.target_pid,
		  criu_spike_args.anon_addr, criu_spike_args.shared_addr,
		  criu_spike_args.guard_addr, criu_spike_args.insert_addr,
		  result, reason);
	scnprintf(criu_spike_vmas, sizeof(criu_spike_vmas),
		  "protocol=1\nprobe=%s\nresult=%s\nvmas=not-probed\n",
		  criu_spike_args.probe, result);
}

static void criu_spike_vma_perms(unsigned long flags, char *perms)
{
	perms[0] = flags & VM_READ ? 'r' : '-';
	perms[1] = flags & VM_WRITE ? 'w' : '-';
	perms[2] = flags & VM_EXEC ? 'x' : '-';
	perms[3] = flags & VM_SHARED ? 's' : 'p';
	perms[4] = '\0';
}

static void criu_spike_vmas_append(size_t *used, const char *format, ...)
{
	va_list args;
	int written;

	if (*used >= sizeof(criu_spike_vmas))
		return;

	va_start(args, format);
	written = vscnprintf(criu_spike_vmas + *used,
			     sizeof(criu_spike_vmas) - *used, format, args);
	va_end(args);
	if (written > 0)
		*used += written;
}

static bool criu_spike_vma_contains(const struct vm_area_struct *vma,
				    unsigned long address)
{
	return address >= vma->vm_start && address < vma->vm_end;
}

static void criu_spike_snapshot_vma(struct criu_spike_vma_snapshot *snapshot,
					const struct vm_area_struct *vma)
{
	snapshot->start = vma->vm_start;
	snapshot->end = vma->vm_end;
	snapshot->flags = vma->vm_flags;
	snapshot->found = true;
}

static int criu_spike_run_mm_basic(void)
{
	int mm_users;

	if (!criu_spike_mm) {
		criu_spike_mm_report_base("WRONG-VALUE", "get_task_mm returned NULL");
		return -ESRCH;
	}

	mm_users = atomic_read(&criu_spike_mm->mm_users);
	if (mm_users <= 0) {
		criu_spike_mm_report_base("WRONG-VALUE", "mm_users is not positive");
		return -EINVAL;
	}

	criu_spike_mm_report_base("OK", "");
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "mm_nonnull=1\nmm_users=%d\n", mm_users);
	return 0;
}

static int criu_spike_run_mm_lock(void)
{
	unsigned long first_start = 0;
	unsigned long first_end = 0;

	if (!criu_spike_mm) {
		criu_spike_mm_report_base("WRONG-VALUE", "get_task_mm returned NULL");
		return -ESRCH;
	}

	mmap_read_lock(criu_spike_mm);
	if (criu_spike_mm->mmap) {
		first_start = criu_spike_mm->mmap->vm_start;
		first_end = criu_spike_mm->mmap->vm_end;
	}
	mmap_read_unlock(criu_spike_mm);

	criu_spike_mm_report_base("OK", "");
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "mmap_lock_checked=1\nfirst_vma_start=0x%lx\n"
		  "first_vma_end=0x%lx\n", first_start, first_end);
	return 0;
}

static int criu_spike_run_mm_walk(void)
{
	struct criu_spike_vma_snapshot anon = { 0 };
	struct criu_spike_vma_snapshot shared = { 0 };
	struct vm_area_struct *vma;
	char anon_perms[5] = "----";
	char shared_perms[5] = "----";
	unsigned char anon_byte = 0;
	unsigned char shared_byte = 0;
	size_t used = 0;
	unsigned int count = 0;
	int anon_bytes;
	int shared_bytes;
	const char *result = "OK";
	const char *reason = "";

	if (!criu_spike_mm) {
		criu_spike_mm_report_base("WRONG-VALUE", "get_task_mm returned NULL");
		return -ESRCH;
	}

	mmap_read_lock(criu_spike_mm);
	for (vma = criu_spike_mm->mmap; vma; vma = vma->vm_next) {
		count++;
		criu_spike_vmas_append(&used, "vma=0x%lx-0x%lx flags=0x%lx\n",
				      vma->vm_start, vma->vm_end, vma->vm_flags);
		if (!anon.found && criu_spike_vma_contains(vma,
						   criu_spike_args.anon_addr))
			criu_spike_snapshot_vma(&anon, vma);
		if (!shared.found && criu_spike_vma_contains(vma,
						     criu_spike_args.shared_addr))
			criu_spike_snapshot_vma(&shared, vma);
	}
	mmap_read_unlock(criu_spike_mm);

	if (anon.found)
		criu_spike_vma_perms(anon.flags, anon_perms);
	if (shared.found)
		criu_spike_vma_perms(shared.flags, shared_perms);
	anon_bytes = access_process_vm(criu_spike_task,
				      criu_spike_args.anon_addr,
				      &anon_byte, sizeof(anon_byte), FOLL_FORCE);
	shared_bytes = access_process_vm(criu_spike_task,
					criu_spike_args.shared_addr,
					&shared_byte, sizeof(shared_byte), FOLL_FORCE);
	if (!anon.found || !shared.found || anon_bytes != 1 ||
	    shared_bytes != 1 || anon_byte != 0xa5 || shared_byte != 0x5a) {
		result = "WRONG-VALUE";
		reason = "VMA or magic byte mismatch";
	}

	criu_spike_mm_report_base(result, reason);
	used = scnprintf(criu_spike_vmas, sizeof(criu_spike_vmas),
			 "protocol=1\nprobe=%s\nresult=%s\n",
			 criu_spike_args.probe, result);
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "vma_count=%u\n"
		  "anon_vma_start=0x%lx\nanon_vma_end=0x%lx\n"
		  "anon_vma_perms=%s\nanon_byte=0x%02x\n"
		  "shared_vma_start=0x%lx\nshared_vma_end=0x%lx\n"
		  "shared_vma_perms=%s\nshared_byte=0x%02x\n",
		  count, anon.start, anon.end, anon_perms, anon_byte,
		  shared.start, shared.end, shared_perms, shared_byte);
	criu_spike_vmas_append(&used, "vma_count=%u\nresult=%s\n", count, result);
	return result[0] == 'O' ? 0 : -EINVAL;
}

static int criu_spike_run_mm_path(void)
{
	char *path_buf;
	char *path;
	struct vm_area_struct *vma;

	if (!criu_spike_mm) {
		criu_spike_mm_report_base("WRONG-VALUE", "get_task_mm returned NULL");
		return -ESRCH;
	}

	path_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path_buf) {
		criu_spike_mm_report_base("CRASH", "path buffer allocation failed");
		return -ENOMEM;
	}

	path = ERR_PTR(-ENOENT);
	mmap_read_lock(criu_spike_mm);
	for (vma = criu_spike_mm->mmap; vma; vma = vma->vm_next) {
		if (vma->vm_file) {
			path = d_path(&vma->vm_file->f_path, path_buf, PATH_MAX);
			break;
		}
	}
	mmap_read_unlock(criu_spike_mm);

	if (IS_ERR(path)) {
		kfree(path_buf);
		criu_spike_mm_report_base("WRONG-VALUE", "no file-backed VMA path");
		return -ENOENT;
	}

	criu_spike_mm_report_base("OK", "");
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "file_path=%s\n", path);
	kfree(path_buf);
	return 0;
}

#ifdef CRIU_SPIKE_COMPILE_2_1
static int criu_spike_compile_2_1(void)
{
	if (mmget_not_zero(criu_spike_mm)) {
		mmput(criu_spike_mm);
		return 0;
	}
	return -ESRCH;
}
#endif

static int criu_spike_run_mm_probe(void)
{
	if (!strcmp(criu_spike_args.probe, "2.2"))
		return criu_spike_run_mm_basic();
	if (!strcmp(criu_spike_args.probe, "2.3"))
		return criu_spike_run_mm_lock();
	if (!strcmp(criu_spike_args.probe, "2.4"))
		return criu_spike_run_mm_walk();
	if (!strcmp(criu_spike_args.probe, "2.5"))
		return criu_spike_run_mm_path();
	return -EOPNOTSUPP;
}
