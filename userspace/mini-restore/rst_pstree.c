#define _GNU_SOURCE

#include "rst_pstree.h"

#include "criu_wire.h"
#include "image_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMG_COMMON_MAGIC 0x54564319U
#define PSTREE_MAGIC 0x50273030U
#define RST_MAX_TASKS 4096U

struct rst_blob {
	uint8_t *data;
	size_t len;
};

static uint32_t read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static int read_blob(const char *path, struct rst_blob *blob)
{
	struct stat st;
	size_t off = 0;
	int fd;

	memset(blob, 0, sizeof(*blob));
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX) {
		close(fd);
		return -1;
	}
	blob->len = (size_t)st.st_size;
	blob->data = malloc(blob->len ? blob->len : 1U);
	if (!blob->data) {
		close(fd);
		return -1;
	}
	while (off < blob->len) {
		ssize_t got = read(fd, blob->data + off, blob->len - off);

		if (got <= 0) {
			free(blob->data);
			memset(blob, 0, sizeof(*blob));
			close(fd);
			return -1;
		}
		off += (size_t)got;
	}
	close(fd);
	return 0;
}

static int framed_header(const struct rst_blob *blob, size_t *off)
{
	if (blob->len < 8U ||
	    read_le32(blob->data) != IMG_COMMON_MAGIC ||
	    read_le32(blob->data + 4U) != PSTREE_MAGIC)
		return -1;
	*off = 8U;
	return 0;
}

static int next_record(const struct rst_blob *blob, size_t *off,
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

static struct rst_item *item_by_pid(struct rst_pstree *tree, pid_t pid)
{
	size_t i;

	for (i = 0; i < tree->count; i++)
		if (tree->items[i].pid == pid)
			return &tree->items[i];
	return NULL;
}

const struct rst_item *rst_find_item(const struct rst_pstree *tree, pid_t pid)
{
	size_t i;

	if (!tree)
		return NULL;
	for (i = 0; i < tree->count; i++)
		if (tree->items[i].pid == pid)
			return &tree->items[i];
	return NULL;
}

static enum b1_restore_status pstree_diag(struct b1_restore_image *diag,
					   enum b1_restore_status status,
					   const char *message)
{
	if (diag)
		b1_restore_set_diag(diag, status, message);
	return status;
}

static int parse_entry(const uint8_t *payload, size_t payload_len,
		       struct rst_item *item)
{
	struct b1_pb_cursor cursor = { payload, payload_len, 0 };
	struct b1_pb_field field;
	unsigned int thread_count = 0;
	int rc;

	memset(item, 0, sizeof(*item));
	item->born_sid = -1;
	while ((rc = b1_pb_next(&cursor, &field)) > 0) {
		uint32_t value;

		switch (field.number) {
		case 1:
			if (b1_pb_read_u32(&field, &value) || value == 0 ||
			    value > INT_MAX)
				return -1;
			item->pid = (pid_t)value;
			break;
		case 2:
			if (b1_pb_read_u32(&field, &value) || value > INT_MAX)
				return -1;
			item->ppid = (pid_t)value;
			break;
		case 3:
			if (b1_pb_read_u32(&field, &value) || value > INT_MAX)
				return -1;
			item->pgid = (pid_t)value;
			break;
		case 4:
			if (b1_pb_read_u32(&field, &value) || value > INT_MAX)
				return -1;
			item->sid = (pid_t)value;
			break;
		case 5:
			if (b1_pb_read_u32(&field, &value) || value == 0 ||
			    value > INT_MAX)
				return -1;
			thread_count++;
			if (thread_count != 1U || value != (uint32_t)item->pid)
				return -1;
			break;
		default:
			break;
		}
	}
	if (rc < 0 || item->pid <= 0 || item->ppid < 0 ||
	    item->pgid <= 0 || item->sid <= 0 || thread_count != 1U)
		return -1;
	if (item->pid == item->sid)
		item->flags |= RST_ITEM_SESSION_LEADER;
	if (item->pid == item->pgid)
		item->flags |= RST_ITEM_PGRP_LEADER;
	return 0;
}

static enum b1_restore_status derive_born_sid(struct rst_pstree *tree,
					      struct b1_restore_image *diag)
{
	size_t i;

	for (i = 0; i < tree->count; i++) {
		struct rst_item *item = &tree->items[i];
		struct rst_item *parent;

		if (item == tree->root || item->sid == item->pid)
			continue;
		parent = item->parent;
		if (!parent || parent->sid == item->sid)
			continue;
		while (parent && parent->pid != item->sid) {
			if (parent->born_sid != -1 &&
			    parent->born_sid != item->sid)
				return pstree_diag(diag, B1_RESTORE_UNSUPPORTED,
						   "conflicting derived born_sid");
			parent->born_sid = item->sid;
			parent = parent->parent;
		}
		if (!parent)
			return pstree_diag(diag, B1_RESTORE_UNSUPPORTED,
					   "session leader is outside restore tree");
	}
	return B1_RESTORE_OK;
}

static enum b1_restore_status validate_parent_chain(
	struct rst_pstree *tree, struct b1_restore_image *diag)
{
	size_t i;

	for (i = 0; i < tree->count; i++) {
		struct rst_item *slow = &tree->items[i];
		struct rst_item *fast = slow;
		unsigned int steps = 0;

		while (fast && fast->parent) {
			slow = slow->parent;
			fast = fast->parent->parent;
			if (++steps > tree->count)
				return pstree_diag(diag, B1_RESTORE_FORMAT,
						   "pstree parent chain overflow");
			if (slow == fast)
				return pstree_diag(diag, B1_RESTORE_FORMAT,
						   "pstree contains a cycle");
		}
	}
	return B1_RESTORE_OK;
}

enum b1_restore_status rst_read_pstree(const char *dir,
					struct rst_pstree *tree)
{
	char path[PATH_MAX];
	struct rst_blob blob;
	const uint8_t *payload;
	size_t payload_len;
	size_t off;
	size_t capacity = 0;
	int rc;

	if (!dir || !tree)
		return B1_RESTORE_FORMAT;
	memset(tree, 0, sizeof(*tree));
	if (snprintf(path, sizeof(path), "%s/pstree.img", dir) >=
	    (int)sizeof(path))
		return B1_RESTORE_IO;
	if (read_blob(path, &blob))
		return B1_RESTORE_IO;
	if (framed_header(&blob, &off)) {
		free(blob.data);
		return B1_RESTORE_FORMAT;
	}
	while ((rc = next_record(&blob, &off, &payload, &payload_len)) > 0) {
		struct rst_item *item;

		if (tree->count == RST_MAX_TASKS) {
			free(blob.data);
			rst_free_pstree(tree);
			return B1_RESTORE_UNSUPPORTED;
		}
		if (tree->count == capacity) {
			size_t new_capacity = capacity ? capacity * 2U : 8U;
			struct rst_item *new_items;

			if (new_capacity > RST_MAX_TASKS)
				new_capacity = RST_MAX_TASKS;
			new_items = realloc(tree->items,
					    new_capacity * sizeof(*new_items));
			if (!new_items) {
				free(blob.data);
				rst_free_pstree(tree);
				return B1_RESTORE_IO;
			}
			tree->items = new_items;
			capacity = new_capacity;
		}
		item = &tree->items[tree->count];
		if (parse_entry(payload, payload_len, item)) {
			free(blob.data);
			rst_free_pstree(tree);
			return B1_RESTORE_FORMAT;
		}
		item->index = tree->count++;
	}
	free(blob.data);
	if (rc < 0 || tree->count == 0)
		return B1_RESTORE_FORMAT;
	return B1_RESTORE_OK;
}

enum b1_restore_status rst_validate_pstree(const struct rst_pstree *const_tree,
					    struct b1_restore_image *diag)
{
	struct rst_pstree *tree = (struct rst_pstree *)(uintptr_t)const_tree;
	size_t i;
	unsigned int roots = 0;
	enum b1_restore_status st;

	if (!tree || !tree->items || !tree->count)
		return pstree_diag(diag, B1_RESTORE_FORMAT, "empty pstree");
	for (i = 0; i < tree->count; i++) {
		struct rst_item *item = &tree->items[i];

		if (item_by_pid(tree, item->pid) != item)
			return pstree_diag(diag, B1_RESTORE_FORMAT,
					   "duplicate pstree pid");
		if (item->ppid == 0) {
			roots++;
			tree->root = item;
			continue;
		}
		item->parent = item_by_pid(tree, item->ppid);
		if (!item->parent)
			return pstree_diag(diag, B1_RESTORE_FORMAT,
					   "pstree parent is missing");
		item->next_sibling = item->parent->children;
		item->parent->children = item;
	}
	if (roots != 1U)
		return pstree_diag(diag, B1_RESTORE_FORMAT,
				   "pstree must contain exactly one root");
	for (i = 0; i < tree->count; i++) {
		struct rst_item *item = &tree->items[i];
		struct rst_item *leader;

		if (item->sid == 0 || item->pgid == 0)
			return pstree_diag(diag, B1_RESTORE_FORMAT,
					   "pstree has zero session or group id");
		leader = item_by_pid(tree, item->sid);
		if (!leader)
			return pstree_diag(diag, B1_RESTORE_UNSUPPORTED,
					   "session leader is outside restore tree");
		leader = item_by_pid(tree, item->pgid);
		if (!leader)
			return pstree_diag(diag, B1_RESTORE_UNSUPPORTED,
					   "process-group leader is outside restore tree");
	}
	st = validate_parent_chain(tree, diag);
	if (st != B1_RESTORE_OK)
		return st;
	return derive_born_sid(tree, diag);
}

void rst_free_pstree(struct rst_pstree *tree)
{
	if (!tree)
		return;
	free(tree->items);
	memset(tree, 0, sizeof(*tree));
}

int rst_before_setsid(const struct rst_item *child)
{
	pid_t child_sid;

	if (!child || !child->parent)
		return 0;
	child_sid = child->born_sid == -1 ? child->sid : child->born_sid;
	return child->parent->born_sid == child_sid;
}
