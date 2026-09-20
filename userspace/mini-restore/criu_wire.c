#include "criu_wire.h"

#include <limits.h>
#include <string.h>

static int read_varint(const uint8_t *data, size_t len, size_t *off,
		       uint64_t *out)
{
	uint64_t value = 0;
	unsigned int shift = 0;

	while (*off < len && shift < 64) {
		uint8_t byte = data[(*off)++];

		if (shift == 63 && byte > 1)
			return -1;
		value |= (uint64_t)(byte & 0x7fU) << shift;
		if (!(byte & 0x80U)) {
			*out = value;
			return 0;
		}
		shift += 7;
	}
	return -1;
}

static int skip_bytes(size_t len, size_t *off, size_t count)
{
	if (count > len - *off)
		return -1;
	*off += count;
	return 0;
}

int b1_pb_next(struct b1_pb_cursor *cursor, struct b1_pb_field *field)
{
	uint64_t tag;
	uint64_t length;
	size_t payload;

	memset(field, 0, sizeof(*field));
	if (cursor->off == cursor->len)
		return 0;
	if (read_varint(cursor->data, cursor->len, &cursor->off, &tag))
		return -1;
	if (tag < 8 || tag > UINT32_MAX)
		return -1;
	field->number = (uint32_t)(tag >> 3);
	field->wire_type = (uint32_t)(tag & 7U);
	switch (field->wire_type) {
	case 0:
		return read_varint(cursor->data, cursor->len, &cursor->off,
				  &field->varint) ? -1 : 1;
	case 1:
		if (skip_bytes(cursor->len, &cursor->off, 8))
			return -1;
		field->bytes = cursor->data + cursor->off - 8;
		field->length = 8;
		return 1;
	case 2:
		if (read_varint(cursor->data, cursor->len, &cursor->off, &length))
			return -1;
		if (length > SIZE_MAX || length > cursor->len - cursor->off)
			return -1;
		payload = cursor->off;
		cursor->off += (size_t)length;
		field->bytes = cursor->data + payload;
		field->length = (size_t)length;
		return 1;
	case 5:
		if (skip_bytes(cursor->len, &cursor->off, 4))
			return -1;
		field->bytes = cursor->data + cursor->off - 4;
		field->length = 4;
		return 1;
	default:
		return -1;
	}
}

int b1_pb_submessage(const struct b1_pb_field *field,
		     struct b1_pb_cursor *cursor)
{
	if (!field || field->wire_type != 2 || !cursor)
		return -1;
	cursor->data = field->bytes;
	cursor->len = field->length;
	cursor->off = 0;
	return 0;
}

int b1_pb_read_u32(const struct b1_pb_field *field, uint32_t *out)
{
	if (!field || field->wire_type != 0 || field->varint > UINT32_MAX)
		return -1;
	*out = (uint32_t)field->varint;
	return 0;
}

int b1_pb_read_u64(const struct b1_pb_field *field, uint64_t *out)
{
	if (!field || field->wire_type != 0)
		return -1;
	*out = field->varint;
	return 0;
}

int b1_pb_read_s64(const struct b1_pb_field *field, int64_t *out)
{
	uint64_t value;

	if (b1_pb_read_u64(field, &value))
		return -1;
	*out = (int64_t)((value >> 1) ^ (~(value & 1U) + 1U));
	return 0;
}

int b1_pb_read_packed_u64(const struct b1_pb_field *field,
			  uint64_t *out, size_t capacity, size_t *count)
{
	struct b1_pb_cursor cursor;
	size_t n = 0;

	if (!field || field->wire_type != 2)
		return -1;
	if (b1_pb_submessage(field, &cursor))
		return -1;
	while (cursor.off < cursor.len) {
		uint64_t value;

		if (read_varint(cursor.data, cursor.len, &cursor.off, &value))
			return -1;
		if (n == capacity)
			return -1;
		out[n++] = value;
	}
	*count = n;
	return 0;
}
