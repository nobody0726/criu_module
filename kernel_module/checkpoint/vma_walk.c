/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sched/mm.h>

#include "criu_kernel.h"

static enum criu_vma_special special_kind(struct vm_area_struct *vma,
					  const char **name)
{
	const char *vma_name = NULL;

	if (vma->vm_ops && vma->vm_ops->name)
		vma_name = vma->vm_ops->name(vma);
	if (name)
		*name = vma_name;
	if (vma_name) {
		if (!strcmp(vma_name, "[vdso]"))
			return CRIU_VMA_SPECIAL_VDSO;
		if (!strcmp(vma_name, "[vvar]"))
			return CRIU_VMA_SPECIAL_VVAR;
	}
	if (vma->vm_flags & VM_HUGETLB)
		return CRIU_VMA_SPECIAL_HUGETLB;
	if (vma->vm_flags & VM_IO)
		return CRIU_VMA_SPECIAL_DEVICE;
	if (vma->vm_flags & VM_PFNMAP)
		return CRIU_VMA_SPECIAL_PFNMAP;
	if (vma->vm_flags & VM_MIXEDMAP)
		return CRIU_VMA_SPECIAL_MIXEDMAP;
	return CRIU_VMA_SPECIAL_NONE;
}

static enum criu_vma_class vma_class(struct vm_area_struct *vma)
{
	if (vma_is_anonymous(vma))
		return CRIU_VMA_ANON_PRIVATE;
	if ((vma->vm_flags & VM_SHARED) && vma->vm_file &&
	    (file_inode(vma->vm_file)->i_flags & S_PRIVATE))
		return CRIU_VMA_ANON_SHARED;
	if (vma->vm_file)
		return (vma->vm_flags & VM_MAYSHARE) ?
			CRIU_VMA_FILE_SHARED : CRIU_VMA_FILE_PRIVATE;
	return CRIU_VMA_UNSUPPORTED;
}

static void fill_vma(struct vm_area_struct *vma, struct criu_vma_info *out)
{
	char *buf;
	const char *name;
	char *path;

	memset(out, 0, sizeof(*out));
	out->start = vma->vm_start;
	out->end = vma->vm_end;
	out->pgoff = vma->vm_pgoff;
	out->vm_flags_raw = vma->vm_flags;
	out->prot = vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC);
	out->shared = !!(vma->vm_flags & VM_MAYSHARE);
	out->growsdown = !!(vma->vm_flags & VM_GROWSDOWN);
	out->dontdump = !!(vma->vm_flags & VM_DONTDUMP);
	out->locked = !!(vma->vm_flags & VM_LOCKED);
	out->class = vma_class(vma);
	out->special = special_kind(vma, &name);
	if (!out->prot && out->special == CRIU_VMA_SPECIAL_NONE)
		out->special = CRIU_VMA_SPECIAL_PROT_NONE;
	out->sample_status = CRIU_SAMPLE_SKIPPED;
	out->path_status = CRIU_PATH_OK;
	if (!vma->vm_file) {
		if (name) {
			strscpy(out->path, name, sizeof(out->path));
		} else if (vma->vm_mm && vma->vm_start <= vma->vm_mm->start_brk &&
			   vma->vm_end >= vma->vm_mm->brk) {
			strscpy(out->path, "[heap]", sizeof(out->path));
		} else if (vma->vm_mm && vma->vm_start <= vma->vm_mm->start_stack &&
			   vma->vm_end >= vma->vm_mm->start_stack) {
			strscpy(out->path, "[stack]", sizeof(out->path));
		} else {
			strscpy(out->path, "-", sizeof(out->path));
		}
		return;
	}
	out->dev = file_inode(vma->vm_file)->i_sb->s_dev;
	out->ino = file_inode(vma->vm_file)->i_ino;
	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		out->path_status = CRIU_PATH_ERROR;
		strscpy(out->path, "-", sizeof(out->path));
		return;
	}
	path = d_path(&vma->vm_file->f_path, buf, PAGE_SIZE);
	if (IS_ERR(path)) {
		out->path_status = CRIU_PATH_ERROR;
		strscpy(out->path, "-", sizeof(out->path));
	} else if (strscpy(out->path, path, sizeof(out->path)) < 0) {
		out->path_status = CRIU_PATH_TRUNCATED;
	}
	kfree(buf);
}

int criu_walk_vmas(struct task_struct *task, criu_vma_info_fn fn, void *arg)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int ret = 0;

	if (!task || !fn)
		return -EINVAL;
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;
	mmap_read_lock(mm);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		struct criu_vma_info info;

		fill_vma(vma, &info);
		ret = fn(&info, arg);
		if (ret)
			break;
	}
	mmap_read_unlock(mm);
	mmput(mm);
	return ret;
}

struct count_ctx {
	unsigned long count;
	unsigned long special;
	unsigned long unsupported;
};

static int count_vma(const struct criu_vma_info *info, void *arg)
{
	struct count_ctx *count = arg;

	count->count++;
	if (info->special != CRIU_VMA_SPECIAL_NONE)
		count->special++;
	if (info->class == CRIU_VMA_UNSUPPORTED)
		count->unsupported++;
	return 0;
}

int criu_collect_mm_info(struct task_struct *task, struct criu_mm_info *out)
{
	struct mm_struct *mm;
	struct count_ctx count = { 0 };

	if (!task || !out)
		return -EINVAL;
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;
	memset(out, 0, sizeof(*out));
	out->pid = task_pid_nr(task);
	out->tgid = task_tgid_nr(task);
	get_task_comm(out->comm, task);
	out->state = READ_ONCE(task->state);
	out->total_vm = mm->total_vm;
	out->start_code = mm->start_code;
	out->end_code = mm->end_code;
	out->start_data = mm->start_data;
	out->end_data = mm->end_data;
	out->start_brk = mm->start_brk;
	out->brk = mm->brk;
	out->start_stack = mm->start_stack;
	out->arg_start = mm->arg_start;
	out->arg_end = mm->arg_end;
	out->env_start = mm->env_start;
	out->env_end = mm->env_end;
	mmput(mm);
	if (criu_walk_vmas(task, count_vma, &count))
		return -ESRCH;
	out->vma_count = count.count;
	out->special_count = count.special;
	out->unsupported_count = count.unsupported;
	return 0;
}

struct fill_ctx {
	struct criu_vma_info *vmas;
	unsigned long capacity;
	unsigned long count;
};

static int sample_byte(struct mm_struct *mm, unsigned long address, u8 *value)
{
	struct page *page = NULL;
	void *mapped;
	long ret;

	ret = get_user_pages_remote(mm, address & PAGE_MASK, 1, 0, &page, NULL,
				    NULL);
	if (ret != 1)
		return -EFAULT;
	mapped = kmap(page);
	*value = *((u8 *)mapped + (address & ~PAGE_MASK));
	kunmap(page);
	put_page(page);
	return 0;
}

static int fill_vma_array(const struct criu_vma_info *info, void *arg)
{
	struct fill_ctx *fill = arg;

	if (fill->count == fill->capacity)
		return -E2BIG;
	fill->vmas[fill->count++] = *info;
	return 0;
}

int criu_snapshot_capture(struct task_struct *task, struct criu_snapshot *out,
			  bool samples)
{
	struct mm_struct *mm;
	struct fill_ctx fill;
	unsigned long capacity;
	int attempt, ret;

	if (!task || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;
	ret = criu_collect_mm_info(task, &out->mm);
	if (ret)
		goto out_mm;
	for (attempt = 0; attempt < 3; attempt++) {
		mmap_read_lock(mm);
		capacity = mm->map_count;
		mmap_read_unlock(mm);
		if (capacity > CRIU_SNAPSHOT_MAX / sizeof(*out->vmas)) {
			ret = -E2BIG;
			goto out_mm;
		}
		out->vmas = kcalloc(capacity ?: 1, sizeof(*out->vmas), GFP_KERNEL);
		if (!out->vmas) {
			ret = -ENOMEM;
			goto out_mm;
		}
		fill.vmas = out->vmas;
		fill.capacity = capacity;
		fill.count = 0;
		ret = criu_walk_vmas(task, fill_vma_array, &fill);
		if (ret != -E2BIG) {
			out->mm.vma_count = fill.count;
			ret = 0;
			break;
		}
		kfree(out->vmas);
		out->vmas = NULL;
	}
	if (ret)
		goto out_mm;
	if (samples) {
		unsigned long i;

		for (i = 0; i < out->mm.vma_count && i < 4096; i++) {
			if (out->vmas[i].special != CRIU_VMA_SPECIAL_NONE ||
			    out->vmas[i].dontdump || !out->vmas[i].prot)
				continue;
			ret = sample_byte(mm, out->vmas[i].start,
					  &out->vmas[i].sample);
			if (!ret) {
				out->vmas[i].sample_status = CRIU_SAMPLE_OK;
			} else {
				out->vmas[i].sample_status = CRIU_SAMPLE_UNAVAILABLE;
			}
		}
	}
	mmput(mm);
	return 0;
out_mm:
	mmput(mm);
	kfree(out->vmas);
	out->vmas = NULL;
	return ret;
}

void criu_snapshot_destroy(struct criu_snapshot *snapshot)
{
	if (snapshot)
		kfree(snapshot->vmas);
}
