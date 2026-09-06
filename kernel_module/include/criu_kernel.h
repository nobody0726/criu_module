/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_KERNEL_H
#define CRIU_KERNEL_H

#include <linux/sched.h>
#include <linux/types.h>

#define CRIU_PATH_MAX 512
#define CRIU_SNAPSHOT_MAX (16 * 1024 * 1024)

enum criu_vma_class {
	CRIU_VMA_ANON_PRIVATE,
	CRIU_VMA_ANON_SHARED,
	CRIU_VMA_FILE_SHARED,
	CRIU_VMA_FILE_PRIVATE,
	CRIU_VMA_UNSUPPORTED,
};

enum criu_vma_special {
	CRIU_VMA_SPECIAL_NONE,
	CRIU_VMA_SPECIAL_PROT_NONE,
	CRIU_VMA_SPECIAL_VDSO,
	CRIU_VMA_SPECIAL_VVAR,
	CRIU_VMA_SPECIAL_HUGETLB,
	CRIU_VMA_SPECIAL_DEVICE,
	CRIU_VMA_SPECIAL_PFNMAP,
	CRIU_VMA_SPECIAL_MIXEDMAP,
	CRIU_VMA_SPECIAL_UNKNOWN,
};

enum criu_path_status {
	CRIU_PATH_OK,
	CRIU_PATH_TRUNCATED,
	CRIU_PATH_ERROR,
};

enum criu_sample_status {
	CRIU_SAMPLE_SKIPPED,
	CRIU_SAMPLE_OK,
	CRIU_SAMPLE_UNAVAILABLE,
};

struct criu_vma_info {
	unsigned long start, end, pgoff;
	unsigned long vm_flags_raw;
	unsigned int prot;
	enum criu_vma_class class;
	enum criu_vma_special special;
	bool shared, growsdown, dontdump, locked;
	dev_t dev;
	unsigned long ino;
	enum criu_path_status path_status;
	enum criu_sample_status sample_status;
	u8 sample;
	char path[CRIU_PATH_MAX];
};

struct criu_mm_info {
	pid_t pid, tgid;
	char comm[TASK_COMM_LEN];
	unsigned long state;
	unsigned long vma_count, special_count, unsupported_count;
	unsigned long total_vm;
	unsigned long start_code, end_code;
	unsigned long start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end;
	unsigned long env_start, env_end;
};

struct criu_snapshot {
	struct criu_mm_info mm;
	struct criu_vma_info *vmas;
};

struct criu_freeze_ctx;

typedef int (*criu_vma_info_fn)(const struct criu_vma_info *info, void *arg);

int criu_target_set(pid_t pid);
struct task_struct *criu_target_get(u64 *generation);
void criu_target_clear(void);
bool criu_freeze_context_active(void);
int criu_freeze(pid_t vpid, bool include_children,
		struct criu_freeze_ctx **ctx);
void criu_thaw(struct criu_freeze_ctx *ctx);
bool criu_freeze_settled(struct criu_freeze_ctx *ctx);
int criu_collect_mm_info(struct task_struct *task, struct criu_mm_info *out);
int criu_walk_vmas(struct task_struct *task, criu_vma_info_fn fn, void *arg);
int criu_snapshot_capture(struct task_struct *task, struct criu_snapshot *out,
			  bool samples);
void criu_snapshot_destroy(struct criu_snapshot *snapshot);

#endif /* CRIU_KERNEL_H */
