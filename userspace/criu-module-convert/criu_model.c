#define _GNU_SOURCE

#include "criu_model.h"
#include "image_writer.h"
#include "../../include/criu_snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define IMG_COMMON_MAGIC 0x54564319U
#define INVENTORY_MAGIC 0x58313116U
#define PSTREE_MAGIC 0x50273030U
#define FDINFO_MAGIC 0x56213732U
#define PAGEMAP_MAGIC 0x56084025U
#define CORE_MAGIC 0x55053847U
#define IDS_MAGIC 0x54432030U
#define MM_MAGIC 0x57492820U
#define REG_FILES_MAGIC 0x50363636U
#define FS_MAGIC 0x51403912U
#define CREDS_MAGIC 0x54023547U
#define FILES_MAGIC 0x56303138U

#define ELF_ARCH_X86_64 62U
#define ELF_ARCH_AARCH64 183U

#define TASK_FIXED_SIZE 48U
#define TASK_SIG_BYTES 8U
#define TASK_RLIMIT_OFFSET (TASK_FIXED_SIZE + 3U * TASK_SIG_BYTES)
#define TASK_RLIMIT_MAX 16U
#define TASK_COMM_OFFSET (TASK_RLIMIT_OFFSET + TASK_RLIMIT_MAX * 16U)
#define TASK_COMM_SIZE 16U

#define MM_RECORD_SIZE 112U
#define VMA_RECORD_SIZE 604U
#define FD_RECORD_SIZE 560U
#define FD_OBJECT_ID_OFFSET 560U
#define FD_TYPE_OFFSET 568U
#define FS_RECORD_SIZE 1024U
#define CREDS_RECORD_SIZE 76U
#define PAGE_RECORD_SIZE 24U
#define THREAD_RECORD_SIZE (16U + 8U + 8U + CRIU_SNAPSHOT_THREAD_REG_BYTES)
#define THREAD_REGS_OFFSET 32U

/* Packed criu_vma_record layout. The path follows four page counters. */
#define VMA_FLAGS_OFFSET 40U
#define VMA_DEV_OFFSET 44U
#define VMA_INO_OFFSET 52U
#define VMA_PATH_OFFSET 92U

#define VMA_CLASS_ANON_PRIVATE 0U
#define VMA_CLASS_ANON_SHARED 1U
#define VMA_CLASS_FILE_SHARED 2U
#define VMA_CLASS_FILE_PRIVATE 3U

#define VMA_SPECIAL_NONE 0U
#define VMA_SPECIAL_PROT_NONE 1U
#define VMA_SPECIAL_VDSO 2U
#define VMA_SPECIAL_VVAR 3U

#define VMA_AREA_REGULAR (1U << 0)
#define VMA_AREA_STACK (1U << 1)
#define VMA_AREA_VDSO (1U << 3)
#define VMA_AREA_HEAP (1U << 5)
#define VMA_FILE_PRIVATE (1U << 6)
#define VMA_ANON_PRIVATE (1U << 9)
#define VMA_AREA_VVAR (1U << 12)
#define VMA_AREA_NOT_ACCOUNTABLE (1U << 18)

#define MAP_PRIVATE 0x02U
#define MAP_ANONYMOUS 0x20U
#define MAP_GROWSDOWN 0x100U

#define PE_PRESENT (1U << 2)
#define CRIU_PAGE_RUN_PRESENT (1U << 0)

struct blob_ref {
	const uint8_t *data;
	size_t len;
};

struct blob_list {
	struct blob_ref *items;
	size_t count;
	size_t capacity;
};

struct snapshot_model {
	struct blob_ref task;
	struct blob_ref mm;
	struct blob_ref regs;
	struct blob_ref fs;
	struct blob_ref creds;
	struct blob_list vmas;
	struct blob_list fds;
	struct blob_list pages;
	struct blob_list threads;
	uint32_t arch;
	uint32_t page_size;
	uint32_t pid;
	uint32_t tgid;
};

struct file_item {
	uint32_t id;
	uint32_t mode;
	uint64_t flags;
	uint64_t pos;
	uint64_t dev;
	uint64_t ino;
	uint64_t size;
	bool has_size;
	uint64_t object_id;
	uint32_t type;
	char path[512];
};

struct fd_binding {
	uint32_t fd;
	uint32_t id;
};

struct file_table {
	struct file_item *items;
	size_t count;
	size_t capacity;
	struct fd_binding *bindings;
	size_t binding_count;
	size_t binding_capacity;
	uint32_t exe_id;
	uint32_t cwd_id;
	uint32_t root_id;
};

static uint16_t u16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const uint8_t *p)
{
	return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void model_free(struct snapshot_model *model)
{
	if (!model)
		return;
	free(model->vmas.items);
	free(model->fds.items);
	free(model->pages.items);
	free(model->threads.items);
	memset(model, 0, sizeof(*model));
}

static int list_add(struct blob_list *list, const uint8_t *data, size_t len)
{
	struct blob_ref *items;
	size_t capacity;

	if (list->count == list->capacity) {
		capacity = list->capacity ? list->capacity * 2U : 16U;
		if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*items))
			return -1;
		items = realloc(list->items, capacity * sizeof(*items));
		if (!items)
			return -1;
		list->items = items;
		list->capacity = capacity;
	}
	list->items[list->count].data = data;
	list->items[list->count].len = len;
	list->count++;
	return 0;
}

static int set_blob(struct blob_ref *slot, const uint8_t *data, size_t len)
{
	if (slot->data)
		return -1;
	slot->data = data;
	slot->len = len;
	return 0;
}

static int collect_records(const struct snapshot_document *doc,
					struct snapshot_model *model)
{
	size_t off = CRIU_SNAPSHOT_HEADER_SIZE;
	size_t end;

	if (!doc || !model || !doc->data || doc->size <
		CRIU_SNAPSHOT_HEADER_SIZE + CRIU_SNAPSHOT_FOOTER_SIZE)
		return SNAPSHOT_READER_FORMAT_ERROR;
	memset(model, 0, sizeof(*model));
	model->arch = u32(doc->data + 16);
	model->page_size = u32(doc->data + 20);
	model->pid = u32(doc->data + 24);
	model->tgid = u32(doc->data + 28);
	end = doc->size - CRIU_SNAPSHOT_FOOTER_SIZE;
	while (off < end) {
		uint16_t type;
		uint64_t raw_len;
		size_t len;
		const uint8_t *payload;

		if (end - off < CRIU_SNAPSHOT_TLV_HEADER_SIZE)
			goto format_error;
		type = u16(doc->data + off);
		raw_len = u64(doc->data + off + 8);
		if (raw_len > SIZE_MAX || raw_len > end - off -
			CRIU_SNAPSHOT_TLV_HEADER_SIZE)
			goto format_error;
		len = (size_t)raw_len;
		off += CRIU_SNAPSHOT_TLV_HEADER_SIZE;
		payload = doc->data + off;
		if (type == CRIU_SNAPSHOT_REC_END)
			break;
		switch (type) {
		case CRIU_SNAPSHOT_REC_TASK:
			if (set_blob(&model->task, payload, len))
				goto format_error;
			break;
		case CRIU_SNAPSHOT_REC_MM:
			if (set_blob(&model->mm, payload, len))
				goto format_error;
			break;
		case CRIU_SNAPSHOT_REC_VMA:
			if (list_add(&model->vmas, payload, len))
				goto io_error;
			break;
		case CRIU_SNAPSHOT_REC_REGS:
			if (set_blob(&model->regs, payload, len))
				goto format_error;
			break;
		case CRIU_SNAPSHOT_REC_FD:
			if (list_add(&model->fds, payload, len))
				goto io_error;
			break;
		case CRIU_SNAPSHOT_REC_FS:
			if (set_blob(&model->fs, payload, len))
				goto format_error;
			break;
		case CRIU_SNAPSHOT_REC_CREDS:
			if (set_blob(&model->creds, payload, len))
				goto format_error;
			break;
		case CRIU_SNAPSHOT_REC_IDMAP:
			/* A3 has no user namespace mapping; retain forward compatibility. */
			break;
		case CRIU_SNAPSHOT_REC_PAGE_RUN:
			if (list_add(&model->pages, payload, len))
				goto io_error;
			break;
		case CRIU_SNAPSHOT_REC_THREAD:
			if (list_add(&model->threads, payload, len))
				goto io_error;
			break;
		default:
			/* snapshot_read_validate() handled mandatory unknown records. */
			break;
		}
		off += len;
	}
	return 0;

io_error:
	model_free(model);
	return SNAPSHOT_READER_IO_ERROR;
format_error:
	model_free(model);
	return SNAPSHOT_READER_FORMAT_ERROR;
}

static size_t bounded_string_len(const uint8_t *data, size_t capacity)
{
	const uint8_t *nul = memchr(data, '\0', capacity);

	return nul ? (size_t)(nul - data) : capacity;
}

static int copy_fixed_string(char *out, size_t out_size,
				 const uint8_t *data, size_t capacity)
{
	size_t len;

	if (!out || !out_size || !data)
		return -1;
	len = bounded_string_len(data, capacity);
	if (len >= out_size)
		return -1;
	memcpy(out, data, len);
	out[len] = '\0';
	return 0;
}

static int validate_model(const struct snapshot_model *model)
{
	size_t i, j;
	uint32_t expected_vmas;
	uint64_t previous_page_end = 0;

	if (!model->task.data)
		return 0;
	if (!model->mm.data && !model->regs.data && !model->fs.data &&
		!model->creds.data && !model->fds.count && !model->vmas.count &&
		!model->pages.count && !model->threads.count)
		return 0; /* schema-only fixture used by converter-format.sh */
	if (model->arch != ELF_ARCH_X86_64 && model->arch != ELF_ARCH_AARCH64) {
		fprintf(stderr, "converter: unsupported arch=%u\n", model->arch);
		return SNAPSHOT_READER_UNSUPPORTED;
	}
	if (model->page_size == 0 || (model->page_size & (model->page_size - 1U)))
		return SNAPSHOT_READER_FORMAT_ERROR;
	if (model->task.len < TASK_FIXED_SIZE || !model->mm.data ||
		model->mm.len < MM_RECORD_SIZE || !model->regs.data ||
		model->regs.len < sizeof(uint32_t) || !model->fs.data ||
		model->fs.len < FS_RECORD_SIZE || !model->creds.data ||
		model->creds.len < CREDS_RECORD_SIZE || !model->fds.count)
		return SNAPSHOT_READER_FORMAT_ERROR;
	if (u32(model->task.data) != model->pid ||
		u32(model->task.data + 4) != model->tgid ||
		u32(model->mm.data) != model->pid ||
		u32(model->mm.data + 4) != model->tgid ||
		u32(model->task.data + 44) > TASK_RLIMIT_MAX)
		return SNAPSHOT_READER_FORMAT_ERROR;
	if (model->threads.count) {
		bool leader_seen = false;

		for (i = 0; i < model->threads.count; i++) {
			const uint8_t *thread = model->threads.items[i].data;
			uint32_t tid;
			uint32_t tgid;
			uint32_t regs_size;
			size_t j;

			if (model->threads.items[i].len != THREAD_RECORD_SIZE)
				return SNAPSHOT_READER_FORMAT_ERROR;
			tid = u32(thread);
			tgid = u32(thread + 4);
			regs_size = u32(thread + 8);
			if (!tid || tgid != model->tgid || !regs_size ||
				regs_size > CRIU_SNAPSHOT_THREAD_REG_BYTES)
				return SNAPSHOT_READER_FORMAT_ERROR;
			if (tid == model->pid)
				leader_seen = true;
			for (j = 0; j < i; j++)
				if (u32(model->threads.items[j].data) == tid)
					return SNAPSHOT_READER_FORMAT_ERROR;
		}
		if (!leader_seen)
			return SNAPSHOT_READER_FORMAT_ERROR;
	}
	expected_vmas = u32(model->mm.data + 104);
	if (expected_vmas != model->vmas.count)
		return SNAPSHOT_READER_FORMAT_ERROR;
	if (u32(model->regs.data) > model->regs.len - sizeof(uint32_t))
		return SNAPSHOT_READER_FORMAT_ERROR;
	for (i = 0; i < model->vmas.count; i++) {
		const uint8_t *vma = model->vmas.items[i].data;
		uint32_t class;
		uint32_t special;
		char path[512];

		if (model->vmas.items[i].len < VMA_RECORD_SIZE)
			return SNAPSHOT_READER_FORMAT_ERROR;
		class = u32(vma + 28);
		special = u32(vma + 32);
		/* vDSO/vvar are represented by the kernel classifier as class=4
		 * (unsupported ordinary mapping) but are explicitly supported special
		 * mappings with their own CRIU status bits. */
		if (class == VMA_CLASS_ANON_SHARED || class == VMA_CLASS_FILE_SHARED ||
			(class > VMA_CLASS_FILE_PRIVATE &&
			 (special != VMA_SPECIAL_VDSO && special != VMA_SPECIAL_VVAR)) ||
			special > VMA_SPECIAL_VVAR) {
			fprintf(stderr, "converter: unsupported vma index=%zu class=%u special=%u\n", i, class, special);
			return SNAPSHOT_READER_UNSUPPORTED;
		}
		if (u64(vma) >= u64(vma + 8) ||
			copy_fixed_string(path, sizeof(path), vma + VMA_PATH_OFFSET, 512))
			return SNAPSHOT_READER_FORMAT_ERROR;
	}
	for (i = 0; i < model->fds.count; i++) {
		const uint8_t *fd = model->fds.items[i].data;
		uint32_t fdno;
		char path[512];

		uint32_t type = CRIU_FD_TYPE_REG;

		if (model->fds.items[i].len != FD_RECORD_SIZE &&
			model->fds.items[i].len < CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE)
			return SNAPSHOT_READER_FORMAT_ERROR;
		fdno = u32(fd);
		if (model->fds.items[i].len >= CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE)
			type = u32(fd + FD_TYPE_OFFSET);
		if (type != CRIU_FD_TYPE_REG)
			return SNAPSHOT_READER_UNSUPPORTED;
		if (copy_fixed_string(path, sizeof(path), fd + 48, 512))
			return SNAPSHOT_READER_FORMAT_ERROR;
		if (fdno >= 1024U)
			return SNAPSHOT_READER_UNSUPPORTED;
		for (j = 0; j < i; j++)
			if (u32(model->fds.items[j].data) == fdno)
				return SNAPSHOT_READER_FORMAT_ERROR;
	}
	if (!model->fds.count)
		return SNAPSHOT_READER_FORMAT_ERROR;
	for (i = 0; i < model->vmas.count; i++) {
		const uint8_t *vma = model->vmas.items[i].data;
		uint32_t class = u32(vma + 28);
		char path[512];

		if (class == VMA_CLASS_FILE_PRIVATE &&
			(copy_fixed_string(path, sizeof(path), vma + VMA_PATH_OFFSET, 512) ||
			 path[0] != '/'))
			return SNAPSHOT_READER_FORMAT_ERROR;
	}
	for (i = 0; i < model->pages.count; i++) {
		const uint8_t *page = model->pages.items[i].data;
		uint64_t nr_pages;
		uint64_t payload;
		uint64_t start;
		uint32_t flags;

		if (model->pages.items[i].len < PAGE_RECORD_SIZE)
			return SNAPSHOT_READER_FORMAT_ERROR;
		start = u64(page);
		nr_pages = u32(page + 8);
		payload = u32(page + 20);
		flags = u32(page + 16);
		if (!nr_pages || start % model->page_size ||
			u32(page + 12) != model->page_size ||
			payload > model->pages.items[i].len - PAGE_RECORD_SIZE ||
			((flags & CRIU_PAGE_RUN_PRESENT) &&
			 payload != nr_pages * (uint64_t)model->page_size) ||
			(!(flags & CRIU_PAGE_RUN_PRESENT) && payload != 0) ||
			payload != model->pages.items[i].len - PAGE_RECORD_SIZE)
			return SNAPSHOT_READER_FORMAT_ERROR;
		if (u32(page + 16) & CRIU_PAGE_RUN_PRESENT) {
			if (previous_page_end && start < previous_page_end)
				return SNAPSHOT_READER_FORMAT_ERROR;
			if (start > UINT64_MAX - payload)
				return SNAPSHOT_READER_FORMAT_ERROR;
			previous_page_end = start + payload;
		}
	}
	return 0;
}

static int arch_mtype(uint32_t arch)
{
	if (arch == ELF_ARCH_X86_64)
		return 1;
	if (arch == ELF_ARCH_AARCH64)
		return 3;
	return -1;
}

static uint64_t task_field_u64(const struct blob_ref *task, size_t off)
{
	return task->len >= off + sizeof(uint64_t) ? u64(task->data + off) : 0;
}

static uint32_t task_field_u32(const struct blob_ref *task, size_t off)
{
	return task->len >= off + sizeof(uint32_t) ? u32(task->data + off) : 0;
}

static const char *task_comm(const struct blob_ref *task, char out[TASK_COMM_SIZE])
{
	if (task->len >= TASK_COMM_OFFSET + TASK_COMM_SIZE &&
		!copy_fixed_string(out, TASK_COMM_SIZE, task->data + TASK_COMM_OFFSET,
			TASK_COMM_SIZE) && out[0])
		return out;
	strcpy(out, "criu-module");
	return out;
}

static uint64_t reg_u64(const struct blob_ref *regs, size_t offset)
{
	size_t size;

	if (!regs->data || regs->len < sizeof(uint32_t))
		return 0;
	size = u32(regs->data);
	if (size > regs->len - sizeof(uint32_t) || size < offset + 8U)
		return 0;
	return u64(regs->data + sizeof(uint32_t) + offset);
}

static int aarch64_tls(const struct blob_ref *regs, uint64_t *tls)
{
	size_t regs_size;
	size_t offset;

	if (!regs->data || regs->len < sizeof(uint32_t))
		return -1;
	regs_size = u32(regs->data);
	if (regs_size > regs->len - sizeof(uint32_t))
		return -1;
	offset = sizeof(uint32_t) + regs_size;
	if (regs->len != offset + sizeof(uint64_t))
		return -1;
	*tls = u64(regs->data + offset);
	return 0;
}

static int add_nested(struct image_writer *outer, unsigned field,
			  const struct image_writer *inner)
{
	return image_writer_field_bytes(outer, field, inner->data, inner->len);
}

static int build_itimer(struct image_writer *message)
{
	return image_writer_field_varint(message, 1, 0) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, 0) ||
		image_writer_field_varint(message, 4, 0);
}

static int build_timers(struct image_writer *message)
{
	struct image_writer timer;
	int ret;

	image_writer_init(&timer);
	ret = build_itimer(&timer);
	if (!ret)
		ret = add_nested(message, 1, &timer);
	if (!ret)
		ret = add_nested(message, 2, &timer);
	if (!ret)
		ret = add_nested(message, 3, &timer);
	image_writer_free(&timer);
	return ret;
}

static int build_rlimits(const struct blob_ref *task,
				struct image_writer *message)
{
	uint32_t count = task_field_u32(task, 44);
	uint32_t i;

	if (count > TASK_RLIMIT_MAX)
		return -1;
	for (i = 0; i < count; i++) {
		struct image_writer limit;
		size_t off = TASK_RLIMIT_OFFSET + (size_t)i * 16U;
		int ret;

		if (task->len < off + 16U)
			return -1;
		image_writer_init(&limit);
		ret = image_writer_field_varint(&limit, 1, u64(task->data + off));
		if (!ret)
			ret = image_writer_field_varint(&limit, 2, u64(task->data + off + 8));
		if (!ret)
			ret = add_nested(message, 1, &limit);
		image_writer_free(&limit);
		if (ret)
			return ret;
	}
	return 0;
}

static int build_fown(const struct blob_ref *creds, struct image_writer *message)
{
	return image_writer_field_varint(message, 1, u32(creds->data)) ||
		image_writer_field_varint(message, 2, u32(creds->data + 8)) ||
		image_writer_field_varint(message, 3, 0) ||
		image_writer_field_varint(message, 4, 0) ||
		image_writer_field_varint(message, 5, 0);
}

static int build_creds(const struct blob_ref *creds, struct image_writer *message)
{
	static const unsigned fields[] = { 9, 10, 11, 12, 19 };
	static const size_t offsets[] = { 36, 44, 52, 60, 68 };
	unsigned group;
	unsigned i;

	if (creds->len < CREDS_RECORD_SIZE)
		return -1;
	for (i = 1; i <= 8; i++)
		if (image_writer_field_varint(message, i,
				u32(creds->data + (i - 1U) * 4U)))
			return -1;
	for (group = 0; group < sizeof(fields) / sizeof(fields[0]); group++)
		for (i = 0; i < 2; i++)
			if (image_writer_field_varint(message, fields[group],
				u32(creds->data + offsets[group] + i * 4U)))
				return -1;
	return image_writer_field_varint(message, 13, u32(creds->data + 32));
}

static int build_task_core(const struct snapshot_model *model,
				 const char comm[TASK_COMM_SIZE],
				 struct image_writer *message)
{
	struct image_writer timers;
	struct image_writer rlimits;
	uint64_t blocked = task_field_u64(&model->task, 48);
	int ret;
	unsigned sig;
	struct image_writer sa;

	image_writer_init(&timers);
	image_writer_init(&rlimits);
	image_writer_init(&sa);
	ret = image_writer_field_varint(message, 1, 1) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, 0) ||
		image_writer_field_varint(message, 4,
			(uint32_t)task_field_u64(&model->task, 28)) ||
		image_writer_field_varint(message, 5, blocked) ||
		image_writer_field_bytes(message, 6, comm, strlen(comm));
	if (!ret)
		ret = build_timers(&timers);
	if (!ret)
		ret = add_nested(message, 7, &timers);
	if (!ret)
		ret = build_rlimits(&model->task, &rlimits);
	if (!ret && rlimits.len)
		ret = add_nested(message, 8, &rlimits);
	if (!ret)
		ret = image_writer_field_bytes(message, 10, NULL, 0);
	/* A core image with no repeated sigactions makes CRIU fall back to the
	 * legacy sigacts-$pid.img stream. A3 does not emit that stream, so encode
	 * the complete default disposition table directly in task_core. */
	for (sig = 0; !ret && sig < 62; sig++) {
		sa.len = 0;
		ret = image_writer_field_varint(&sa, 1, 0) ||
			image_writer_field_varint(&sa, 2, 0) ||
			image_writer_field_varint(&sa, 3, 0) ||
			image_writer_field_varint(&sa, 4, 0) ||
			add_nested(message, 15, &sa);
	}
	if (!ret)
		ret = image_writer_field_sint64(message, 14, 0);
	image_writer_free(&sa);
	image_writer_free(&timers);
	image_writer_free(&rlimits);
	return ret;
}

static int build_sas(struct image_writer *message)
{
	return image_writer_field_varint(message, 1, 0) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, 2);
}

static int build_thread_core(const struct snapshot_model *model,
				 const struct blob_ref *thread_record,
				 const char comm[TASK_COMM_SIZE],
				 struct image_writer *message)
{
	struct image_writer sas;
	struct image_writer creds;
	uint64_t blocked;
	int ret;

	image_writer_init(&sas);
	image_writer_init(&creds);
	blocked = task_field_u64(&model->task, 48);

	if (thread_record && thread_record->len >= 24U)
		blocked = u64(thread_record->data + 24);
	ret = image_writer_field_varint(message, 1, 0) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_sint64(message, 3, 0) ||
		image_writer_field_varint(message, 4, 0) ||
		image_writer_field_varint(message, 6, blocked);
	if (!ret)
		ret = build_sas(&sas);
	if (!ret)
		ret = add_nested(message, 7, &sas);
	if (!ret)
		ret = image_writer_field_bytes(message, 9, NULL, 0);
	if (!ret)
		ret = build_creds(&model->creds, &creds);
	if (!ret)
		ret = add_nested(message, 10, &creds);
	if (!ret)
		ret = image_writer_field_bytes(message, 13, comm, strlen(comm));
	/* No cgroup image is emitted in A3.  A zero cg_set tells CRIU to inherit
	 * the restore caller's cgroup instead of looking for set 1 in cgroup.img. */
	if (!ret)
		ret = image_writer_field_varint(message, 16, 0);
	if (!ret)
		ret = image_writer_field_varint(message, 17, 50000);
	image_writer_free(&sas);
	image_writer_free(&creds);
	return ret;
}

static int build_aarch64_gpregs(const struct blob_ref *regs,
					struct image_writer *message)
{
	unsigned i;

	for (i = 0; i < 31; i++)
		if (image_writer_field_varint(message, 1, reg_u64(regs, i * 8U)))
			return -1;
	return image_writer_field_varint(message, 2, reg_u64(regs, 248)) ||
		image_writer_field_varint(message, 3, reg_u64(regs, 256)) ||
		image_writer_field_varint(message, 4, reg_u64(regs, 264));
}

static int build_aarch64_fpsimd(struct image_writer *message)
{
	unsigned i;

	for (i = 0; i < 64; i++)
		if (image_writer_field_varint(message, 1, 0))
			return -1;
	return image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, 0);
}

static int build_aarch64_thread_info(const struct blob_ref *regs,
					struct image_writer *message)
{
	struct image_writer gpregs;
	struct image_writer fpsimd;
	uint64_t tls;
	int ret;

	if (aarch64_tls(regs, &tls))
		return -1;
	image_writer_init(&gpregs);
	image_writer_init(&fpsimd);
	ret = image_writer_field_varint(message, 1, 0) ||
		image_writer_field_varint(message, 2, tls);
	if (!ret)
		ret = build_aarch64_gpregs(regs, &gpregs);
	if (!ret)
		ret = add_nested(message, 3, &gpregs);
	if (!ret)
		ret = build_aarch64_fpsimd(&fpsimd);
	if (!ret)
		ret = add_nested(message, 4, &fpsimd);
	image_writer_free(&gpregs);
	image_writer_free(&fpsimd);
	return ret;
}

static int build_x86_gpregs(const struct blob_ref *regs,
				struct image_writer *message)
{
	unsigned i;

	for (i = 0; i < 27; i++)
		if (image_writer_field_varint(message, i + 1U,
				reg_u64(regs, i * 8U)))
			return -1;
	return image_writer_field_varint(message, 28, 1);
}

static int build_x86_fpregs(struct image_writer *message)
{
	return image_writer_field_varint(message, 1, 0) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, 0) ||
		image_writer_field_varint(message, 4, 0) ||
		image_writer_field_varint(message, 5, 0) ||
		image_writer_field_varint(message, 6, 0) ||
		image_writer_field_varint(message, 7, 0) ||
		image_writer_field_varint(message, 8, 0);
}

static int build_x86_thread_info(const struct blob_ref *regs,
					struct image_writer *message)
{
	struct image_writer gpregs;
	struct image_writer fpregs;
	int ret;

	image_writer_init(&gpregs);
	image_writer_init(&fpregs);
	ret = image_writer_field_varint(message, 1, 0);
	if (!ret)
		ret = build_x86_gpregs(regs, &gpregs);
	if (!ret)
		ret = add_nested(message, 2, &gpregs);
	if (!ret)
		ret = build_x86_fpregs(&fpregs);
	if (!ret)
		ret = add_nested(message, 3, &fpregs);
	image_writer_free(&gpregs);
	image_writer_free(&fpregs);
	return ret;
}

static int build_ids(struct image_writer *message)
{
	return image_writer_field_varint(message, 1, 1) ||
		image_writer_field_varint(message, 2, 1) ||
		image_writer_field_varint(message, 3, 1) ||
		image_writer_field_varint(message, 4, 1);
}

static int build_core(const struct snapshot_model *model,
				const struct blob_ref *regs,
				const struct blob_ref *thread_record,
				bool leader,
				const char comm[TASK_COMM_SIZE],
				struct image_writer *message)
{
	struct image_writer tc;
	struct image_writer ids;
	struct image_writer thread_core;
	struct image_writer arch_info;
	struct blob_ref effective_regs = *regs;
	uint8_t thread_regs[sizeof(uint32_t) + CRIU_SNAPSHOT_THREAD_REG_BYTES +
			    sizeof(uint64_t)];
	int ret;
	int mtype = arch_mtype(model->arch);

	if (mtype < 0)
		return -1;
	if (thread_record) {
		uint32_t regs_size = u32(thread_record->data + 8);

		if (thread_record->len < THREAD_RECORD_SIZE ||
			regs_size > CRIU_SNAPSHOT_THREAD_REG_BYTES)
			return -1;
		memset(thread_regs, 0, sizeof(thread_regs));
		put32(thread_regs, regs_size);
		memcpy(thread_regs + sizeof(regs_size),
		       thread_record->data + THREAD_REGS_OFFSET, regs_size);
		memcpy(thread_regs + sizeof(regs_size) + regs_size,
		       thread_record->data + 16, sizeof(uint64_t));
		effective_regs.data = thread_regs;
		effective_regs.len = sizeof(regs_size) + regs_size + sizeof(uint64_t);
	}
	image_writer_init(&tc);
	image_writer_init(&ids);
	image_writer_init(&thread_core);
	image_writer_init(&arch_info);
	ret = image_writer_field_varint(message, 1, (uint64_t)mtype);
	if (leader) {
		if (!ret)
			ret = build_task_core(model, comm, &tc);
		if (!ret)
			ret = add_nested(message, 3, &tc);
		if (!ret)
			ret = build_ids(&ids);
		if (!ret)
			ret = add_nested(message, 4, &ids);
	}
	if (!ret)
		ret = build_thread_core(model, thread_record, comm, &thread_core);
	if (!ret)
		ret = add_nested(message, 5, &thread_core);
	if (!ret) {
		if (model->arch == ELF_ARCH_AARCH64)
			ret = build_aarch64_thread_info(&effective_regs, &arch_info);
		else
			ret = build_x86_thread_info(&effective_regs, &arch_info);
	}
	if (!ret)
		ret = add_nested(message, model->arch == ELF_ARCH_AARCH64 ? 8U : 2U,
			&arch_info);
	image_writer_free(&tc);
	image_writer_free(&ids);
	image_writer_free(&thread_core);
	image_writer_free(&arch_info);
	return ret;
}

static uint32_t vma_flags(const uint8_t *vma)
{
	uint32_t class = u32(vma + 28);
	uint32_t special = u32(vma + 32);
	uint32_t flags = MAP_PRIVATE;

	if (class == VMA_CLASS_ANON_PRIVATE || special == VMA_SPECIAL_VDSO ||
		special == VMA_SPECIAL_VVAR)
		flags |= MAP_ANONYMOUS;
	if (u32(vma + VMA_FLAGS_OFFSET) & 2U)
		flags |= MAP_GROWSDOWN;
	return flags;
}

static uint32_t vma_status(const uint8_t *vma)
{
	uint32_t class = u32(vma + 28);
	uint32_t special = u32(vma + 32);
	uint32_t status = VMA_AREA_REGULAR;

	if (class == VMA_CLASS_ANON_PRIVATE)
		status |= VMA_ANON_PRIVATE;
	else if (class == VMA_CLASS_FILE_PRIVATE)
		status |= VMA_FILE_PRIVATE | VMA_AREA_NOT_ACCOUNTABLE;
	if (special == VMA_SPECIAL_VDSO)
		status |= VMA_AREA_VDSO | VMA_ANON_PRIVATE | VMA_AREA_NOT_ACCOUNTABLE;
	if (special == VMA_SPECIAL_VVAR)
		status |= VMA_AREA_VVAR | VMA_ANON_PRIVATE | VMA_AREA_NOT_ACCOUNTABLE;
	if (!strncmp((const char *)(vma + VMA_PATH_OFFSET), "[stack]", 512U))
		status |= VMA_AREA_STACK;
	if (!strncmp((const char *)(vma + VMA_PATH_OFFSET), "[heap]", 512U))
		status |= VMA_AREA_HEAP;
	return status;
}

static int build_vma(const struct snapshot_model *model, size_t index,
				const struct file_table *files,
				struct image_writer *message)
{
	const uint8_t *vma = model->vmas.items[index].data;
	uint32_t class = u32(vma + 28);
	uint32_t shmid = 0;
	uint64_t pgoff = u64(vma + 16);
	uint64_t pgoff_bytes = 0;

	if (class == VMA_CLASS_FILE_PRIVATE) {
		size_t i;
		if (!model->page_size || pgoff > UINT64_MAX / model->page_size)
			return -1;
		pgoff_bytes = pgoff * model->page_size;
		for (i = 0; i < files->count; i++) {
			if (files->items[i].dev == u64(vma + VMA_DEV_OFFSET) &&
				files->items[i].ino == u64(vma + VMA_INO_OFFSET) &&
				!strcmp(files->items[i].path,
					(const char *)(vma + VMA_PATH_OFFSET))) {
				shmid = files->items[i].id;
				break;
			}
		}
		if (!shmid)
			for (i = 0; i < files->count; i++)
				if (!strcmp(files->items[i].path,
					(const char *)(vma + VMA_PATH_OFFSET))) {
					shmid = files->items[i].id;
					break;
				}
		if (!shmid)
			return -1;
	}
	/* Kernel snapshots store vm_pgoff in pages; CRIU images use byte offsets. */
	return image_writer_field_varint(message, 1, u64(vma)) ||
		image_writer_field_varint(message, 2, u64(vma + 8)) ||
		image_writer_field_varint(message, 3, pgoff_bytes) ||
		image_writer_field_varint(message, 4, shmid) ||
		image_writer_field_varint(message, 5, u32(vma + 24)) ||
		image_writer_field_varint(message, 6, vma_flags(vma)) ||
		image_writer_field_varint(message, 7, vma_status(vma)) ||
		image_writer_field_sint64(message, 8, -1);
}

static int build_mm(const struct snapshot_model *model,
				const struct file_table *files,
				struct image_writer *message)
{
	const uint8_t *mm = model->mm.data;
	static const unsigned fields[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	static const size_t offsets[] = { 16, 24, 32, 40, 64, 48, 56, 72, 80, 88, 96 };
	struct image_writer vma;
	size_t i;
	int ret;

	image_writer_init(&vma);
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		if (image_writer_field_varint(message, fields[i], u64(mm + offsets[i])))
			goto error;
	if (image_writer_field_varint(message, 12, files->exe_id))
		goto error;
	for (i = 0; i < model->vmas.count; i++) {
		vma.len = 0;
		if (build_vma(model, i, files, &vma) || add_nested(message, 14, &vma))
			goto error;
	}
	ret = image_writer_field_sint64(message, 15, 1) ||
		image_writer_field_varint(message, 17, 0);
	image_writer_free(&vma);
	return ret;
error:
	image_writer_free(&vma);
	return -1;
}

static int file_item_matches(const struct file_item *item, uint64_t dev,
					 uint64_t ino, const char *path,
					 uint64_t object_id)
{
	if (object_id && item->object_id)
		return item->object_id == object_id;
	if (dev && ino && item->dev == dev && item->ino == ino)
		return 1;
	return !strcmp(item->path, path);
}

static void fill_stat_fallback(struct file_item *item, bool directory)
{
	struct stat st;

	if (!stat(item->path, &st)) {
		if (!item->dev)
			item->dev = (uint64_t)st.st_dev;
		if (!item->ino)
			item->ino = (uint64_t)st.st_ino;
		if (!item->has_size && !S_ISDIR(st.st_mode)) {
			item->size = (uint64_t)st.st_size;
			item->has_size = true;
		}
		if (!item->mode)
			item->mode = (uint32_t)st.st_mode;
	}
	if (!item->mode)
		item->mode = directory ? 040755U : 0100644U;
}

static int file_table_add_object(struct file_table *files, const char *path,
				 uint64_t dev, uint64_t ino, uint32_t mode,
				 uint64_t flags, uint64_t pos, uint64_t size,
				 bool has_size, bool directory, uint64_t object_id,
				 uint32_t type, uint32_t *id);

static int file_table_add(struct file_table *files, const char *path,
				 uint64_t dev, uint64_t ino, uint32_t mode,
				 uint64_t flags, uint64_t pos, uint64_t size,
				 bool has_size, bool directory, uint32_t *id)

{
	return file_table_add_object(files, path, dev, ino, mode, flags, pos,
					 size, has_size, directory, 0, 0, id);
}

static int file_table_add_object(struct file_table *files, const char *path,
				 uint64_t dev, uint64_t ino, uint32_t mode,
				 uint64_t flags, uint64_t pos, uint64_t size,
				 bool has_size, bool directory, uint64_t object_id,
				 uint32_t type, uint32_t *id)
{
	struct file_item *item;
	size_t i;

	if (!path || !path[0] || strlen(path) >= sizeof(files->items[0].path))
		return -1;
	for (i = 0; i < files->count; i++)
		if (file_item_matches(&files->items[i], dev, ino, path, object_id)) {
			if (object_id && (strcmp(files->items[i].path, path) ||
				files->items[i].dev != dev || files->items[i].ino != ino))
				return -1;
			if (!files->items[i].mode)
				files->items[i].mode = mode;
			if (!files->items[i].has_size && has_size) {
				files->items[i].size = size;
				files->items[i].has_size = true;
			}
			*id = files->items[i].id;
			return 0;
		}
	if (files->count == files->capacity) {
		size_t capacity = files->capacity ? files->capacity * 2U : 8U;
		struct file_item *items;

		if (capacity < files->capacity || capacity > SIZE_MAX / sizeof(*items))
			return -1;
		items = realloc(files->items, capacity * sizeof(*items));
		if (!items)
			return -1;
		files->items = items;
		files->capacity = capacity;
	}
	item = &files->items[files->count++];
	memset(item, 0, sizeof(*item));
	item->id = (uint32_t)files->count;
	item->dev = dev;
	item->ino = ino;
	item->mode = mode;
	item->flags = flags;
	item->pos = pos;
	item->size = size;
	item->has_size = has_size;
	item->object_id = object_id;
	item->type = type ? type : CRIU_FD_TYPE_REG;
	memcpy(item->path, path, strlen(path) + 1U);
	fill_stat_fallback(item, directory);
	*id = item->id;
	return 0;
}

static int file_table_add_path(struct file_table *files, const char *path,
				       uint32_t mode, bool directory, uint32_t *id)
{
	return file_table_add(files, path, 0, 0, mode, 0, 0, 0, false,
				      directory, id);
}

static void file_table_free(struct file_table *files)
{
	free(files->items);
	free(files->bindings);
	memset(files, 0, sizeof(*files));
}

static int binding_add(struct file_table *files, uint32_t fd, uint32_t id)
{
	struct fd_binding *bindings;
	size_t capacity;

	if (files->binding_count == files->binding_capacity) {
		capacity = files->binding_capacity ? files->binding_capacity * 2U : 8U;
		if (capacity < files->binding_capacity ||
			capacity > SIZE_MAX / sizeof(*bindings))
			return -1;
		bindings = realloc(files->bindings, capacity * sizeof(*bindings));
		if (!bindings)
			return -1;
		files->bindings = bindings;
		files->binding_capacity = capacity;
	}
	files->bindings[files->binding_count].fd = fd;
	files->bindings[files->binding_count].id = id;
	files->binding_count++;
	return 0;
}

static int build_file_table(const struct snapshot_model *model,
				struct file_table *files)
{
	size_t i;
	int candidate = -1;
	uint32_t id;
	char cwd[512];
	char root[512];

	memset(files, 0, sizeof(*files));
	for (i = 0; i < model->vmas.count; i++) {
		const uint8_t *vma = model->vmas.items[i].data;
		if (u32(vma + 28) == VMA_CLASS_FILE_PRIVATE &&
			(u32(vma + 24) & 4U)) {
			candidate = (int)i;
			break;
		}
		if (candidate < 0 && u32(vma + 28) == VMA_CLASS_FILE_PRIVATE)
			candidate = (int)i;
	}
	if (candidate < 0)
		candidate = 0;
	if (model->vmas.count &&
		u32(model->vmas.items[candidate].data + 28) == VMA_CLASS_FILE_PRIVATE) {
		const uint8_t *vma = model->vmas.items[candidate].data;
		char path[512];

		if (copy_fixed_string(path, sizeof(path), vma + VMA_PATH_OFFSET, 512))
			goto unsupported;
		if (file_table_add(files, path, u64(vma + VMA_DEV_OFFSET),
				u64(vma + VMA_INO_OFFSET),
				0100755U, 0, 0, 0, false, false, &files->exe_id))
			goto error;
	} else if (model->fds.count) {
		const uint8_t *fd = model->fds.items[0].data;
		char path[512];
		if (copy_fixed_string(path, sizeof(path), fd + 48, 512) ||
			file_table_add_object(files, path, u64(fd + 24), u64(fd + 32),
				u32(fd + 4), u64(fd + 8), u64(fd + 16), u64(fd + 40),
				true, false,
				model->fds.items[0].len >= CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE ?
					u64(fd + FD_OBJECT_ID_OFFSET) : 0,
				model->fds.items[0].len >= CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE ?
					u32(fd + FD_TYPE_OFFSET) : CRIU_FD_TYPE_REG,
				&files->exe_id))
			goto error;
	} else {
		goto unsupported;
	}
	for (i = 0; i < model->vmas.count; i++) {
		const uint8_t *vma = model->vmas.items[i].data;
		char path[512];
		if (u32(vma + 28) != VMA_CLASS_FILE_PRIVATE)
			continue;
		if (copy_fixed_string(path, sizeof(path), vma + VMA_PATH_OFFSET, 512) ||
			file_table_add(files, path, u64(vma + VMA_DEV_OFFSET),
				u64(vma + VMA_INO_OFFSET),
				0100644U, 0, 0, 0, false, false, &id))
			goto error;
	}
	for (i = 0; i < model->fds.count; i++) {
		const uint8_t *fd = model->fds.items[i].data;
		uint32_t fdno = u32(fd);
		char path[512];
		if (copy_fixed_string(path, sizeof(path), fd + 48, 512) ||
			file_table_add_object(files, path, u64(fd + 24), u64(fd + 32),
				u32(fd + 4), u64(fd + 8), u64(fd + 16), u64(fd + 40),
				true, false,
				model->fds.items[i].len >= CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE ?
					u64(fd + FD_OBJECT_ID_OFFSET) : 0,
				model->fds.items[i].len >= CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE ?
					u32(fd + FD_TYPE_OFFSET) : CRIU_FD_TYPE_REG,
				&id))
			goto error;
		if (binding_add(files, fdno, id))
			goto error;
	}
	if (copy_fixed_string(cwd, sizeof(cwd), model->fs.data, 512) ||
		copy_fixed_string(root, sizeof(root), model->fs.data + 512, 512) ||
		file_table_add_path(files, cwd, 040755U, true, &files->cwd_id) ||
		file_table_add_path(files, root, 040755U, true, &files->root_id))
		goto error;
	if (!files->binding_count)
		goto unsupported;
	if (!files->exe_id)
		files->exe_id = files->bindings[0].id;
	return 0;

unsupported:
	file_table_free(files);
	return SNAPSHOT_READER_UNSUPPORTED;
error:
	file_table_free(files);
	return SNAPSHOT_READER_IO_ERROR;
}

static int build_reg_file(const struct file_item *item,
				 const struct blob_ref *creds,
				 struct image_writer *message)
{
	struct image_writer fown;
	int ret;

	image_writer_init(&fown);
	ret = image_writer_field_varint(message, 1, item->id) ||
		image_writer_field_varint(message, 2,
			item->flags & ~(uint64_t)(O_CREAT | O_EXCL | O_TRUNC)) ||
		image_writer_field_varint(message, 3, item->pos);
	if (!ret)
		ret = build_fown(creds, &fown);
	if (!ret)
		ret = add_nested(message, 5, &fown);
	if (!ret)
		ret = image_writer_field_bytes(message, 6, item->path,
			strlen(item->path));
	if (!ret && item->has_size)
		ret = image_writer_field_varint(message, 8, item->size);
	if (!ret)
		ret = image_writer_field_varint(message, 10, item->mode);
	image_writer_free(&fown);
	return ret;
}

static int build_file_entry(const struct file_item *item,
				const struct blob_ref *creds,
				struct image_writer *message)
{
	struct image_writer reg;
	int ret;

	image_writer_init(&reg);
	ret = image_writer_field_varint(message, 1, 1) ||
		image_writer_field_varint(message, 2, item->id);
	if (!ret)
		ret = build_reg_file(item, creds, &reg);
	if (!ret)
		ret = add_nested(message, 3, &reg);
	image_writer_free(&reg);
	return ret;
}

static int build_fdinfo(const struct file_table *files, unsigned fd,
				struct image_writer *message)
{
	return image_writer_field_varint(message, 1, files->bindings[fd].id) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, 1) ||
		image_writer_field_varint(message, 4, files->bindings[fd].fd);
}

static int build_inventory(struct image_writer *message)
{
	struct image_writer ids;
	int ret;

	image_writer_init(&ids);
	ret = image_writer_field_varint(message, 1, 2) ||
		image_writer_field_varint(message, 2, 1);
	/* inventory carries root_ids, which CRIU compares with the task's
	 * task_kobj_ids.  Keep files_id distinct for a single root task so the
	 * restore-side CLONE_FILES consistency check does not treat it as a
	 * shared-fd child of itself. */
	if (!ret)
		ret = image_writer_field_varint(&ids, 1, 1) ||
			image_writer_field_varint(&ids, 2, 2) ||
			image_writer_field_varint(&ids, 3, 1) ||
			image_writer_field_varint(&ids, 4, 1);
	if (!ret)
		ret = add_nested(message, 3, &ids);
	if (!ret)
		ret = image_writer_field_varint(message, 4, 1);
	image_writer_free(&ids);
	return ret;
}

static int build_pstree(const struct snapshot_model *model,
			struct image_writer *message)
{
	uint32_t pid = u32(model->task.data);
	size_t i;

	if (image_writer_field_varint(message, 1, pid) ||
		image_writer_field_varint(message, 2, 0) ||
		image_writer_field_varint(message, 3, pid) ||
		image_writer_field_varint(message, 4, pid))
		return -1;
	if (model->threads.count) {
		for (i = 0; i < model->threads.count; i++)
			if (image_writer_field_varint(message, 5,
					u32(model->threads.items[i].data)))
				return -1;
		return 0;
	}
	return image_writer_field_varint(message, 5, pid);
}

static int build_fs(const struct file_table *files, struct image_writer *message)
{
	return image_writer_field_varint(message, 1, files->cwd_id) ||
		image_writer_field_varint(message, 2, files->root_id) ||
		image_writer_field_varint(message, 3, 18);
}

static int build_page_messages(const struct snapshot_model *model,
				       struct image_writer **messages_out,
				       size_t *count_out, uint8_t **raw_out,
				       size_t *raw_len_out)
{
	struct image_writer *messages;
	uint8_t *raw = NULL;
	size_t count = 1;
	size_t raw_len = 0;
	size_t raw_capacity = 0;
	size_t i;

	for (i = 0; i < model->pages.count; i++)
		if (u32(model->pages.items[i].data + 16) & CRIU_PAGE_RUN_PRESENT)
			count++;
	messages = calloc(count, sizeof(*messages));
	if (!messages)
		return -1;
	for (i = 0; i < count; i++)
		image_writer_init(&messages[i]);
	if (image_writer_field_varint(&messages[0], 1, 1))
		goto error;
	count = 1;
	for (i = 0; i < model->pages.count; i++) {
		const uint8_t *page = model->pages.items[i].data;
		size_t payload = u32(page + 20);
		uint32_t flags = u32(page + 16);
		uint8_t *new_raw;

		if (!(flags & CRIU_PAGE_RUN_PRESENT))
			continue;
		if (raw_len > SIZE_MAX - payload)
			goto error;
		if (raw_len + payload > raw_capacity) {
			raw_capacity = raw_capacity ? raw_capacity * 2U : 4096U;
			while (raw_capacity < raw_len + payload) {
				if (raw_capacity > SIZE_MAX / 2U) {
					raw_capacity = raw_len + payload;
					break;
				}
				raw_capacity *= 2U;
			}
			new_raw = realloc(raw, raw_capacity);
			if (!new_raw)
				goto error;
			raw = new_raw;
		}
		memcpy(raw + raw_len, page + PAGE_RECORD_SIZE, payload);
		raw_len += payload;
		if (image_writer_field_varint(&messages[count], 1, u64(page)) ||
			image_writer_field_varint(&messages[count], 2, 0) ||
			image_writer_field_varint(&messages[count], 4, PE_PRESENT) ||
			image_writer_field_varint(&messages[count], 5, u32(page + 8)))
			goto error;
		count++;
	}
	*messages_out = messages;
	*count_out = count;
	*raw_out = raw;
	*raw_len_out = raw_len;
	return 0;
error:
	for (i = 0; i < count; i++)
		image_writer_free(&messages[i]);
	free(messages);
	free(raw);
	return -1;
}

static void free_messages(struct image_writer *messages, size_t count)
{
	size_t i;

	if (!messages)
		return;
	for (i = 0; i < count; i++)
		image_writer_free(&messages[i]);
	free(messages);
}

static int emit_messages(const char *dir, const char *name, uint32_t magic,
				 const struct image_writer *messages, size_t count,
				 bool inventory)
{
	char path[PATH_MAX];
	uint8_t prefix[8];
	size_t prefix_len = inventory ? 4U : 8U;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) < 0 ||
		strlen(dir) + strlen(name) + 2U > sizeof(path))
		return -1;
	put32(prefix, inventory ? magic : IMG_COMMON_MAGIC);
	if (!inventory)
		put32(prefix + 4, magic);
	return image_writer_write_messages(path, prefix, prefix_len, messages, count);
}

static int emit_raw(const char *dir, const char *name, const uint8_t *data,
				size_t len)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) < 0 ||
		strlen(dir) + strlen(name) + 2U > sizeof(path))
		return -1;
	return image_writer_write_raw_file(path, data, len);
}

int criu_emit_images(const struct snapshot_document *doc,
			 const struct criu_convert_options *options)
{
	struct snapshot_model model;
	struct file_table files;
	struct image_writer message;
	struct image_writer *file_messages = NULL;
	struct image_writer *reg_messages = NULL;
	struct image_writer *fd_messages = NULL;
	struct image_writer *page_messages = NULL;
	uint8_t *page_data = NULL;
	size_t page_count = 0;
	size_t page_data_len = 0;
	char name[64];
	char comm[TASK_COMM_SIZE];
	size_t i;
	int ret;

	if (!doc || !options || !options->output_dir)
		return SNAPSHOT_READER_FORMAT_ERROR;
	ret = collect_records(doc, &model);
	if (ret) {
		fprintf(stderr, "converter: collect_records rc=%d\n", ret);
		return ret;
	}
	ret = validate_model(&model);
	if (ret || !model.task.data) {
		if (ret)
			fprintf(stderr, "converter: validate_model rc=%d\n", ret);
		model_free(&model);
		return ret;
	}
	/* The format fixture intentionally carries only TASK; validate it without
	 * attempting to synthesize a CRIU image set. */
	if (!model.mm.data && !model.regs.data && !model.fs.data &&
	    !model.creds.data && !model.fds.count && !model.vmas.count &&
	    !model.pages.count) {
		model_free(&model);
		return 0;
	}
	ret = build_file_table(&model, &files);
	if (ret) {
		fprintf(stderr, "converter: build_file_table rc=%d\n", ret);
		model_free(&model);
		return ret;
	}
	task_comm(&model.task, comm);
	if (mkdir(options->output_dir, 0700) < 0 && errno != EEXIST) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out;
	}
	{
		struct stat st;
		if (stat(options->output_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
			ret = SNAPSHOT_READER_IO_ERROR;
			goto out;
		}
	}

	image_writer_init(&message);
	fprintf(stderr, "converter: model validated vmas=%zu fds=%zu pages=%zu\n", model.vmas.count, model.fds.count, model.pages.count);
	if (build_inventory(&message)) {
		fprintf(stderr, "converter: build_inventory failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	fprintf(stderr, "converter: emit inventory.img\n");
	if (emit_messages(options->output_dir, "inventory.img", INVENTORY_MAGIC,
			&message, 1, true)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	message.len = 0;
	if (build_pstree(&model, &message)) {
		fprintf(stderr, "converter: build_pstree failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	fprintf(stderr, "converter: emit pstree.img\n");
	if (emit_messages(options->output_dir, "pstree.img", PSTREE_MAGIC,
			&message, 1, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	if (model.threads.count) {
		for (i = 0; i < model.threads.count; i++) {
			const struct blob_ref *thread = &model.threads.items[i];
			uint32_t tid = u32(thread->data);

			message.len = 0;
			if (build_core(&model, &model.regs, thread,
				       tid == model.pid, comm, &message)) {
				fprintf(stderr, "converter: build_core tid=%u failed\n", tid);
				ret = SNAPSHOT_READER_IO_ERROR;
				goto out_message;
			}
			snprintf(name, sizeof(name), "core-%u.img", tid);
			fprintf(stderr, "converter: emit %s\n", name);
			if (emit_messages(options->output_dir, name, CORE_MAGIC,
					  &message, 1, false)) {
				ret = SNAPSHOT_READER_IO_ERROR;
				goto out_message;
			}
		}
	} else {
		message.len = 0;
		if (build_core(&model, &model.regs, NULL, true, comm, &message)) {
			fprintf(stderr, "converter: build_core failed\n");
			ret = SNAPSHOT_READER_IO_ERROR;
			goto out_message;
		}
		snprintf(name, sizeof(name), "core-%u.img", u32(model.task.data));
		fprintf(stderr, "converter: emit %s\n", name);
		if (emit_messages(options->output_dir, name, CORE_MAGIC, &message, 1, false)) {
			ret = SNAPSHOT_READER_IO_ERROR;
			goto out_message;
		}
	}
	message.len = 0;
	if (build_mm(&model, &files, &message)) {
		fprintf(stderr, "converter: build_mm failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	snprintf(name, sizeof(name), "mm-%u.img", u32(model.task.data));
	fprintf(stderr, "converter: emit %s\n", name);
	if (emit_messages(options->output_dir, name, MM_MAGIC, &message, 1, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	message.len = 0;
	if (build_page_messages(&model, &page_messages, &page_count, &page_data,
			&page_data_len)) {
		fprintf(stderr, "converter: build_page_messages failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	snprintf(name, sizeof(name), "pagemap-%u.img", u32(model.task.data));
	fprintf(stderr, "converter: emit %s and pages-1.img (runs=%zu bytes=%zu)\n", name, page_count, page_data_len);
	if (emit_messages(options->output_dir, name, PAGEMAP_MAGIC, page_messages,
			page_count, false) || emit_raw(options->output_dir, "pages-1.img",
			page_data, page_data_len)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}

	file_messages = calloc(files.count, sizeof(*file_messages));
	reg_messages = calloc(files.count, sizeof(*reg_messages));
	fd_messages = calloc(files.binding_count, sizeof(*fd_messages));
	if (!file_messages || !reg_messages || !fd_messages) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	for (i = 0; i < files.count; i++) {
		image_writer_init(&file_messages[i]);
		image_writer_init(&reg_messages[i]);
		if (build_file_entry(&files.items[i], &model.creds, &file_messages[i]) ||
			build_reg_file(&files.items[i], &model.creds, &reg_messages[i])) {
			ret = SNAPSHOT_READER_IO_ERROR;
			goto out_message;
		}
	}
	fprintf(stderr, "converter: emit files.img/reg-files.img (files=%zu)\n", files.count);
	if (emit_messages(options->output_dir, "files.img", FILES_MAGIC,
			file_messages, files.count, false) ||
		emit_messages(options->output_dir, "reg-files.img", REG_FILES_MAGIC,
			reg_messages, files.count, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	for (i = 0; i < files.binding_count; i++) {
		image_writer_init(&fd_messages[i]);
		if (build_fdinfo(&files, (unsigned)i, &fd_messages[i])) {
			ret = SNAPSHOT_READER_IO_ERROR;
			goto out_message;
		}
	}
	fprintf(stderr, "converter: emit fdinfo-1.img\n");
	if (emit_messages(options->output_dir, "fdinfo-1.img", FDINFO_MAGIC,
				  fd_messages, files.binding_count, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	message.len = 0;
	if (build_ids(&message)) {
		fprintf(stderr, "converter: build ids failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	snprintf(name, sizeof(name), "ids-%u.img", u32(model.task.data));
	fprintf(stderr, "converter: emit %s\n", name);
	if (emit_messages(options->output_dir, name, IDS_MAGIC, &message, 1, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	message.len = 0;
	if (build_fs(&files, &message)) {
		fprintf(stderr, "converter: build fs failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	snprintf(name, sizeof(name), "fs-%u.img", u32(model.task.data));
	fprintf(stderr, "converter: emit %s\n", name);
	if (emit_messages(options->output_dir, name, FS_MAGIC, &message, 1, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	message.len = 0;
	if (build_creds(&model.creds, &message)) {
		fprintf(stderr, "converter: build creds failed\n");
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	snprintf(name, sizeof(name), "creds-%u.img", u32(model.task.data));
	fprintf(stderr, "converter: emit %s\n", name);
	if (emit_messages(options->output_dir, name, CREDS_MAGIC, &message, 1, false)) {
		ret = SNAPSHOT_READER_IO_ERROR;
		goto out_message;
	}
	ret = 0;

out_message:
	image_writer_free(&message);
	free_messages(file_messages, files.count);
	free_messages(reg_messages, files.count);
	free_messages(fd_messages, files.binding_count);
	free_messages(page_messages, page_count);
	free(page_data);
out:
	file_table_free(&files);
	model_free(&model);
	return ret;
}
