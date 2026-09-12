/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/slab.h>

#include "dump_mm.h"
#include "page_scan.h"
#include "criu_kernel.h"

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
	if (vma->class == CRIU_VMA_ANON_SHARED ||
	    vma->class == CRIU_VMA_FILE_SHARED ||
	    vma->class == CRIU_VMA_UNSUPPORTED)
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
};

static int dump_one_vma(const struct criu_vma_info *vma, void *arg)
{
	struct dump_mm_ctx *ctx = arg;
	struct criu_vma_record rec;
	enum criu_vma_dump_policy policy;
	int ret;

	ret = classify_policy(vma, &policy);
	if (ret)
		return ret;
	memset(&rec, 0, sizeof(rec));
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
	return criu_snapshot_writer_record(ctx->writer, CRIU_SNAPSHOT_REC_VMA,
					   0, &rec, sizeof(rec));
}

int criu_dump_mm(struct task_struct *task,
		 struct criu_snapshot_writer *writer)
{
	struct criu_mm_info mm;
	struct criu_mm_record rec;
	struct dump_mm_ctx ctx = { .writer = writer };
	int ret;

	if (!task || !writer)
		return -EINVAL;
	ret = criu_collect_mm_info(task, &mm);
	if (ret)
		return ret;
	memset(&rec, 0, sizeof(rec));
	rec.pid = mm.pid;
	rec.tgid = mm.tgid;
	rec.total_vm = mm.total_vm;
	rec.start_code = mm.start_code;
	rec.end_code = mm.end_code;
	rec.start_data = mm.start_data;
	rec.end_data = mm.end_data;
	rec.start_brk = mm.start_brk;
	rec.brk = mm.brk;
	rec.start_stack = mm.start_stack;
	rec.arg_start = mm.arg_start;
	rec.arg_end = mm.arg_end;
	rec.env_start = mm.env_start;
	rec.env_end = mm.env_end;
	rec.vma_count = mm.vma_count;
	ret = criu_snapshot_writer_record(writer, CRIU_SNAPSHOT_REC_MM, 0,
					  &rec, sizeof(rec));
	if (ret)
		return ret;
	ret = criu_walk_vmas(task, dump_one_vma, &ctx);
	if (ret)
		return ret == -EOPNOTSUPP ? -EOPNOTSUPP : ret;
	/* Scan only resident pages and emit inline PAGE_RUN records. */
	return criu_dump_pages(task, writer);
}
