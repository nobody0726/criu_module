#include "snapshot_reader.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	struct snapshot_document doc;
	int ret;

	if (argc != 2)
		return SNAPSHOT_READER_FORMAT_ERROR;
	ret = snapshot_read_validate(argv[1], &doc);
	snapshot_document_free(&doc);
	return ret;
}
