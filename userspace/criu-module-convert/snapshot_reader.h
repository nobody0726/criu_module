#ifndef CRIU_MODULE_SNAPSHOT_READER_H
#define CRIU_MODULE_SNAPSHOT_READER_H

#include <stddef.h>
#include <stdint.h>

enum snapshot_reader_status {
	SNAPSHOT_READER_OK = 0,
	SNAPSHOT_READER_UNSUPPORTED = 1,
	SNAPSHOT_READER_INCONSISTENT = 2,
	SNAPSHOT_READER_IO_ERROR = 3,
	SNAPSHOT_READER_FORMAT_ERROR = 4,
};

struct snapshot_document {
	uint8_t *data;
	size_t size;
	uint32_t record_count;
};

int snapshot_read_validate(const char *path, struct snapshot_document *doc);
void snapshot_document_free(struct snapshot_document *doc);

#endif
