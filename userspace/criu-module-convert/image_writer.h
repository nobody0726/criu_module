#ifndef CRIU_MODULE_IMAGE_WRITER_H
#define CRIU_MODULE_IMAGE_WRITER_H

#include <stddef.h>
#include <stdint.h>

struct image_writer {
	uint8_t *data;
	size_t len;
	size_t cap;
};

void image_writer_init(struct image_writer *w);
void image_writer_free(struct image_writer *w);
int image_writer_varint(struct image_writer *w, uint64_t value);
int image_writer_field_varint(struct image_writer *w, unsigned field, uint64_t value);
int image_writer_field_bytes(struct image_writer *w, unsigned field,
				 const void *data, size_t len);
int image_writer_write_file(const char *path, const void *prefix, size_t prefix_len,
				   const struct image_writer *message);

#endif
