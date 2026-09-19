/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

#include "../../include/criu_snapshot.h"
#include "dump_shmem.h"

#define CRIU_SHMEM_RUN_PAGES 16U

int criu_shmem_register(struct criu_dump_shared_ctx *ctx,
			struct inode *inode, u64 size,
			u32 *shmid, bool *is_new)
{
	if (!ctx || !ctx->shmem_ids || !inode || !shmid)
		return -EINVAL;
	*shmid = criu_objmap_get(ctx->shmem_ids, inode, is_new);
	return *shmid ? 0 : -ENOMEM;
}

static int emit_run(struct criu_snapshot_writer *writer, u32 shmid,
		    u64 page_index, u32 nr_pages, const u8 *data)
{
	struct criu_snapshot_shmem_page_run_record *run;
	size_t data_len;
	size_t length;
	int ret;

	if (!nr_pages || nr_pages > CRIU_SHMEM_RUN_PAGES)
		return -EINVAL;
	data_len = (size_t)nr_pages * PAGE_SIZE;
	length = sizeof(*run) + data_len;
	run = kmalloc(length, GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	memset(run, 0, sizeof(*run));
	run->version = cpu_to_le32(CRIU_SNAPSHOT_SHMEM_PAGE_RUN_VERSION);
	run->shmid = cpu_to_le32(shmid);
	run->page_index = cpu_to_le64(page_index);
	run->nr_pages = cpu_to_le32(nr_pages);
	run->data_len = cpu_to_le32(data_len);
	memcpy((u8 *)run + sizeof(*run), data, data_len);
	ret = criu_snapshot_writer_global_record(writer,
						 CRIU_SNAPSHOT_REC_SHMEM_PAGE_RUN,
						 run, length);
	kfree(run);
	return ret;
}

int criu_shmem_dump_content(struct criu_dump_shared_ctx *ctx,
			    struct criu_snapshot_writer *writer,
			    struct inode *inode, u32 shmid, u64 size)
{
	struct address_space *mapping;
	u8 *data;
	u64 nr_pages;
	u64 index;
	int ret = 0;

	if (!ctx || !writer || !inode || !shmid || !size)
		return -EINVAL;
	if (!inode->i_mapping)
		return -EOPNOTSUPP;
	if (size > (u64)ULONG_MAX * PAGE_SIZE)
		return -E2BIG;
	mapping = inode->i_mapping;
	nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	data = kmalloc((size_t)CRIU_SHMEM_RUN_PAGES * PAGE_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/*
	 * The target is frozen. find_get_page() observes only pages already in
	 * the shmem page cache and never faults a missing page into the mapping.
	 * Missing pages remain implicit zero holes in the restore stream.
	 */
	for (index = 0; index < nr_pages;) {
		u32 run_pages = 0;

		while (run_pages < CRIU_SHMEM_RUN_PAGES &&
		       index + run_pages < nr_pages) {
			struct page *page;
			u64 page_index = index + run_pages;
			void *mapped;

			page = find_get_page(mapping, page_index);
			if (!page)
				break;
			mapped = kmap(page);
			memcpy(data + (size_t)run_pages * PAGE_SIZE,
			       mapped, PAGE_SIZE);
			kunmap(page);
			put_page(page);
			run_pages++;
		}
		if (run_pages) {
			ret = emit_run(writer, shmid, index, run_pages, data);
			if (ret)
				break;
			index += run_pages;
		} else {
			index++;
		}
	}
	kfree(data);
	return ret;
}
