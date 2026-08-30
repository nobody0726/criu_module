/* SPDX-License-Identifier: GPL-2.0 */

#define CRIU_SPIKE_PAGE_UNMAPPED 0x1000UL

static bool criu_spike_page_probe_selected(void)
{
	return !strcmp(criu_spike_args.probe, "3.2") ||
	       !strcmp(criu_spike_args.probe, "3.3") ||
	       !strcmp(criu_spike_args.probe, "3.4") ||
	       !strcmp(criu_spike_args.probe, "3.5");
}

static void criu_spike_page_report_base(const char *result, const char *reason)
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

static long criu_spike_gup_read_byte(unsigned long address,
					 unsigned char *byte)
{
	struct page *page = NULL;
	int locked = 1;
	long ret;
	void *mapped;

	mmap_read_lock(criu_spike_mm);
	ret = get_user_pages_remote(criu_spike_mm, address, 1, FOLL_FORCE,
					&page, NULL, &locked);
	if (locked)
		mmap_read_unlock(criu_spike_mm);
	if (ret <= 0)
		return ret;
	if (!page)
		return -EFAULT;

	mapped = kmap(page);
	*byte = *((unsigned char *)mapped + (address & (PAGE_SIZE - 1)));
	kunmap(page);
	put_page(page);
	return ret;
}

static unsigned long criu_spike_find_unmapped(void)
{
	struct vm_area_struct *vma;
	unsigned long candidate = CRIU_SPIKE_PAGE_UNMAPPED;

	mmap_read_lock(criu_spike_mm);
	for (vma = criu_spike_mm->mmap; vma; vma = vma->vm_next) {
		if (candidate < vma->vm_start &&
		    candidate <= vma->vm_start - PAGE_SIZE)
			break;
		if (candidate < vma->vm_end)
			candidate = PAGE_ALIGN(vma->vm_end);
		if (candidate > TASK_SIZE - PAGE_SIZE) {
			candidate = 0;
			break;
		}
	}
	mmap_read_unlock(criu_spike_mm);
	return candidate;
}

static int criu_spike_run_page_gup(void)
{
	unsigned char byte = 0;
	long ret;
	const char *result = "OK";
	const char *reason = "";

	ret = criu_spike_gup_read_byte(criu_spike_args.anon_addr, &byte);
	if (ret != 1 || byte != 0xa5) {
		result = "WRONG-VALUE";
		reason = "GUP did not return the expected page";
	}

	criu_spike_page_report_base(result, reason);
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "gup_ret=%ld\ngup_byte=0x%02x\n", ret, byte);
	return result[0] == 'O' ? 0 : -EINVAL;
}

static int criu_spike_run_page_access(void)
{
	unsigned char anon_byte = 0;
	unsigned char shared_byte = 0;
	int anon_ret;
	int shared_ret;
	const char *result = "OK";
	const char *reason = "";

	anon_ret = access_process_vm(criu_spike_task, criu_spike_args.anon_addr,
				     &anon_byte, sizeof(anon_byte), FOLL_FORCE);
	shared_ret = access_process_vm(criu_spike_task,
				       criu_spike_args.shared_addr, &shared_byte,
				       sizeof(shared_byte), FOLL_FORCE);
	if (anon_ret != 1 || anon_byte != 0xa5 || shared_ret != 1 ||
	    shared_byte != 0x5a) {
		result = "WRONG-VALUE";
		reason = "access_process_vm returned the wrong data";
	}

	criu_spike_page_report_base(result, reason);
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "access_anon_ret=%d\naccess_anon_byte=0x%02x\n"
		  "access_shared_ret=%d\naccess_shared_byte=0x%02x\n",
		  anon_ret, anon_byte, shared_ret, shared_byte);
	return result[0] == 'O' ? 0 : -EINVAL;
}

static int criu_spike_run_page_invalid(void)
{
	unsigned long address = criu_spike_find_unmapped();
	unsigned char byte = 0;
	long gup_ret;
	int access_ret;
	const char *result = "OK";
	const char *reason = "";

	if (!address) {
		criu_spike_page_report_base("WRONG-VALUE", "no unmapped page gap found");
		return -ENOMEM;
	}
	gup_ret = criu_spike_gup_read_byte(address, &byte);
	access_ret = access_process_vm(criu_spike_task, address, &byte,
					       sizeof(byte), FOLL_FORCE);
	if (gup_ret > 0 || access_ret != 0) {
		result = "WRONG-VALUE";
		reason = "unmapped access did not fail cleanly";
	}

	criu_spike_page_report_base(result, reason);
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "unmapped_addr=0x%lx\ngup_unmapped_ret=%ld\n"
		  "access_unmapped_ret=%d\n", address, gup_ret, access_ret);
	return result[0] == 'O' ? 0 : -EINVAL;
}

static int criu_spike_run_page_guard(void)
{
	unsigned char byte = 0;
	long gup_ret;
	int access_ret;

	gup_ret = criu_spike_gup_read_byte(criu_spike_args.guard_addr, &byte);
	access_ret = access_process_vm(criu_spike_task,
				       criu_spike_args.guard_addr, &byte,
				       sizeof(byte), FOLL_FORCE);
	criu_spike_page_report_base("OK", "actual PROT_NONE return values recorded");
	scnprintf(criu_spike_report + strlen(criu_spike_report),
		  sizeof(criu_spike_report) - strlen(criu_spike_report),
		  "gup_guard_ret=%ld\naccess_guard_ret=%d\n", gup_ret,
		  access_ret);
	return 0;
}

#ifdef CRIU_SPIKE_COMPILE_3_1
static int criu_spike_compile_3_1(void)
{
	struct page *page;

	page = follow_page(criu_spike_mm->mmap, criu_spike_args.anon_addr,
			   FOLL_GET);
	if (page)
		put_page(page);
	return 0;
}
#endif

static int criu_spike_run_page_probe(void)
{
	if (!strcmp(criu_spike_args.probe, "3.2"))
		return criu_spike_run_page_gup();
	if (!strcmp(criu_spike_args.probe, "3.3"))
		return criu_spike_run_page_access();
	if (!strcmp(criu_spike_args.probe, "3.4"))
		return criu_spike_run_page_invalid();
	if (!strcmp(criu_spike_args.probe, "3.5"))
		return criu_spike_run_page_guard();
	return -EOPNOTSUPP;
}
