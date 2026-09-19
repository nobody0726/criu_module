/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include "dump_mm.h"
#include "page_scan.h"
#include "criu_kernel.h"
#include "dump_shmem.h"

/* Policy rejects VM_SHARED, VM_HUGETLB, VM_IO, VM_PFNMAP and VM_MIXEDMAP
 * through the normalized class/special values produced by vma_walk.c. */

static int semantic_prot(const struct criu_vma_info *vma)
{
	int prot = 0;
	if (vma->prot & VM_READ)
		prot |= 1;
	if (vma->prot & VM_WRITE)
		prot |= 2;
	if (vma->prot & VM_EXEC)
		prot |= 4;
	return prot;
}

static int classify_policy(const struct criu_vma_info *vma,
				  enum criu_vma_dump_policy *policy)
{
	if (vma->special == CRIU_VMA_SPECIAL_HUGETLB ||
	    vma->special == CRIU_VMA_SPECIAL_DEVICE ||
	    vma->special == CRIU_VMA_SPECIAL_PFNMAP ||
	    vma->special == CRIU_VMA_SPECIAL_MIXEDMAP ||
	    vma->special == CRIU_VMA_SPECIAL_UNKNOWN)
		return -EOPNOTSUPP;
	if (vma->special == CRIU_VMA_SPECIAL_VDSO) {
		*policy = CRIU_VMA_DUMP_VDSO;
		return 0;
	}
	if (vma->special == CRIU_VMA_SPECIAL_VVAR) {
		*policy = CRIU_VMA_DUMP_VVAR;
		return 0;
	}
	if (vma->special == CRIU_VMA_SPECIAL_PROT_NONE) {
		*policy = CRIU_VMA_DUMP_SKIP_GUARD;
		return 0;
	}
	if (vma->dontdump) {
		*policy = CRIU_VMA_DUMP_SKIP_DONTDUMP;
		return 0;
	}
	if (vma->class == CRIU_VMA_ANON_SHARED) {
		*policy = CRIU_VMA_DUMP_SHMEM;
		return 0;
	}
	if (vma->class == CRIU_VMA_FILE_SHARED ||
	    vma->class == CRIU_VMA_UNSUPPORTED)
		return -EOPNOTSUPP;
	if (vma->class == CRIU_VMA_ANON_PRIVATE)
		*policy = CRIU_VMA_DUMP_PRIVATE_ANON;
	else if (vma->class == CRIU_VMA_FILE_PRIVATE)
		*policy = CRIU_VMA_DUMP_PRIVATE_FILE;
	else
		return -EOPNOTSUPP;
	return 0;
}

struct dump_mm_ctx {
	struct criu_snapshot_writer *writer;
	struct criu_dump_shared_ctx *shared_ctx;
};

static int dump_one_vma(const struct criu_vma_info *vma, void *arg)
{
	struct dump_mm_ctx *ctx = arg;
	struct criu_vma_record rec;
	struct criu_snapshot_vma_shared_record shared_rec;
	enum criu_vma_dump_policy policy;
	u32 shmid = 0;
	bool is_new = false;
	int ret;

	ret = classify_policy(vma, &policy);
	if (ret) {
		pr_info("criu_dump_mm: reject vma=%lx-%lx class=%u special=%u flags=%lx path=%s ret=%d\n",
			vma->start, vma->end, vma->class, vma->special,
			vma->vm_flags_raw, vma->path[0] ? vma->path : "-", ret);
		return ret;
	}
	memset(&rec, 0, sizeof(rec));
	memset(&shared_rec, 0, sizeof(shared_rec));
	if (policy == CRIU_VMA_DUMP_SHMEM) {
		struct criu_snapshot_shmem_object_record object;
		u64 shmem_size;

		if (!ctx->shared_ctx || !vma->inode)
			return -EOPNOTSUPP;
		shmem_size = i_size_read(vma->inode);
		if (!shmem_size || shmem_size < vma->end - vma->start)
			shmem_size = vma->end - vma->start;
		ret = criu_shmem_register(ctx->shared_ctx, vma->inode,
					  shmem_size, &shmid, &is_new);
		if (ret)
			return ret;
		if (is_new) {
			memset(&object, 0, sizeof(object));
			object.version = cpu_to_le32(CRIU_SNAPSHOT_SHMEM_OBJECT_VERSION);
			object.shmid = cpu_to_le32(shmid);
			object.size = cpu_to_le64(shmem_size);
			object.dev = cpu_to_le64(vma->dev);
			object.ino = cpu_to_le64(vma->ino);
			ret = criu_snapshot_writer_global_record(
				ctx->writer, CRIU_SNAPSHOT_REC_SHMEM_OBJECT,
				&object, sizeof(object));
			if (ret)
				return ret;
			ret = criu_shmem_dump_content(ctx->shared_ctx, ctx->writer,
						      vma->inode, shmid,
						      shmem_size);
			if (ret)
				return ret;
		}
	}
	rec.start = vma->start;
	rec.end = vma->end;
	rec.pgoff = vma->pgoff;
	rec.prot = semantic_prot(vma);
	rec.class = vma->class;
	rec.special = vma->special;
	rec.dump_policy = policy;
	rec.flags = (vma->shared ? 1 : 0) | (vma->growsdown ? 2 : 0) |
			(vma->dontdump ? 4 : 0) | (vma->locked ? 8 : 0);
	rec.dev = vma->dev;
	rec.ino = vma->ino;
	strscpy(rec.path, vma->path, sizeof(rec.path));
	if (policy == CRIU_VMA_DUMP_SHMEM) {
		shared_rec.base = rec;
		shared_rec.shmid = cpu_to_le32(shmid);
		return criu_snapshot_writer_record(ctx->writer,
						   CRIU_SNAPSHOT_REC_VMA, 0,
						   &shared_rec,
						   sizeof(shared_rec));
	}
	return criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_VMA,
					   0, &rec, sizeof(rec));
}

static int criu_dump_mm_common(struct task_struct *task,
			       struct criu_snapshot_writer *writer,
			       struct criu_dump_shared_ctx *shared_ctx)
{
	struct criu_snapshot snapshot;
	struct criu_mm_record rec;
	struct dump_mm_ctx ctx = {
		.writer = writer,
		.shared_ctx = shared_ctx,
	};
	unsigned long i;
	int ret;

	if (!task || !writer)
		return -EINVAL;
	/*
	 * Capture the VMA descriptions while holding mmap_lock, then write the
	 * snapshot records after the lock is released. kernel_write() may acquire
	 * filesystem locks that can fault user pages and take mmap_lock itself.
	 */
	ret = criu_snapshot_capture(task, &snapshot, false);
	if (ret)
		return ret;
	memset(&rec, 0, sizeof(rec));
	rec.pid = snapshot.mm.pid;
	rec.tgid = snapshot.mm.tgid;
	rec.total_vm = snapshot.mm.total_vm;
	rec.start_code = snapshot.mm.start_code;
	rec.end_code = snapshot.mm.end_code;
	rec.start_data = snapshot.mm.start_data;
	rec.end_data = snapshot.mm.end_data;
	rec.start_brk = snapshot.mm.start_brk;
	rec.brk = snapshot.mm.brk;
	rec.start_stack = snapshot.mm.start_stack;
	rec.arg_start = snapshot.mm.arg_start;
	rec.arg_end = snapshot.mm.arg_end;
	rec.env_start = snapshot.mm.env_start;
	rec.env_end = snapshot.mm.env_end;
	rec.vma_count = snapshot.mm.vma_count;
	ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_MM, 0,
					  &rec, sizeof(rec));
	if (!ret) {
		for (i = 0; i < snapshot.mm.vma_count; i++) {
			ret = dump_one_vma(&snapshot.vmas[i], &ctx);
			if (ret)
				break;
		}
	}
	criu_snapshot_destroy(&snapshot);
	if (ret)
		return ret == -EOPNOTSUPP ? -EOPNOTSUPP : ret;
	/* Scan only resident pages and emit inline PAGE_RUN records. */
	return criu_dump_pages(task, writer);
}

int criu_dump_mm(struct task_struct *task,
		 struct criu_snapshot_writer *writer)
{
	return criu_dump_mm_common(task, writer, NULL);
}

int criu_dump_mm_process(const struct criu_freeze_process_view *view,
			 struct criu_snapshot_writer *writer,
			 struct criu_dump_shared_ctx *shared_ctx)
{
	if (!view)
		return -EINVAL;
	return criu_dump_mm_common(view->leader, writer, shared_ctx);
}
