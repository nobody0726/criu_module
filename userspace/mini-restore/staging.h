#ifndef B1_STAGING_H
#define B1_STAGING_H

#include <stddef.h>

#include "criu_restore_abi.h"
#include "restore.h"

struct b1_staging_plan {
	void *arena;
	size_t arena_len;
	size_t vma_count;
	struct criu_restore_vma_v1 *restore_vmas;
};

void b1_staging_plan_init(struct b1_staging_plan *plan);
void b1_staging_plan_free(struct b1_staging_plan *plan);
enum b1_restore_status b1_stage_image(const struct b1_restore_image *image,
				      const char *image_dir,
				      struct b1_staging_plan *plan);
enum b1_restore_status b1_staging_copy(struct b1_staging_plan *plan,
				       uint64_t staging_addr,
				       const void *data, size_t length);

#endif
