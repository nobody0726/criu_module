#ifndef CRIU_MODULE_CRIU_MODEL_H
#define CRIU_MODULE_CRIU_MODEL_H

#include "snapshot_reader.h"

struct criu_convert_options {
	const char *output_dir;
};

int criu_emit_images(const struct snapshot_document *doc,
			 const struct criu_convert_options *options);

#endif
