#ifndef B1_FILES_H
#define B1_FILES_H

#include <stdint.h>
#include <sys/types.h>

#include "restore.h"

struct b1_file_spec {
	const char *path;
	uint64_t dev;
	uint64_t ino;
	uint64_t min_size;
	int fd;
	int flags;
	uint64_t offset;
};

enum b1_restore_status b1_prepare_backing_file(const struct b1_file_spec *spec,
					       int *out_fd,
					       struct b1_restore_image *diag);
enum b1_restore_status b1_prepare_stdio(const struct b1_file_spec specs[3],
					int out_fds[3],
					struct b1_restore_image *diag);

#endif
