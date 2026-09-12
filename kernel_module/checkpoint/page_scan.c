/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "page_scan.h"
#include "criu_kernel.h"

/* Linux 5.10.29 has no FOLL_NOFAULT; NOWAIT is the exported non-prefetch
 * equivalent for this module's frozen, resident-page probe. */
#ifndef FOLL_NOFAULT
#define FOLL_NOFAULT FOLL_NOWAIT
#endif

/*
 * The scanner deliberately uses FOLL_NOFAULT.  An absent page is a hole in
 * the snapshot, not a reason to fault data into the target address space.
 */
static int get_resident_page(struct mm_struct *mm, unsigned long addr,
				struct page **out)
{
	long ret;
	struct page *page = NULL;

	ret = get_user_pages_remote(mm, addr & PAGE_MASK, 1,
					FOLL_GET | FOLL_NOFAULT, &page, NULL, NULL);
	if (ret == 1) {
		*out = page;
		return 1;
	}
	if (ret == -EBUSY || ret == -EHWPOISON)
		return -EOPNOTSUPP; /* swap/special entry requiring preservation */
	/* FOLL_NOFAULT reports holes and non-resident pages as an absent page. */
	return 0;
}

static bool page_is_zero(struct page *page)
{
	return page && is_zero_pfn(page_to_pfn(page));
}

static bool page_is_file_backed(const struct vm_area_struct *vma,
					struct page *page)
{
	/* A private file page which is still file-backed must come from the file. */
	return vma->vm_file && !(vma->vm_flags & VM_SHARED) &&
			page && !PageAnon(page);
}

static int emit_page(struct criu_snapshot_writer *writer,
			     unsigned long addr, struct page *page, bool vdso)
{
	struct criu_page_run_record *run;
	void *mapped;
	unsigned int payload_len = sizeof(*run) + PAGE_SIZE;
	int ret;

	run = kmalloc(payload_len, GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	memset(run, 0, sizeof(*run));
	run->start = addr;
	run->nr_pages = 1;
	run->page_size = PAGE_SIZE;
	run->flags = CRIU_PAGE_RUN_PRESENT |
			(vdso ? CRIU_PAGE_RUN_VDSO : 0);
	run->payload_bytes = PAGE_SIZE;
	mapped = kmap(page);
	memcpy((char *)run + sizeof(*run), mapped, PAGE_SIZE);
	kunmap(page);
	ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_PAGE_RUN,
					 0, run, payload_len);
	kfree(run);
	return ret;
}

static int scan_vma(struct mm_struct *mm, struct vm_area_struct *vma,
			    struct criu_snapshot_writer *writer)
{
	unsigned long addr;
	bool vdso = false;

	if (vma->vm_ops && vma->vm_ops->name) {
		const char *name = vma->vm_ops->name(vma);
		vdso = name && !strcmp(name, "[vdso]");
	}
	/* vvar and guard/PROT_NONE mappings have no readable payload. */
	if ((vma->vm_ops && vma->vm_ops->name &&
		 !strcmp(vma->vm_ops->name(vma), "[vvar]")) ||
	    !(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) ||
	    (vma->vm_flags & VM_DONTDUMP))
		return 0;

	for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
		struct page *page = NULL;
		int state = get_resident_page(mm, addr, &page);
		int ret;

		if (state < 0)
			return state;
		if (!state)
			continue; /* absent: do not fault it in */
		if (page_is_zero(page)) {
			put_page(page);
			continue;
		}
		if (page_is_file_backed(vma, page)) {
			put_page(page); /* file-backed private page is recreated by mmap */
			continue;
		}
		ret = emit_page(writer, addr, page, vdso);
		put_page(page);
		if (ret)
			return ret;
	}
	return 0;
}

int criu_dump_pages(struct task_struct *task,
			struct criu_snapshot_writer *writer)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int ret = 0;

	if (!task || !writer)
		return -EINVAL;
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;
	mmap_read_lock(mm);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		ret = scan_vma(mm, vma, writer);
		if (ret)
			break;
	}
	mmap_read_unlock(mm);
	mmput(mm);
	return ret;
}
