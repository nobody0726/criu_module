#include "criu_image_reader.h"

#include "criu_wire.h"
#include "image_reader.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMG_COMMON_MAGIC 0x54564319U
#define INVENTORY_MAGIC 0x58313116U
#define PSTREE_MAGIC 0x50273030U
#define CORE_MAGIC 0x55053847U
#define MM_MAGIC 0x57492820U
#define PAGEMAP_MAGIC 0x56084025U

#define VMA_AREA_STACK (1U << 1)
#define VMA_AREA_VDSO (1U << 3)
#define VMA_FILE_PRIVATE (1U << 6)
#define VMA_FILE_SHARED (1U << 7)
#define VMA_ANON_SHARED (1U << 8)
#define VMA_ANON_PRIVATE (1U << 9)
#define VMA_AREA_VVAR (1U << 12)
#define VMA_AREA_GUARD (1U << 16)
#define VMA_AREA_UPROBES (1U << 17)
#define VMA_AREA_SHSTK (1U << 15)

#define MAP_SHARED_FLAG 0x01U
#define PAGEMAP_HAS_BLOCKS 6U

struct b1_blob {
	uint8_t *data;
	size_t len;
};

struct b1_real_paths {
	char core[NAME_MAX + 1];
	char mm[NAME_MAX + 1];
	char pagemap[NAME_MAX + 1];
	char pages[NAME_MAX + 1];
	uint32_t pid;
};

static enum b1_restore_status real_format(struct b1_restore_image *image,
					   const char *message)
{
	b1_restore_set_diag(image, B1_RESTORE_FORMAT, message);
	return B1_RESTORE_FORMAT;
}

static enum b1_restore_status real_io(struct b1_restore_image *image,
				      const char *message)
{
	b1_restore_set_diag(image, B1_RESTORE_IO, message);
	return B1_RESTORE_IO;
}

static enum b1_restore_status real_unsupported(struct b1_restore_image *image,
					       const char *message)
{
	b1_restore_set_diag(image, B1_RESTORE_UNSUPPORTED, message);
	return B1_RESTORE_UNSUPPORTED;
}

static uint32_t read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static enum b1_restore_status read_blob(const char *path, struct b1_blob *blob,
					struct b1_restore_image *image)
{
	struct stat st;
	int fd;
	size_t off = 0;

	memset(blob, 0, sizeof(*blob));
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return real_io(image, "opening CRIU image");
	if (fstat(fd, &st) < 0 || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		close(fd);
		return real_io(image, "statting CRIU image");
	}
	blob->len = (size_t)st.st_size;
	blob->data = malloc(blob->len ? blob->len : 1);
	if (!blob->data) {
		close(fd);
		return real_io(image, "allocating CRIU image");
	}
	while (off < blob->len) {
		ssize_t got = read(fd, blob->data + off, blob->len - off);

		if (got <= 0) {
			free(blob->data);
			memset(blob, 0, sizeof(*blob));
			close(fd);
			return real_io(image, "reading CRIU image");
		}
		off += (size_t)got;
	}
	close(fd);
	return B1_RESTORE_OK;
}

static int framed_header(const struct b1_blob *blob, uint32_t magic,
			 int inventory, size_t *off)
{
	size_t header = inventory ? 4U : 8U;

	if (blob->len < header)
		return -1;
	if (inventory) {
		if (read_le32(blob->data) != magic)
			return -1;
	} else if (read_le32(blob->data) != IMG_COMMON_MAGIC ||
		   read_le32(blob->data + 4) != magic) {
		return -1;
	}
	*off = header;
	return 0;
}

static int next_record(const struct b1_blob *blob, size_t *off,
		       const uint8_t **payload, size_t *payload_len)
{
	uint32_t len;

	if (*off == blob->len)
		return 0;
	if (blob->len - *off < sizeof(uint32_t))
		return -1;
	len = read_le32(blob->data + *off);
	*off += sizeof(uint32_t);
	if (len > blob->len - *off)
		return -1;
	*payload = blob->data + *off;
	*payload_len = len;
	*off += len;
	return 1;
}

static int field_varint(const uint8_t *data, size_t len, uint32_t number,
			uint64_t *value)
{
	struct b1_pb_cursor cursor = { data, len, 0 };
	struct b1_pb_field field;
	int rc;

	while ((rc = b1_pb_next(&cursor, &field)) > 0)
		if (field.number == number)
			return b1_pb_read_u64(&field, value);
	return rc < 0 ? -1 : 1;
}

static enum b1_restore_status parse_inventory(const char *dir,
					      struct b1_restore_image *image)
{
	char path[PATH_MAX];
	struct b1_blob blob;
	const uint8_t *payload;
	size_t payload_len;
	size_t off;
	uint64_t version;
	enum b1_restore_status st;

	snprintf(path, sizeof(path), "%s/inventory.img", dir);
	st = read_blob(path, &blob, image);
	if (st != B1_RESTORE_OK)
		return st;
	if (framed_header(&blob, INVENTORY_MAGIC, 1, &off) ||
	    next_record(&blob, &off, &payload, &payload_len) != 1 ||
	    field_varint(payload, payload_len, 1, &version) ||
	    version == 0) {
		free(blob.data);
		return real_format(image, "invalid CRIU inventory framing");
	}
	free(blob.data);
	return B1_RESTORE_OK;
}

static enum b1_restore_status parse_pstree(const char *dir,
					   struct b1_restore_image *image)
{
	char path[PATH_MAX];
	struct b1_blob blob;
	const uint8_t *payload;
	size_t payload_len;
	size_t off;
	int records = 0;
	int rc;
	uint64_t pid = 0;
	enum b1_restore_status st;

	snprintf(path, sizeof(path), "%s/pstree.img", dir);
	st = read_blob(path, &blob, image);
	if (st != B1_RESTORE_OK)
		return st;
	if (framed_header(&blob, PSTREE_MAGIC, 0, &off)) {
		free(blob.data);
		return real_format(image, "invalid CRIU pstree header");
	}
	while ((rc = next_record(&blob, &off, &payload, &payload_len)) > 0) {
		uint64_t value;

		if (field_varint(payload, payload_len, 1, &value)) {
			free(blob.data);
			return real_format(image, "invalid CRIU pstree record");
		}
		if (!records)
			pid = value;
		records++;
	}
	free(blob.data);
	if (rc < 0 || records == 0 || pid > UINT32_MAX)
		return real_format(image, "invalid CRIU pstree contents");
	image->target_pid = (uint32_t)pid;
	image->tasks = records;
	image->children = records > 1;
	image->threads = 1;
	return B1_RESTORE_OK;
}

static enum b1_restore_status parse_regs(const uint8_t *data, size_t len,
					 struct b1_restore_image *image)
{
	struct b1_pb_cursor cursor = { data, len, 0 };
	struct b1_pb_field field;
	uint64_t values[31];
	size_t count;
	int rc;

	count = 0;
	while ((rc = b1_pb_next(&cursor, &field)) > 0) {
		if (field.number == 1) {
			if (field.wire_type == 2) {
				size_t packed_count;

				if (b1_pb_read_packed_u64(&field, values + count,
							  31 - count, &packed_count))
					return -1;
				count += packed_count;
			} else if (field.wire_type == 0) {
				if (count == 31 || b1_pb_read_u64(&field, &values[count]))
					return -1;
				count++;
			} else {
				return -1;
			}
		} else if (field.number == 2 && b1_pb_read_u64(&field, &image->sp)) {
			return -1;
		} else if (field.number == 3 && b1_pb_read_u64(&field, &image->pc)) {
			return -1;
		} else if (field.number == 4 && b1_pb_read_u64(&field, &image->pstate)) {
			return -1;
		}
	}
	if (count != 31)
		return -1;
	memcpy(image->regs, values, sizeof(values));
	return rc < 0 ? -1 : 0;
}

static enum b1_restore_status parse_fpsimd(const uint8_t *data, size_t len,
					   struct b1_restore_image *image)
{
	struct b1_pb_cursor cursor = { data, len, 0 };
	struct b1_pb_field field;
	uint64_t values[64];
	size_t count;
	int rc;

	count = 0;
	while ((rc = b1_pb_next(&cursor, &field)) > 0) {
		if (field.number == 1) {
			if (field.wire_type == 2) {
				size_t packed_count;

				if (b1_pb_read_packed_u64(&field, values + count,
							  64 - count, &packed_count))
					return -1;
				count += packed_count;
			} else if (field.wire_type == 0) {
				if (count == 64 || b1_pb_read_u64(&field, &values[count]))
					return -1;
				count++;
			} else {
				return -1;
			}
		} else if (field.number == 2 && b1_pb_read_u32(&field, &image->fpsr)) {
			return -1;
		} else if (field.number == 3 && b1_pb_read_u32(&field, &image->fpcr)) {
			return -1;
		}
	}
	if (count != 64)
		return -1;
	for (size_t i = 0; i < 32; i++) {
		memcpy(image->vregs[i], &values[i * 2], sizeof(uint64_t));
		memcpy(image->vregs[i] + sizeof(uint64_t),
		       &values[i * 2 + 1], sizeof(uint64_t));
	}
	return rc < 0 ? -1 : 0;
}

static enum b1_restore_status parse_thread_info(const uint8_t *data, size_t len,
						struct b1_restore_image *image)
{
	struct b1_pb_cursor cursor = { data, len, 0 };
	struct b1_pb_field field;
	int rc;

	while ((rc = b1_pb_next(&cursor, &field)) > 0) {
		struct b1_pb_cursor nested;

		if (field.number == 2 && b1_pb_read_u64(&field, &image->tls))
			return real_format(image, "invalid aarch64 TLS field");
		if (field.number == 3 || field.number == 4) {
			if (b1_pb_submessage(&field, &nested))
				return real_format(image, "invalid aarch64 core submessage");
			if (field.number == 3 && parse_regs(nested.data, nested.len, image))
				return real_format(image, "invalid aarch64 register set");
			if (field.number == 4 && parse_fpsimd(nested.data, nested.len, image))
				return real_format(image, "invalid aarch64 FPSIMD set");
		}
		if (field.number == 5 && field.wire_type == 2 && field.length != 0)
			image->pac = 1;
		if (field.number == 6 && field.wire_type == 2 && field.length != 0)
			image->gcs = 1;
	}
	return rc < 0 ? real_format(image, "invalid aarch64 thread info") :
		       B1_RESTORE_OK;
}

static enum b1_restore_status parse_core(const char *path,
					 struct b1_restore_image *image)
{
	struct b1_blob blob;
	const uint8_t *payload;
	size_t payload_len;
	size_t off;
	struct b1_pb_cursor cursor;
	struct b1_pb_field field;
	int rc;
	uint64_t mtype;
	enum b1_restore_status st;

	st = read_blob(path, &blob, image);
	if (st != B1_RESTORE_OK)
		return st;
	if (framed_header(&blob, CORE_MAGIC, 0, &off) ||
	    next_record(&blob, &off, &payload, &payload_len) != 1) {
		free(blob.data);
		return real_format(image, "invalid CRIU core framing");
	}
	cursor = (struct b1_pb_cursor){ payload, payload_len, 0 };
	mtype = 0;
	while ((rc = b1_pb_next(&cursor, &field)) > 0) {
		struct b1_pb_cursor nested;

		if (field.number == 1 && b1_pb_read_u64(&field, &mtype)) {
			free(blob.data);
			return real_format(image, "invalid CRIU core architecture");
		}
		if (field.number == 8) {
			if (b1_pb_submessage(&field, &nested) ||
			    parse_thread_info(nested.data, nested.len, image) != B1_RESTORE_OK) {
				free(blob.data);
				return B1_RESTORE_FORMAT;
			}
		}
		if (field.number == 5) {
			if (b1_pb_submessage(&field, &nested))
				break;
			for (;;) {
				struct b1_pb_field tf;
				int trc = b1_pb_next(&nested, &tf);

				if (trc <= 0)
					break;
				if (tf.number == 6 && b1_pb_read_u64(&tf, &image->sigmask)) {
					free(blob.data);
					return real_format(image, "invalid signal mask");
				}
			}
		}
	}
	free(blob.data);
	if (rc < 0)
		return real_format(image, "invalid CRIU core record");
	if (mtype != 3)
		return real_unsupported(image, "CRIU image is not aarch64");
	image->arch_aarch64 = 1;
	image->have_core = 1;
	return B1_RESTORE_OK;
}

static enum b1_restore_status append_vma(struct b1_restore_image *image,
					 uint64_t start, uint64_t end,
					 uint32_t flags, uint32_t status)
{
	struct b1_vma_record *new_vmas;
	struct b1_vma_record *vma;

	if (end <= start || image->vma_count >= B1_RESTORE_MAX_VMAS)
		return real_format(image, "invalid CRIU VMA range");
	new_vmas = realloc(image->vmas,
			   (image->vma_count + 1U) * sizeof(*image->vmas));
	if (!new_vmas)
		return real_io(image, "allocating CRIU VMA model");
	image->vmas = new_vmas;
	vma = &image->vmas[image->vma_count++];
	memset(vma, 0, sizeof(*vma));
	vma->start = start;
	vma->length = end - start;
	vma->shared = (flags & MAP_SHARED_FLAG) != 0 ||
		      (status & (VMA_FILE_SHARED | VMA_ANON_SHARED)) != 0;
	vma->dirty_file_private = 0;
	vma->file_stable = 1;
	vma->backing_fd = -1;
	vma->file_size = vma->length;
	if (status & VMA_AREA_STACK)
		vma->kind = B1_VMA_STACK;
	else if (status & VMA_AREA_VDSO)
		vma->kind = B1_VMA_VDSO;
	else if (status & VMA_FILE_PRIVATE)
		vma->kind = B1_VMA_FILE_PRIVATE;
	else if (status & VMA_ANON_PRIVATE)
		vma->kind = B1_VMA_ANON_PRIVATE;
	else if (status & (VMA_AREA_VVAR | VMA_AREA_GUARD | VMA_AREA_UPROBES |
			   VMA_AREA_SHSTK))
		return real_unsupported(image, "unsupported CRIU special VMA");
	else
		return real_unsupported(image, "unsupported CRIU VMA kind");
	return B1_RESTORE_OK;
}

static enum b1_restore_status parse_mm(const char *path,
				       struct b1_restore_image *image)
{
	struct b1_blob blob;
	const uint8_t *payload;
	size_t payload_len;
	size_t off;
	struct b1_pb_cursor cursor;
	struct b1_pb_field field;
	int rc;
	enum b1_restore_status st;

	st = read_blob(path, &blob, image);
	if (st != B1_RESTORE_OK)
		return st;
	if (framed_header(&blob, MM_MAGIC, 0, &off) ||
	    next_record(&blob, &off, &payload, &payload_len) != 1) {
		free(blob.data);
		return real_format(image, "invalid CRIU mm framing");
	}
	cursor = (struct b1_pb_cursor){ payload, payload_len, 0 };
	while ((rc = b1_pb_next(&cursor, &field)) > 0) {
		if (field.number == 14) {
			struct b1_pb_cursor vcursor;
			uint64_t start = 0, end = 0;
			uint32_t flags = 0, status = 0;
			int have_start = 0, have_end = 0;
			struct b1_pb_field vf;
			int vrc;

			if (b1_pb_submessage(&field, &vcursor)) {
				free(blob.data);
				return real_format(image, "invalid CRIU VMA message");
			}
			while ((vrc = b1_pb_next(&vcursor, &vf)) > 0) {
				if (vf.number == 1 && !b1_pb_read_u64(&vf, &start))
					have_start = 1;
				else if (vf.number == 2 && !b1_pb_read_u64(&vf, &end))
					have_end = 1;
				else if (vf.number == 6 && b1_pb_read_u32(&vf, &flags)) {
					free(blob.data);
					return real_format(image, "invalid CRIU VMA flags");
				} else if (vf.number == 7 && b1_pb_read_u32(&vf, &status)) {
					free(blob.data);
					return real_format(image, "invalid CRIU VMA status");
				}
			}
			if (vrc < 0 || !have_start || !have_end) {
				free(blob.data);
				return real_format(image, "incomplete CRIU VMA message");
			}
			st = append_vma(image, start, end, flags, status);
			if (st != B1_RESTORE_OK) {
				free(blob.data);
				return st;
			}
		}
	}
	free(blob.data);
	if (rc < 0)
		return real_format(image, "invalid CRIU mm record");
	image->have_mm = image->vma_count != 0;
	return image->have_mm ? B1_RESTORE_OK :
		real_format(image, "CRIU mm contains no VMAs");
}

static enum b1_restore_status append_page_run(struct b1_restore_image *image,
					      uint64_t addr, uint64_t pages,
					      uint64_t image_offset,
					      const char *pages_name)
{
	struct b1_page_run *new_runs;

	if (!pages || image->page_run_count >= B1_RESTORE_MAX_PAGE_RUNS)
		return real_format(image, "invalid CRIU page run");
	new_runs = realloc(image->page_runs,
			   (image->page_run_count + 1U) * sizeof(*image->page_runs));
	if (!new_runs)
		return real_io(image, "allocating CRIU page model");
	image->page_runs = new_runs;
	memset(&image->page_runs[image->page_run_count], 0,
	       sizeof(image->page_runs[image->page_run_count]));
	image->page_runs[image->page_run_count].addr = addr;
	image->page_runs[image->page_run_count].pages = pages;
	image->page_runs[image->page_run_count].image_offset = image_offset;
	snprintf(image->page_runs[image->page_run_count].image,
		 sizeof(image->page_runs[image->page_run_count].image), "%s",
		 pages_name);
	image->page_run_count++;
	return B1_RESTORE_OK;
}

static enum b1_restore_status parse_pagemap(const char *path,
					    const char *dir,
					    struct b1_restore_image *image)
{
	struct b1_blob blob;
	const uint8_t *payload;
	size_t payload_len;
	size_t off;
	uint64_t pages_id;
	uint64_t image_offset = 0;
	char pages_name[NAME_MAX + 1];
	enum b1_restore_status st;
	int rc;

	st = read_blob(path, &blob, image);
	if (st != B1_RESTORE_OK)
		return st;
	if (framed_header(&blob, PAGEMAP_MAGIC, 0, &off) ||
	    next_record(&blob, &off, &payload, &payload_len) != 1 ||
	    field_varint(payload, payload_len, 1, &pages_id) ||
	    pages_id > UINT32_MAX) {
		free(blob.data);
		return real_format(image, "invalid CRIU pagemap header");
	}
	snprintf(pages_name, sizeof(pages_name), "pages-%" PRIu64 ".img", pages_id);
	{
		char pages_path[PATH_MAX];
		struct stat stbuf;

		snprintf(pages_path, sizeof(pages_path), "%s/%s", dir, pages_name);
		if (stat(pages_path, &stbuf) < 0)
			{
				free(blob.data);
				return real_io(image, "missing CRIU pages image");
			}
	}
	while ((rc = next_record(&blob, &off, &payload, &payload_len)) > 0) {
		struct b1_pb_cursor cursor = { payload, payload_len, 0 };
		struct b1_pb_field field;
		uint64_t addr = 0, pages = 0, nr_pages = 0;
		int in_parent = 0;
		int have_addr = 0, have_pages = 0, compressed = 0;
		int frc;

		while ((frc = b1_pb_next(&cursor, &field)) > 0) {
			if (field.number == 1 && !b1_pb_read_u64(&field, &addr))
				have_addr = 1;
			else if (field.number == 2 && !b1_pb_read_u64(&field, &pages))
				have_pages = 1;
			else if (field.number == 3)
				in_parent = 1;
			else if (field.number == PAGEMAP_HAS_BLOCKS)
				compressed = 1;
			else if (field.number == 5 && b1_pb_read_u64(&field, &nr_pages)) {
				free(blob.data);
				return real_format(image, "invalid CRIU pagemap page count");
			}
		}
		if (frc < 0 || !have_addr || !have_pages) {
			free(blob.data);
			return real_format(image, "invalid CRIU pagemap record");
		}
		if (compressed) {
			free(blob.data);
			return real_unsupported(image, "compressed CRIU pages are not supported");
		}
		if (in_parent)
			continue;
		if (nr_pages)
			pages = nr_pages;
		if (pages > UINT64_MAX / B1_RESTORE_PAGE_SIZE ||
		    image_offset > UINT64_MAX - pages * B1_RESTORE_PAGE_SIZE) {
			free(blob.data);
			return real_format(image, "CRIU page payload offset overflow");
		}
		st = append_page_run(image, addr, pages, image_offset, pages_name);
		if (st != B1_RESTORE_OK) {
			free(blob.data);
			return st;
		}
		image_offset += pages * B1_RESTORE_PAGE_SIZE;
	}
	free(blob.data);
	if (rc < 0)
		return real_format(image, "invalid CRIU pagemap stream");
	image->have_pagemap = 1;
	return B1_RESTORE_OK;
}

static int find_named_image(const char *dir, const char *prefix,
			    struct b1_real_paths *paths)
{
	DIR *dp;
	struct dirent *entry;
	size_t prefix_len = strlen(prefix);

	dp = opendir(dir);
	if (!dp)
		return -1;
	while ((entry = readdir(dp)) != NULL) {
		const char *name = entry->d_name;
		const char *suffix;
		char *end = NULL;
		unsigned long pid;

		if (strncmp(name, prefix, prefix_len) != 0)
			continue;
		suffix = name + prefix_len;
		if (*suffix == '\0')
			continue;
		pid = strtoul(suffix, &end, 10);
		if (end == suffix || strcmp(end, ".img") != 0 || pid > UINT32_MAX)
			continue;
		if (!paths->pid)
			paths->pid = (uint32_t)pid;
		if (strcmp(prefix, "core-") == 0)
			snprintf(paths->core, sizeof(paths->core), "%s", name);
		else if (strcmp(prefix, "mm-") == 0)
			snprintf(paths->mm, sizeof(paths->mm), "%s", name);
		else if (strcmp(prefix, "pagemap-") == 0)
			snprintf(paths->pagemap, sizeof(paths->pagemap), "%s", name);
	}
	closedir(dp);
	return 0;
}

enum b1_restore_status b1_read_criu_images(const char *dir,
					   struct b1_restore_image *image)
{
	struct b1_real_paths paths;
	char path[PATH_MAX];
	enum b1_restore_status st;

	memset(&paths, 0, sizeof(paths));
	st = parse_inventory(dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	st = parse_pstree(dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	if (find_named_image(dir, "core-", &paths) ||
	    find_named_image(dir, "mm-", &paths) ||
	    find_named_image(dir, "pagemap-", &paths) ||
	    !paths.core[0] || !paths.mm[0] || !paths.pagemap[0])
		return real_io(image, "missing CRIU task image");
	snprintf(path, sizeof(path), "%s/%s", dir, paths.core);
	st = parse_core(path, image);
	if (st != B1_RESTORE_OK)
		return st;
	snprintf(path, sizeof(path), "%s/%s", dir, paths.mm);
	st = parse_mm(path, image);
	if (st != B1_RESTORE_OK)
		return st;
	snprintf(path, sizeof(path), "%s/%s", dir, paths.pagemap);
	st = parse_pagemap(path, dir, image);
	if (st != B1_RESTORE_OK)
		return st;
	image->shared_mm = 0;
	image->shared_mappings = 0;
	image->vdso_reloc = 0;
	image->namespaces = 0;
	b1_restore_set_diag(image, B1_RESTORE_OK, "real CRIU protobuf image loaded");
	return B1_RESTORE_OK;
}
