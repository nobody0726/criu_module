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
struct criu_objmap;
struct file;

struct criu_freeze_process_view {
	struct task_struct *leader;
	struct task_struct *parent;
	pid_t pid;
	pid_t tgid;
	pid_t ppid;
	pid_t pgid;
	pid_t sid;
	pid_t born_sid;
	bool root;
	bool external_parent;
	bool session_leader;
	bool process_group_leader;
	unsigned int task_count;
};

/* Borrowed view into the A2-pinned task set. The task reference remains owned
 * by the freeze context; callers must not retain this view past criu_thaw(). */
struct criu_freeze_task_view {
	struct task_struct *task;
	pid_t tid;
	bool stopped;
};

struct criu_freeze_status {
	char state[16];
	u64 generation;
	unsigned int task_count;
	bool settled;
	bool was_stopped;
	int last_error;
	char original_cgroup[CRIU_PATH_MAX];
	char temporary_cgroup[CRIU_PATH_MAX];
};

typedef int (*criu_vma_info_fn)(const struct criu_vma_info *info, void *arg);
typedef int (*criu_fd_fn)(unsigned int fd, struct file *file,
			unsigned int fd_flags, void *arg);

struct criu_objmap *criu_objmap_new(void);
void criu_objmap_free(struct criu_objmap *map);
u32 criu_objmap_get(struct criu_objmap *map, const void *obj, bool *is_new);
int criu_walk_fds(struct task_struct *task, criu_fd_fn fn, void *arg);

int criu_target_set(pid_t pid);
struct task_struct *criu_target_get(u64 *generation);
void criu_target_clear(void);
bool criu_freeze_context_active(void);
int criu_freeze(pid_t vpid, bool include_children,
		struct criu_freeze_ctx **ctx);
int criu_thaw(struct criu_freeze_ctx *ctx);
bool criu_freeze_settled(struct criu_freeze_ctx *ctx);
int criu_freeze_task_count(struct criu_freeze_ctx *ctx,
			   unsigned int *count);
int criu_freeze_task_get(struct criu_freeze_ctx *ctx, unsigned int index,
			 struct criu_freeze_task_view *view);
int criu_freeze_generation(struct criu_freeze_ctx *ctx, u64 *generation);
int criu_freeze_process_count(struct criu_freeze_ctx *ctx,
			      unsigned int *count);
int criu_freeze_process_get(struct criu_freeze_ctx *ctx,
			    unsigned int process_index,
			    struct criu_freeze_process_view *view);
int criu_freeze_process_task_count(struct criu_freeze_ctx *ctx,
				   unsigned int process_index,
				   unsigned int *count);
int criu_freeze_process_task_get(struct criu_freeze_ctx *ctx,
				 unsigned int process_index,
				 unsigned int task_index,
				 struct criu_freeze_task_view *view);
int criu_freeze_status(struct criu_freeze_status *out);
int criu_collect_mm_info(struct task_struct *task, struct criu_mm_info *out);
int criu_walk_vmas(struct task_struct *task, criu_vma_info_fn fn, void *arg);
int criu_snapshot_capture(struct task_struct *task, struct criu_snapshot *out,
			  bool samples);
void criu_snapshot_destroy(struct criu_snapshot *snapshot);

#endif /* CRIU_KERNEL_H */
