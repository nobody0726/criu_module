#ifndef B2_RST_SHARED_H
#define B2_RST_SHARED_H

#include <stddef.h>
#include <sys/types.h>

enum rst_task_state {
	RST_TASK_EMPTY = 0,
	RST_TASK_CREATED = 1,
	RST_TASK_READY = 2,
	RST_TASK_COMMITTED = 3,
	RST_TASK_FAILED = 4,
};

struct rst_shared;

int rst_shared_init(size_t task_count, struct rst_shared **out);
void rst_shared_destroy(struct rst_shared *shared);
int rst_shared_register_pid(struct rst_shared *shared, size_t index, pid_t pid);
int rst_shared_pid(const struct rst_shared *shared, size_t index, pid_t *pid);
int rst_shared_mark(struct rst_shared *shared, size_t index,
		    enum rst_task_state state);
int rst_shared_state(const struct rst_shared *shared, size_t index,
		     enum rst_task_state *state);
int rst_shared_wait_task_state(struct rst_shared *shared, size_t index,
			       enum rst_task_state expected,
			       unsigned timeout_ms);
int rst_shared_mark_pgid(struct rst_shared *shared, size_t index);
int rst_shared_wait_pgid(struct rst_shared *shared, size_t index,
			 unsigned timeout_ms);
int rst_shared_find_pid(const struct rst_shared *shared, pid_t pid,
			size_t *index);
size_t rst_shared_task_count(const struct rst_shared *shared);
int rst_shared_wait_commit(struct rst_shared *shared, unsigned timeout_ms);
int rst_shared_wait_count(struct rst_shared *shared, unsigned expected,
			  unsigned timeout_ms);
int rst_shared_wait_flag(struct rst_shared *shared, unsigned *word,
			 unsigned expected, unsigned timeout_ms);
int rst_shared_release_commit(struct rst_shared *shared);
int rst_shared_abort(struct rst_shared *shared, int error_code);
int rst_shared_is_aborted(const struct rst_shared *shared);
int rst_shared_error(const struct rst_shared *shared);

#endif
