#include "validator.h"

#include <limits.h>
#include <stdio.h>

static int page_aligned(uint64_t value)
{
	return (value & (B1_RESTORE_PAGE_SIZE - 1)) == 0;
}

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
	*out = a + b;
	return *out < a;
}

static enum b1_restore_status unsupported(struct b1_restore_image *image,
					  const char *message)
{
	b1_restore_set_diag(image, B1_RESTORE_UNSUPPORTED, message);
	return B1_RESTORE_UNSUPPORTED;
}

static enum b1_restore_status format(struct b1_restore_image *image,
				     const char *message)
{
	b1_restore_set_diag(image, B1_RESTORE_FORMAT, message);
	return B1_RESTORE_FORMAT;
}

static enum b1_restore_status io_error(struct b1_restore_image *image,
				       const char *message)
{
	b1_restore_set_diag(image, B1_RESTORE_IO, message);
	return B1_RESTORE_IO;
}

enum b1_restore_status b1_validate_supported(struct b1_restore_image *image)
{
	size_t i, j;

	if (!image->arch_aarch64)
		return unsupported(image, "only aarch64 images are supported");
	if (image->tasks != 1)
		return unsupported(image, "only a single task is supported");
	if (image->threads != 1)
		return unsupported(image, "threads are not supported in B1");
	if (image->children)
		return unsupported(image, "pstree children are not supported");
	if (image->shared_mm)
		return unsupported(image, "shared mm is not supported");
	if (image->namespaces)
		return unsupported(image, "namespace restore is not supported");
	if (image->shared_mappings)
		return unsupported(image, "shared mappings are not supported");
	if (image->vdso_reloc)
		return unsupported(image, "vDSO relocation is not supported");
	if (image->pac)
		return unsupported(image, "PAC state is not supported");
	if (image->sve)
		return unsupported(image, "SVE state is not supported");
	if (image->gcs)
		return unsupported(image, "GCS state is not supported");
	if (!image->have_core || !image->have_mm || !image->have_pagemap)
		return io_error(image, "missing required image");
	if (!image->target_pid)
		return format(image, "missing target pid");

	for (i = 0; i < image->vma_count; i++) {
		uint64_t end;

		if (!page_aligned(image->vmas[i].start) ||
		    !page_aligned(image->vmas[i].length) ||
		    image->vmas[i].length == 0)
			return format(image, "VMA range is not page aligned");
		if (add_overflow_u64(image->vmas[i].start, image->vmas[i].length, &end))
			return format(image, "VMA range overflow");
		if (image->vmas[i].shared)
			return unsupported(image, "shared VMA is not supported");
		if (image->vmas[i].kind == B1_VMA_FILE_PRIVATE) {
			if (image->vmas[i].dirty_file_private)
				return unsupported(image, "dirty file-private VMA is not supported");
			if (!image->vmas[i].file_stable)
				return io_error(image, "file-private backing file is unstable");
			if (image->vmas[i].file_size < image->vmas[i].length)
				return format(image, "backing file is shorter than VMA");
		}
		for (j = 0; j < i; j++) {
			uint64_t old_end = image->vmas[j].start + image->vmas[j].length;

			if (image->vmas[i].start < old_end && image->vmas[j].start < end)
				return format(image, "overlapping target VMAs");
		}
	}

	for (i = 0; i < image->page_run_count; i++) {
		uint64_t bytes;
		uint64_t end;

		if (!image->page_runs[i].pages)
			return format(image, "empty page run");
		if (image->page_runs[i].pages > UINT64_MAX / B1_RESTORE_PAGE_SIZE)
			return format(image, "page run byte overflow");
		bytes = image->page_runs[i].pages * B1_RESTORE_PAGE_SIZE;
		if (!page_aligned(image->page_runs[i].addr) ||
		    add_overflow_u64(image->page_runs[i].addr, bytes, &end))
			return format(image, "page run address overflow");
	}

	b1_restore_set_diag(image, B1_RESTORE_OK, "supported");
	return B1_RESTORE_OK;
}
