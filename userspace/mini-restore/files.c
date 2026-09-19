#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static enum b1_restore_status file_io(struct b1_restore_image *diag,
				      const char *message)
{
	b1_restore_set_diag(diag, B1_RESTORE_IO, message);
	return B1_RESTORE_IO;
}

enum b1_restore_status b1_prepare_backing_file(const struct b1_file_spec *spec,
					       int *out_fd,
					       struct b1_restore_image *diag)
{
	struct stat st;
	int fd;
	int flags;

	if (!spec || !spec->path || !out_fd)
		return file_io(diag, "bad file preparation request");

	fd = open(spec->path, spec->flags ? spec->flags : O_RDONLY);
	if (fd < 0)
		return file_io(diag, "opening backing file");
	if (fstat(fd, &st) < 0) {
		close(fd);
		return file_io(diag, "stat backing file");
	}
	if ((uint64_t)st.st_dev != spec->dev || (uint64_t)st.st_ino != spec->ino) {
		close(fd);
		return file_io(diag, "backing file identity changed");
	}
	if ((uint64_t)st.st_size < spec->min_size) {
		close(fd);
		b1_restore_set_diag(diag, B1_RESTORE_FORMAT, "backing file too short");
		return B1_RESTORE_FORMAT;
	}
	if (lseek(fd, (off_t)spec->offset, SEEK_SET) < 0) {
		close(fd);
		return file_io(diag, "seeking backing file");
	}
	flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	*out_fd = fd;
	return B1_RESTORE_OK;
}

enum b1_restore_status b1_prepare_stdio(const struct b1_file_spec specs[3],
					int out_fds[3],
					struct b1_restore_image *diag)
{
	int i;

	for (i = 0; i < 3; i++)
		out_fds[i] = -1;
	for (i = 0; i < 3; i++) {
		enum b1_restore_status st;

		if (specs[i].fd != i) {
			b1_restore_set_diag(diag, B1_RESTORE_FORMAT,
					    "stdio descriptor order mismatch");
			return B1_RESTORE_FORMAT;
		}
		st = b1_prepare_backing_file(&specs[i], &out_fds[i], diag);
		if (st != B1_RESTORE_OK) {
			int j;

			for (j = 0; j < i; j++)
				if (out_fds[j] >= 0)
					close(out_fds[j]);
			return st;
		}
	}
	return B1_RESTORE_OK;
}
