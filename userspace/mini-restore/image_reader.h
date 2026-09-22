#ifndef B1_IMAGE_READER_H
#define B1_IMAGE_READER_H

#include "restore.h"

#include <sys/types.h>

void b1_restore_image_init(struct b1_restore_image *image);
void b1_restore_image_free(struct b1_restore_image *image);
enum b1_restore_status b1_read_images(const char *dir, struct b1_restore_image *image);
enum b1_restore_status b1_read_images_for_pid(const char *dir, pid_t pid,
					      struct b1_restore_image *image);

#endif
