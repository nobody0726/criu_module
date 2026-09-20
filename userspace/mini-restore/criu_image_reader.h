#ifndef B1_CRIU_IMAGE_READER_H
#define B1_CRIU_IMAGE_READER_H

#include "restore.h"

enum b1_restore_status b1_read_criu_images(const char *dir,
					   struct b1_restore_image *image);

#endif
