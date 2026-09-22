#ifndef B1_CRIU_IMAGE_READER_H
#define B1_CRIU_IMAGE_READER_H

#include "restore.h"

enum b1_restore_status b1_read_criu_images(const char *dir,
					   struct b1_restore_image *image);
enum b1_restore_status b1_read_criu_images_for_pid(
	const char *dir, uint32_t pid, struct b1_restore_image *image);

#endif
