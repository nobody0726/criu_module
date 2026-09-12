#include "image_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

static int reserve(struct image_writer *w, size_t add)
{
	size_t need, cap;
	uint8_t *p;
	if (add > SIZE_MAX - w->len)
		return -1;
	need = w->len + add;
	if (need <= w->cap)
		return 0;
	cap = w->cap ? w->cap : 128;
	while (cap < need) {
		if (cap > SIZE_MAX / 2)
			return -1;
		cap *= 2;
	}
	p = realloc(w->data, cap);
	if (!p)
		return -1;
	w->data = p;
	w->cap = cap;
	return 0;
}

void image_writer_init(struct image_writer *w) { memset(w, 0, sizeof(*w)); }
void image_writer_free(struct image_writer *w) { free(w->data); memset(w, 0, sizeof(*w)); }

int image_writer_varint(struct image_writer *w, uint64_t value)
{
	while (value >= 0x80) {
		if (reserve(w, 1)) return -1;
		w->data[w->len++] = (uint8_t)(value | 0x80);
		value >>= 7;
	}
	if (reserve(w, 1)) return -1;
	w->data[w->len++] = (uint8_t)value;
	return 0;
}

int image_writer_field_varint(struct image_writer *w, unsigned field, uint64_t value)
{
	if (!field || field > 0x1fffffff || image_writer_varint(w, ((uint64_t)field << 3) | 0))
		return -1;
	return image_writer_varint(w, value);
}

int image_writer_field_bytes(struct image_writer *w, unsigned field,
				 const void *data, size_t len)
{
	if (!field || field > 0x1fffffff || len > UINT64_MAX ||
		image_writer_varint(w, ((uint64_t)field << 3) | 2) ||
		image_writer_varint(w, len) || reserve(w, len))
		return -1;
	memcpy(w->data + w->len, data, len);
	w->len += len;
	return 0;
}

static int write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n; len -= (size_t)n;
	}
	return 0;
}

static void put_le32(uint8_t out[4], uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

int image_writer_write_file(const char *path, const void *prefix, size_t prefix_len,
				   const struct image_writer *message)
{
	char *tmp;
	int fd, ret = -1;
	size_t n = strlen(path) + 5;
	tmp = malloc(n);
	if (!tmp) return -1;
	if (snprintf(tmp, n, "%s.tmp", path) < 0) goto out;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) goto out;
	uint8_t length[4];
	put_le32(length, (uint32_t)message->len);
	if (write_all(fd, prefix, prefix_len) == 0 && message->len <= UINT32_MAX &&
		write_all(fd, length, sizeof(length)) == 0 &&
		write_all(fd, message->data, message->len) == 0) {
		if (fsync(fd) == 0 && close(fd) == 0 && rename(tmp, path) == 0) ret = 0;
	} else {
		close(fd);
	}
	if (ret) unlink(tmp);
out:
	free(tmp);
	return ret;
}
