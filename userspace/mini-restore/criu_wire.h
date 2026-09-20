#ifndef B1_CRIU_WIRE_H
#define B1_CRIU_WIRE_H

#include <stddef.h>
#include <stdint.h>

struct b1_pb_cursor {
	const uint8_t *data;
	size_t len;
	size_t off;
};

struct b1_pb_field {
	uint32_t number;
	uint32_t wire_type;
	uint64_t varint;
	const uint8_t *bytes;
	size_t length;
};

int b1_pb_next(struct b1_pb_cursor *cursor, struct b1_pb_field *field);
int b1_pb_submessage(const struct b1_pb_field *field,
		     struct b1_pb_cursor *cursor);
int b1_pb_read_u32(const struct b1_pb_field *field, uint32_t *out);
int b1_pb_read_u64(const struct b1_pb_field *field, uint64_t *out);
int b1_pb_read_s64(const struct b1_pb_field *field, int64_t *out);
int b1_pb_read_packed_u64(const struct b1_pb_field *field,
			  uint64_t *out, size_t capacity, size_t *count);

#endif
