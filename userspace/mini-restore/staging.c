#include "staging.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static int b1_ranges_overlap(uint64_t a_start, uint64_t a_len,
			     uint64_t b_start, uint64_t b_len)
{
	uint64_t a_end = a_start + a_len;
	uint64_t b_end = b_start + b_len;

	return a_start < b_end && b_start < a_end;
}

static size_t b1_host_page_size(void)
{
	long value = sysconf(_SC_PAGESIZE);

	if (value <= 0)
		return (size_t)B1_RESTORE_PAGE_SIZE;
	return (size_t)value;
}

static size_t b1_round_up_size(size_t value, size_t align)
{
	return (value + align - 1U) & ~(align - 1U);
}

void b1_staging_plan_init(struct b1_staging_plan *plan)
{
	memset(plan, 0, sizeof(*plan));
}

void b1_staging_plan_free(struct b1_staging_plan *plan)
{
	if (!plan)
		return;
	if (plan->arena && plan->arena != MAP_FAILED)
		munmap(plan->arena, plan->arena_len);
	free(plan->restore_vmas);
	b1_staging_plan_init(plan);
}

static enum b1_restore_status staging_format(struct b1_restore_image *diag,
					     const char *message)
{
	b1_restore_set_diag(diag, B1_RESTORE_FORMAT, message);
	return B1_RESTORE_FORMAT;
}

static enum b1_restore_status staging_io(struct b1_restore_image *diag,
					 const char *message)
{
	b1_restore_set_diag(diag, B1_RESTORE_IO, message);
	return B1_RESTORE_IO;
}

static uint32_t b1_restore_kind(enum b1_vma_kind kind)
{
	switch (kind) {
	case B1_VMA_ANON_PRIVATE:
		return CRIU_RESTORE_VMA_ANON_PRIVATE;
	case B1_VMA_FILE_PRIVATE:
		return CRIU_RESTORE_VMA_FILE_PRIVATE;
	case B1_VMA_STACK:
		return CRIU_RESTORE_VMA_STACK;
	case B1_VMA_VDSO:
		return CRIU_RESTORE_VMA_VDSO;
	}
	return 0;
}

static enum b1_restore_status b1_fill_restore_vmas(const struct b1_restore_image *image,
						    struct b1_staging_plan *plan,
						    struct b1_restore_image *diag)
{
	uint64_t cursor = 0;
	size_t host_page = b1_host_page_size();
	size_t i, j;

	plan->restore_vmas = calloc(image->vma_count, sizeof(*plan->restore_vmas));
	if (!plan->restore_vmas)
		return staging_io(diag, "allocating restore VMA plan");
	plan->vma_count = image->vma_count;

	for (i = 0; i < image->vma_count; i++) {
		uint64_t staging_start = (uint64_t)(uintptr_t)plan->arena + cursor;

		for (j = 0; j < image->vma_count; j++)
			if (b1_ranges_overlap(staging_start, image->vmas[i].length,
					      image->vmas[j].start, image->vmas[j].length))
				return staging_format(diag, "staging overlaps target VMA");
		plan->restore_vmas[i].staging_start = staging_start;
		plan->restore_vmas[i].target_start = image->vmas[i].start;
		plan->restore_vmas[i].length = image->vmas[i].length;
		plan->restore_vmas[i].prot = PROT_READ | PROT_WRITE;
		plan->restore_vmas[i].map_flags = MAP_PRIVATE;
		plan->restore_vmas[i].kind = b1_restore_kind(image->vmas[i].kind);
		if (image->vmas[i].kind == B1_VMA_STACK)
			plan->restore_vmas[i].flags |= CRIU_RESTORE_VMA_F_GROWSDOWN;
		if (image->vmas[i].kind == B1_VMA_FILE_PRIVATE)
			plan->restore_vmas[i].flags |= CRIU_RESTORE_VMA_F_FILE_CLEAN;
		plan->restore_vmas[i].flags |= CRIU_RESTORE_VMA_F_STAGING_WRITABLE;
		cursor += b1_round_up_size((size_t)image->vmas[i].length, host_page);
	}
	return B1_RESTORE_OK;
}

static void *b1_staging_addr_for_page(const struct b1_restore_image *image,
				      const struct b1_staging_plan *plan,
				      uint64_t addr)
{
	size_t i;

	for (i = 0; i < image->vma_count; i++) {
		uint64_t start = image->vmas[i].start;
		uint64_t end = start + image->vmas[i].length;

		if (addr >= start && addr < end)
			return (void *)(uintptr_t)(plan->restore_vmas[i].staging_start +
						   (addr - start));
	}
	return NULL;
}

static enum b1_restore_status b1_stage_page_runs(const struct b1_restore_image *image,
						 const char *image_dir,
						 const struct b1_staging_plan *plan,
						 struct b1_restore_image *diag)
{
	size_t i;

	for (i = 0; i < image->page_run_count; i++) {
		const struct b1_page_run *run = &image->page_runs[i];
		uint64_t page_index;
		char path[512];
		int fd;

		snprintf(path, sizeof(path), "%s/%s", image_dir, run->image);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			return staging_io(diag, "opening page image");
		for (page_index = 0; page_index < run->pages; page_index++) {
			uint64_t page_offset = page_index * B1_RESTORE_PAGE_SIZE;
			void *dst = b1_staging_addr_for_page(image, plan,
							     run->addr + page_offset);
			ssize_t got;

			if (!dst) {
				close(fd);
				return staging_format(diag, "page run outside VMA");
			}
			if (run->image_offset > (uint64_t)INT64_MAX - page_offset) {
				close(fd);
				return staging_format(diag, "page image offset overflow");
			}
			got = pread(fd, dst, B1_RESTORE_PAGE_SIZE,
				    (off_t)(run->image_offset + page_offset));
			if (got != (ssize_t)B1_RESTORE_PAGE_SIZE) {
				close(fd);
				return staging_io(diag, "reading page image");
			}
		}
		close(fd);
	}
	return B1_RESTORE_OK;
}

enum b1_restore_status b1_stage_image(const struct b1_restore_image *image,
				      const char *image_dir,
				      struct b1_staging_plan *plan)
{
	struct b1_restore_image *diag = (struct b1_restore_image *)image;
	size_t host_page = b1_host_page_size();
	size_t i;
	enum b1_restore_status st;

	b1_staging_plan_init(plan);
	for (i = 0; i < image->vma_count; i++) {
		if (image->vmas[i].dirty_file_private)
			return staging_format(diag, "dirty file-private VMA cannot be staged");
		if (image->vmas[i].length > SIZE_MAX)
			return staging_format(diag, "staging VMA length overflow");
		if (b1_round_up_size((size_t)image->vmas[i].length, host_page) >
		    SIZE_MAX - plan->arena_len)
			return staging_format(diag, "staging arena size overflow");
		plan->arena_len += b1_round_up_size((size_t)image->vmas[i].length,
						    host_page);
	}
	if (!plan->arena_len)
		return staging_format(diag, "empty staging arena");

	plan->arena = mmap(NULL, plan->arena_len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (plan->arena == MAP_FAILED)
		return staging_io(diag, "allocating staging arena");

	st = b1_fill_restore_vmas(image, plan, diag);
	if (st != B1_RESTORE_OK)
		return st;

	for (i = 0; i < image->vma_count; i++) {
		if (image->vmas[i].kind == B1_VMA_FILE_PRIVATE &&
		    image->vmas[i].backing_fd >= 0) {
			void *addr = (void *)(uintptr_t)plan->restore_vmas[i].staging_start;
			void *mapped = mmap(addr, image->vmas[i].length,
					    PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_FIXED,
					    image->vmas[i].backing_fd, 0);
			if (mapped != addr)
				return staging_io(diag, "mapping clean file-private VMA");
		}
	}

	st = b1_stage_page_runs(image, image_dir, plan, diag);
	if (st != B1_RESTORE_OK)
		return st;

	for (i = 0; i < image->vma_count; i++) {
		void *addr = (void *)(uintptr_t)plan->restore_vmas[i].staging_start;

		if (mprotect(addr, image->vmas[i].length, PROT_READ) < 0)
			return staging_io(diag, "protecting staging VMA");
	}
	return B1_RESTORE_OK;
}
