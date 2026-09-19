#include "image_reader.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kv_file {
	char **keys;
	char **values;
	size_t count;
};

const char *b1_restore_status_name(enum b1_restore_status status)
{
	switch (status) {
	case B1_RESTORE_OK:
		return "OK";
	case B1_RESTORE_FORMAT:
		return "FORMAT";
	case B1_RESTORE_UNSUPPORTED:
		return "UNSUPPORTED";
	case B1_RESTORE_IO:
		return "IO";
	}
	return "UNKNOWN";
}

void b1_restore_set_diag(struct b1_restore_image *image,
			 enum b1_restore_status status, const char *message)
{
	if (!image)
		return;
	snprintf(image->diagnostic, sizeof(image->diagnostic), "%s: %s",
		 b1_restore_status_name(status), message ? message : "");
}

void b1_restore_image_init(struct b1_restore_image *image)
{
	memset(image, 0, sizeof(*image));
}

void b1_restore_image_free(struct b1_restore_image *image)
{
	if (!image)
		return;
	free(image->vmas);
	free(image->page_runs);
	b1_restore_image_init(image);
}

static char *xstrdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *copy = malloc(len);

	if (copy)
		memcpy(copy, s, len);
	return copy;
}

static void kv_free(struct kv_file *kv)
{
	size_t i;

	for (i = 0; i < kv->count; i++) {
		free(kv->keys[i]);
		free(kv->values[i]);
	}
	free(kv->keys);
	free(kv->values);
	memset(kv, 0, sizeof(*kv));
}

static enum b1_restore_status kv_load(const char *path, struct kv_file *kv)
{
	FILE *f = fopen(path, "r");
	char line[512];

	memset(kv, 0, sizeof(*kv));
	if (!f)
		return B1_RESTORE_IO;

	while (fgets(line, sizeof(line), f)) {
		char *eq;
		char *nl;
		char **new_keys;
		char **new_values;

		nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		eq = strchr(line, '=');
		if (!eq) {
			fclose(f);
			kv_free(kv);
			return B1_RESTORE_FORMAT;
		}
		*eq = '\0';
		new_keys = realloc(kv->keys, sizeof(*kv->keys) * (kv->count + 1));
		if (!new_keys) {
			fclose(f);
			kv_free(kv);
			return B1_RESTORE_IO;
		}
		kv->keys = new_keys;
		new_values = realloc(kv->values, sizeof(*kv->values) * (kv->count + 1));
		if (!new_values) {
			fclose(f);
			kv_free(kv);
			return B1_RESTORE_IO;
		}
		kv->values = new_values;
		kv->keys[kv->count] = xstrdup(line);
		kv->values[kv->count] = xstrdup(eq + 1);
		if (!kv->keys[kv->count] || !kv->values[kv->count]) {
			fclose(f);
			kv_free(kv);
			return B1_RESTORE_IO;
		}
		kv->count++;
	}
	fclose(f);
	return B1_RESTORE_OK;
}

static const char *kv_get(const struct kv_file *kv, const char *key)
{
	size_t i;

	for (i = 0; i < kv->count; i++)
		if (strcmp(kv->keys[i], key) == 0)
			return kv->values[i];
	return NULL;
}

static int parse_u64(const char *value, uint64_t *out)
{
	char *end = NULL;
	unsigned long long parsed;

	if (!value || !*value)
		return -1;
	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno || !end || *end)
		return -1;
	*out = (uint64_t)parsed;
	return 0;
}

static int parse_int(const char *value, int *out)
{
	uint64_t parsed;

	if (parse_u64(value, &parsed) || parsed > INT32_MAX)
		return -1;
	*out = (int)parsed;
	return 0;
}

static enum b1_restore_status read_inventory(const char *dir,
					     struct b1_restore_image *image)
{
	char path[512];
	struct kv_file kv;
	enum b1_restore_status st;
	const char *arch;

	snprintf(path, sizeof(path), "%s/inventory.img", dir);
	st = kv_load(path, &kv);
	if (st != B1_RESTORE_OK) {
		b1_restore_set_diag(image, st, "missing or malformed inventory.img");
		return st;
	}
	arch = kv_get(&kv, "arch");
	image->arch_aarch64 = arch && strcmp(arch, "aarch64") == 0;
	if (parse_int(kv_get(&kv, "tasks"), &image->tasks) ||
	    parse_int(kv_get(&kv, "threads"), &image->threads) ||
	    parse_int(kv_get(&kv, "children"), &image->children) ||
	    parse_int(kv_get(&kv, "namespaces"), &image->namespaces) ||
	    parse_int(kv_get(&kv, "shared_mm"), &image->shared_mm) ||
	    parse_int(kv_get(&kv, "shared_mappings"), &image->shared_mappings) ||
	    parse_int(kv_get(&kv, "vdso_reloc"), &image->vdso_reloc) ||
	    parse_int(kv_get(&kv, "pac"), &image->pac) ||
	    parse_int(kv_get(&kv, "sve"), &image->sve) ||
	    parse_int(kv_get(&kv, "gcs"), &image->gcs)) {
		kv_free(&kv);
		b1_restore_set_diag(image, B1_RESTORE_FORMAT, "bad inventory fields");
		return B1_RESTORE_FORMAT;
	}
	kv_free(&kv);
	return B1_RESTORE_OK;
}

static enum b1_restore_status read_core(const char *dir,
					struct b1_restore_image *image)
{
	char path[512];
	struct kv_file kv;
	enum b1_restore_status st;
	uint64_t tmp;

	snprintf(path, sizeof(path), "%s/core.img", dir);
	st = kv_load(path, &kv);
	if (st != B1_RESTORE_OK) {
		b1_restore_set_diag(image, st, "missing or malformed core.img");
		return st;
	}
	if (parse_u64(kv_get(&kv, "pid"), &tmp) || tmp > UINT32_MAX ||
	    parse_u64(kv_get(&kv, "tls"), &image->tls) ||
	    parse_u64(kv_get(&kv, "sigmask"), &image->sigmask) ||
	    !kv_get(&kv, "gpregs") || !kv_get(&kv, "fpsimd")) {
		kv_free(&kv);
		b1_restore_set_diag(image, B1_RESTORE_FORMAT, "bad core fields");
		return B1_RESTORE_FORMAT;
	}
	image->target_pid = (uint32_t)tmp;
	image->have_core = 1;
	kv_free(&kv);
	return B1_RESTORE_OK;
}

static enum b1_restore_status parse_vma_kind(const char *value,
					     enum b1_vma_kind *kind)
{
	if (!value)
		return B1_RESTORE_FORMAT;
	if (strcmp(value, "anon-private") == 0)
		*kind = B1_VMA_ANON_PRIVATE;
	else if (strcmp(value, "file-private") == 0)
		*kind = B1_VMA_FILE_PRIVATE;
	else if (strcmp(value, "stack") == 0)
		*kind = B1_VMA_STACK;
	else if (strcmp(value, "vdso") == 0)
		*kind = B1_VMA_VDSO;
	else
		return B1_RESTORE_UNSUPPORTED;
	return B1_RESTORE_OK;
}

static enum b1_restore_status read_mm(const char *dir,
				      struct b1_restore_image *image)
{
	char path[512];
	struct kv_file kv;
	enum b1_restore_status st;
	uint64_t count;
	size_t i;

	snprintf(path, sizeof(path), "%s/mm.img", dir);
	st = kv_load(path, &kv);
	if (st != B1_RESTORE_OK) {
		b1_restore_set_diag(image, st, "missing or malformed mm.img");
		return st;
	}
	if (parse_u64(kv_get(&kv, "vma_count"), &count) ||
	    count == 0 || count > B1_RESTORE_MAX_VMAS) {
		kv_free(&kv);
		b1_restore_set_diag(image, B1_RESTORE_FORMAT, "bad vma_count");
		return B1_RESTORE_FORMAT;
	}
	image->vmas = calloc((size_t)count, sizeof(*image->vmas));
	if (!image->vmas) {
		kv_free(&kv);
		b1_restore_set_diag(image, B1_RESTORE_IO, "allocating VMA model");
		return B1_RESTORE_IO;
	}
	image->vma_count = (size_t)count;
	for (i = 0; i < image->vma_count; i++) {
		char key[64];
		enum b1_restore_status kind_st;

		snprintf(key, sizeof(key), "vma%zu.start", i);
		if (parse_u64(kv_get(&kv, key), &image->vmas[i].start))
			goto format;
		snprintf(key, sizeof(key), "vma%zu.length", i);
		if (parse_u64(kv_get(&kv, key), &image->vmas[i].length))
			goto format;
		snprintf(key, sizeof(key), "vma%zu.kind", i);
		kind_st = parse_vma_kind(kv_get(&kv, key), &image->vmas[i].kind);
		if (kind_st != B1_RESTORE_OK) {
			kv_free(&kv);
			b1_restore_set_diag(image, kind_st, "unsupported VMA kind");
			return kind_st;
		}
		snprintf(key, sizeof(key), "vma%zu.shared", i);
		if (parse_int(kv_get(&kv, key), &image->vmas[i].shared))
			goto format;
		snprintf(key, sizeof(key), "vma%zu.dirty_file_private", i);
		if (parse_int(kv_get(&kv, key), &image->vmas[i].dirty_file_private))
			goto format;
		snprintf(key, sizeof(key), "vma%zu.file_stable", i);
		if (parse_int(kv_get(&kv, key), &image->vmas[i].file_stable))
			goto format;
		snprintf(key, sizeof(key), "vma%zu.file_size", i);
		if (parse_u64(kv_get(&kv, key), &image->vmas[i].file_size))
			goto format;
	}
	image->have_mm = 1;
	kv_free(&kv);
	return B1_RESTORE_OK;
format:
	kv_free(&kv);
	b1_restore_set_diag(image, B1_RESTORE_FORMAT, "bad VMA field");
	return B1_RESTORE_FORMAT;
}

static enum b1_restore_status read_pagemap(const char *dir,
					   struct b1_restore_image *image)
{
	char path[512];
	struct kv_file kv;
	enum b1_restore_status st;
	uint64_t count;
	size_t i;

	snprintf(path, sizeof(path), "%s/pagemap.img", dir);
	st = kv_load(path, &kv);
	if (st != B1_RESTORE_OK) {
		b1_restore_set_diag(image, st, "missing or malformed pagemap.img");
		return st;
	}
	if (parse_u64(kv_get(&kv, "runs"), &count) || count > B1_RESTORE_MAX_PAGE_RUNS) {
		kv_free(&kv);
		b1_restore_set_diag(image, B1_RESTORE_FORMAT, "bad page run count");
		return B1_RESTORE_FORMAT;
	}
	image->page_runs = calloc((size_t)count, sizeof(*image->page_runs));
	if (!image->page_runs) {
		kv_free(&kv);
		b1_restore_set_diag(image, B1_RESTORE_IO, "allocating page model");
		return B1_RESTORE_IO;
	}
	image->page_run_count = (size_t)count;
	for (i = 0; i < image->page_run_count; i++) {
		char key[64];
		const char *value;

		snprintf(key, sizeof(key), "run%zu.addr", i);
		if (parse_u64(kv_get(&kv, key), &image->page_runs[i].addr))
			goto format;
		snprintf(key, sizeof(key), "run%zu.pages", i);
		if (parse_u64(kv_get(&kv, key), &image->page_runs[i].pages))
			goto format;
		snprintf(key, sizeof(key), "run%zu.image", i);
		value = kv_get(&kv, key);
		if (!value || strlen(value) >= sizeof(image->page_runs[i].image))
			goto format;
		strcpy(image->page_runs[i].image, value);
	}
	image->have_pagemap = 1;
	kv_free(&kv);
	return B1_RESTORE_OK;
format:
	kv_free(&kv);
	b1_restore_set_diag(image, B1_RESTORE_FORMAT, "bad pagemap field");
	return B1_RESTORE_FORMAT;
}

enum b1_restore_status b1_read_images(const char *dir, struct b1_restore_image *image)
{
	enum b1_restore_status st;

	st = read_inventory(dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	st = read_core(dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	st = read_mm(dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	st = read_pagemap(dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	b1_restore_set_diag(image, B1_RESTORE_OK, "supported image model loaded");
	return B1_RESTORE_OK;
}
