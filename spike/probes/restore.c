/* SPDX-License-Identifier: GPL-2.0 */

static bool criu_spike_restore_compile_selected(void)
{
	return !strcmp(criu_spike_args.probe, "4.1") ||
	       !strcmp(criu_spike_args.probe, "4.2") ||
	       !strcmp(criu_spike_args.probe, "4.3") ||
	       !strcmp(criu_spike_args.probe, "4.4a") ||
	       !strcmp(criu_spike_args.probe, "4.4b") ||
	       !strcmp(criu_spike_args.probe, "4.5a") ||
	       !strcmp(criu_spike_args.probe, "4.5b") ||
	       !strcmp(criu_spike_args.probe, "4.6") ||
	       !strcmp(criu_spike_args.probe, "4.7");
}

static bool criu_spike_restore_probe_selected(void)
{
	return !strcmp(criu_spike_args.probe, "4.8");
}

static void criu_spike_restore_report_base(const char *result,
						 const char *reason)
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

#ifdef CRIU_SPIKE_COMPILE_4_1
static int criu_spike_compile_4_1(void)
{
	struct mm_struct *(*volatile fn)(void) = mm_alloc;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_2
static int criu_spike_compile_4_2(void)
{
	struct vm_area_struct *(*volatile fn)(struct mm_struct *) =
		vm_area_alloc;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_3
static int criu_spike_compile_4_3(void)
{
	int (*volatile fn)(struct mm_struct *, struct vm_area_struct *) =
		insert_vm_struct;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_4A
static int criu_spike_compile_4_4a(void)
{
	unsigned long (*volatile fn)(struct file *, unsigned long,
					     unsigned long, unsigned long,
					     unsigned long, unsigned long,
					     unsigned long *, struct list_head *) = do_mmap;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_4B
static int criu_spike_compile_4_4b(void)
{
	unsigned long (*volatile fn)(struct file *, unsigned long,
					     unsigned long, unsigned long,
					     unsigned long, unsigned long) = vm_mmap;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_5A
static int criu_spike_compile_4_5a(void)
{
	int (*volatile fn)(struct mm_struct *, unsigned long, size_t,
				   struct list_head *) = do_munmap;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_5B
static int criu_spike_compile_4_5b(void)
{
	int (*volatile fn)(unsigned long, size_t) = vm_munmap;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_6
static int criu_spike_compile_4_6(void)
{
	struct pid *(*volatile fn)(struct pid_namespace *, pid_t *, size_t) =
		alloc_pid;

	return fn ? 0 : -EINVAL;
}
#endif

#ifdef CRIU_SPIKE_COMPILE_4_7
static int criu_spike_compile_4_7(void)
{
	int (*volatile fn)(const char *, const char *const *,
				  const char *const *) = kernel_execve;

	return fn ? 0 : -EINVAL;
}
#endif

static int criu_spike_run_restore_compile_probe(void)
{
	const char *symbol = "unknown";
	const char *scope = "none";
	int wrapper_link = 0;
	int ret = 0;

#ifdef CRIU_SPIKE_COMPILE_4_1
	symbol = "mm_alloc";
	ret = criu_spike_compile_4_1();
#elif defined(CRIU_SPIKE_COMPILE_4_2)
	symbol = "vm_area_alloc";
	ret = criu_spike_compile_4_2();
#elif defined(CRIU_SPIKE_COMPILE_4_3)
	symbol = "insert_vm_struct";
	ret = criu_spike_compile_4_3();
#elif defined(CRIU_SPIKE_COMPILE_4_4A)
	symbol = "do_mmap";
	ret = criu_spike_compile_4_4a();
#elif defined(CRIU_SPIKE_COMPILE_4_4B)
	symbol = "vm_mmap";
	scope = "current->mm";
	wrapper_link = 1;
	ret = criu_spike_compile_4_4b();
#elif defined(CRIU_SPIKE_COMPILE_4_5A)
	symbol = "do_munmap";
	ret = criu_spike_compile_4_5a();
#elif defined(CRIU_SPIKE_COMPILE_4_5B)
	symbol = "vm_munmap";
	scope = "current->mm";
	wrapper_link = 1;
	ret = criu_spike_compile_4_5b();
#elif defined(CRIU_SPIKE_COMPILE_4_6)
	symbol = "alloc_pid";
	ret = criu_spike_compile_4_6();
#elif defined(CRIU_SPIKE_COMPILE_4_7)
	symbol = "kernel_execve";
	ret = criu_spike_compile_4_7();
#endif

	if (ret)
		return ret;

	criu_spike_restore_report_base("OK", "compile-time symbol probe");
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "symbol=%s\nwrapper_link=%d\nscope=%s\n", symbol,
		  wrapper_link, scope);
	return 0;
}

static int criu_spike_run_restore_insert(void)
{
	struct vm_area_struct *vma;
	struct page *page;
	unsigned long flags_before = 0;
	unsigned long flags_after = 0;
	int insert_ret = -ENOENT;
	bool found = false;
	bool mixedmap_before;
	bool mixedmap_after;
	const char *result = "UNSAFE";
	const char *reason = "vm_insert_page changes the target VMA";

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		criu_spike_restore_report_base("CRASH", "page allocation failed");
		return -ENOMEM;
	}

	mmap_write_lock(criu_spike_mm);
	for (vma = criu_spike_mm->mmap; vma; vma = vma->vm_next) {
		if (criu_spike_args.insert_addr < vma->vm_start ||
		    criu_spike_args.insert_addr >= vma->vm_end)
			continue;

		found = true;
		flags_before = vma->vm_flags;
		insert_ret = vm_insert_page(vma, criu_spike_args.insert_addr, page);
		flags_after = vma->vm_flags;
		break;
	}
	mmap_write_unlock(criu_spike_mm);

	mixedmap_before = !!(flags_before & VM_MIXEDMAP);
	mixedmap_after = !!(flags_after & VM_MIXEDMAP);
	put_page(page);

	if (!found || insert_ret || mixedmap_before || !mixedmap_after) {
		criu_spike_restore_report_base("WRONG-VALUE",
					       "vm_insert_page contract mismatch");
		return -EINVAL;
	}

	criu_spike_restore_report_base(result, reason);
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "insert_ret=%d\nflags_before=0x%lx\nflags_after=0x%lx\n"
		  "vm_mixedmap_before=%d\nvm_mixedmap_after=%d\n"
		  "vm_mixedmap_added=%d\n", insert_ret, flags_before,
		  flags_after, mixedmap_before, mixedmap_after,
		  !mixedmap_before && mixedmap_after);
	return 0;
}

static int criu_spike_run_restore_probe(void)
{
	return criu_spike_run_restore_insert();
}
