#define _GNU_SOURCE

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

int image_writer_field_sint64(struct image_writer *w, unsigned field, int64_t value)
{
	uint64_t encoded;

	/* Use unsigned arithmetic so INT64_MIN never overflows. */
	encoded = ((uint64_t)value << 1) ^ (uint64_t)-(value < 0);
	return image_writer_field_varint(w, field, encoded);
}

int image_writer_field_bytes(struct image_writer *w, unsigned field,
				 const void *data, size_t len)
{
	if (!field || field > 0x1fffffff || len > UINT64_MAX ||
		image_writer_varint(w, ((uint64_t)field << 3) | 2) ||
		image_writer_varint(w, len) || reserve(w, len))
		return -1;
	if (len)
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

static int open_temp(const char *path, char **tmp_out)
{
	char *tmp;
	size_t n = strlen(path) + 5;
	int fd;

	if (n < strlen(path))
		return -1;
	tmp = malloc(n);
	if (!tmp)
		return -1;
	if (snprintf(tmp, n, "%s.tmp", path) < 0) {
		free(tmp);
		return -1;
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(tmp);
		return -1;
	}
	*tmp_out = tmp;
	return fd;
}

static int finish_temp(int fd, char *tmp, const char *path, int ok)
{
	int ret = -1;

	if (ok && fsync(fd) == 0 && close(fd) == 0 && rename(tmp, path) == 0)
		ret = 0;
	else
		close(fd);
	if (ret)
		unlink(tmp);
	free(tmp);
	return ret;
}

int image_writer_write_messages(const char *path, const void *prefix, size_t prefix_len,
					const struct image_writer *messages, size_t count)
{
	char *tmp = NULL;
	int fd;
	size_t i;
	int ok = 0;

	if (!path || (!prefix && prefix_len) || (!messages && count))
		return -1;
	fd = open_temp(path, &tmp);
	if (fd < 0)
		return -1;
	if (write_all(fd, prefix, prefix_len))
		return finish_temp(fd, tmp, path, 0);
	for (i = 0; i < count; i++) {
		uint8_t length[4];

		if (messages[i].len > UINT32_MAX)
			return finish_temp(fd, tmp, path, 0);
		put_le32(length, (uint32_t)messages[i].len);
		if (write_all(fd, length, sizeof(length)) ||
			write_all(fd, messages[i].data, messages[i].len))
			return finish_temp(fd, tmp, path, 0);
	}
	ok = 1;
	return finish_temp(fd, tmp, path, ok);
}

int image_writer_write_file(const char *path, const void *prefix, size_t prefix_len,
				   const struct image_writer *message)
{
	if (!message)
		return -1;
	return image_writer_write_messages(path, prefix, prefix_len, message, 1);
}

int image_writer_write_raw_file(const char *path, const void *data, size_t len)
{
	char *tmp = NULL;
	int fd;

	if (!path || (!data && len))
		return -1;
	fd = open_temp(path, &tmp);
	if (fd < 0)
		return -1;
	if (write_all(fd, data, len))
		return finish_temp(fd, tmp, path, 0);
	return finish_temp(fd, tmp, path, 1);
}
