#define _GNU_SOURCE

#include "rst_shared.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define RST_SHARED_MAGIC 0x42525348U
#define RST_SHARED_VERSION 1U
#define RST_SHARED_POLL_NS 1000000L

struct rst_shared {
	uint32_t magic;
	uint32_t version;
	uint32_t task_count;
	uint32_t ready_count;
	uint32_t commit_go;
	uint32_t abort;
	int32_t error_code;
	uint32_t map_len;
	uint32_t data[];
};

static uint32_t *state_words(struct rst_shared *shared)
{
	return shared->data + shared->task_count;
}

static uint32_t *pgid_words(struct rst_shared *shared)
{
	return shared->data;
}

static const uint32_t *const_state_words(const struct rst_shared *shared)
{
	return shared->data + shared->task_count;
}

static pid_t *pid_words(struct rst_shared *shared)
{
	return (pid_t *)(shared->data + shared->task_count * 2U);
}

static const pid_t *const_pid_words(const struct rst_shared *shared)
{
	return (const pid_t *)(shared->data + shared->task_count * 2U);
}

static uint32_t load_u32(const uint32_t *value)
{
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_u32(uint32_t *value, uint32_t new_value)
{
	__atomic_store_n(value, new_value, __ATOMIC_RELEASE);
}

static int wait_until(struct rst_shared *shared, uint32_t *word,
		      unsigned expected, unsigned timeout_ms)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -1;
	deadline.tv_sec += timeout_ms / 1000U;
	deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	for (;;) {
		struct timespec now;
		uint32_t value = load_u32(word);

		if (value >= expected)
			return 0;
		if (load_u32(&shared->abort))
			return -ECANCELED;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		     now.tv_nsec >= deadline.tv_nsec))
			return -ETIMEDOUT;
#if defined(__linux__)
		{
			struct timespec remain;
			long rc;

			remain.tv_sec = deadline.tv_sec - now.tv_sec;
			remain.tv_nsec = deadline.tv_nsec - now.tv_nsec;
			if (remain.tv_nsec < 0) {
				remain.tv_sec--;
				remain.tv_nsec += 1000000000L;
			}
			rc = syscall(SYS_futex, word, FUTEX_WAIT, value,
				     &remain, NULL, 0);
			if (rc < 0 && errno != EAGAIN && errno != EINTR)
				return -errno;
		}
#else
		{
			struct timespec pause = {
				.tv_sec = 0,
				.tv_nsec = RST_SHARED_POLL_NS,
			};

			(void)nanosleep(&pause, NULL);
		}
#endif
	}
}

static void wake_word(uint32_t *word)
{
#if defined(__linux__)
	(void)syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX,
		      NULL, NULL, 0);
#else
	(void)word;
#endif
}

int rst_shared_init(size_t task_count, struct rst_shared **out)
{
	size_t words;
	size_t length;
	struct rst_shared *shared;

	if (!out || task_count == 0 || task_count > UINT32_MAX / 3U)
		return -EINVAL;
	words = task_count * 3U;
	if (words > (SIZE_MAX - sizeof(*shared)) / sizeof(uint32_t))
		return -EOVERFLOW;
	length = sizeof(*shared) + words * sizeof(uint32_t);
	if (length > UINT32_MAX)
		return -EOVERFLOW;
	shared = mmap(NULL, length, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED)
		return -errno;
	memset(shared, 0, length);
	shared->magic = RST_SHARED_MAGIC;
	shared->version = RST_SHARED_VERSION;
	shared->task_count = (uint32_t)task_count;
	shared->map_len = (uint32_t)length;
	*out = shared;
	return 0;
}

void rst_shared_destroy(struct rst_shared *shared)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return;
	(void)munmap(shared, shared->map_len);
}

int rst_shared_register_pid(struct rst_shared *shared, size_t index, pid_t pid)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count || pid <= 0)
		return -EINVAL;
	pid_words(shared)[index] = pid;
	store_u32(&state_words(shared)[index], RST_TASK_CREATED);
	wake_word(&state_words(shared)[index]);
	return 0;
}

int rst_shared_pid(const struct rst_shared *shared, size_t index, pid_t *pid)
{
	if (!shared || !pid || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count)
		return -EINVAL;
	*pid = const_pid_words(shared)[index];
	return *pid > 0 ? 0 : -ESRCH;
}

int rst_shared_mark(struct rst_shared *shared, size_t index,
		    enum rst_task_state state)
{
	uint32_t old;

	if (!shared || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count || state > RST_TASK_FAILED)
		return -EINVAL;
	old = __atomic_exchange_n(&state_words(shared)[index], state,
				  __ATOMIC_ACQ_REL);
	if (state == RST_TASK_READY && old != RST_TASK_READY) {
		(void)__atomic_add_fetch(&shared->ready_count, 1U,
					 __ATOMIC_ACQ_REL);
		wake_word(&shared->ready_count);
	}
	wake_word(&state_words(shared)[index]);
	return 0;
}

int rst_shared_state(const struct rst_shared *shared, size_t index,
		     enum rst_task_state *state)
{
	if (!shared || !state || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count)
		return -EINVAL;
	*state = (enum rst_task_state)load_u32(&const_state_words(shared)[index]);
	return 0;
}

int rst_shared_wait_task_state(struct rst_shared *shared, size_t index,
			       enum rst_task_state expected,
			       unsigned timeout_ms)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count || expected > RST_TASK_FAILED)
		return -EINVAL;
	return wait_until(shared, &state_words(shared)[index],
			  (unsigned)expected, timeout_ms);
}

int rst_shared_mark_pgid(struct rst_shared *shared, size_t index)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count)
		return -EINVAL;
	store_u32(&pgid_words(shared)[index], 1U);
	wake_word(&pgid_words(shared)[index]);
	return 0;
}

int rst_shared_wait_pgid(struct rst_shared *shared, size_t index,
			 unsigned timeout_ms)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC ||
	    index >= shared->task_count)
		return -EINVAL;
	return wait_until(shared, &pgid_words(shared)[index], 1U, timeout_ms);
}

int rst_shared_find_pid(const struct rst_shared *shared, pid_t pid,
			size_t *index)
{
	size_t i;
	const pid_t *pids;

	if (!shared || !index || shared->magic != RST_SHARED_MAGIC || pid <= 0)
		return -EINVAL;
	pids = const_pid_words(shared);
	for (i = 0; i < shared->task_count; i++) {
		if (pids[i] == pid) {
			*index = i;
			return 0;
		}
	}
	return -ESRCH;
}

int rst_shared_wait_count(struct rst_shared *shared, unsigned expected,
			  unsigned timeout_ms)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC ||
	    expected > shared->task_count)
		return -EINVAL;
	return wait_until(shared, &shared->ready_count, expected, timeout_ms);
}

int rst_shared_wait_flag(struct rst_shared *shared, unsigned *word,
			 unsigned expected, unsigned timeout_ms)
{
	if (!shared || !word || shared->magic != RST_SHARED_MAGIC)
		return -EINVAL;
	return wait_until(shared, (uint32_t *)word, expected, timeout_ms);
}

int rst_shared_release_commit(struct rst_shared *shared)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return -EINVAL;
	store_u32(&shared->commit_go, 1U);
	wake_word(&shared->commit_go);
	return 0;
}

int rst_shared_abort(struct rst_shared *shared, int error_code)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return -EINVAL;
	shared->error_code = error_code;
	store_u32(&shared->abort, 1U);
	wake_word(&shared->abort);
	wake_word(&shared->ready_count);
	wake_word(&shared->commit_go);
	return 0;
}

int rst_shared_is_aborted(const struct rst_shared *shared)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return 1;
	return load_u32(&shared->abort) != 0;
}

size_t rst_shared_task_count(const struct rst_shared *shared)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return 0;
	return shared->task_count;
}

int rst_shared_wait_commit(struct rst_shared *shared, unsigned timeout_ms)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return -EINVAL;
	return wait_until(shared, &shared->commit_go, 1U, timeout_ms);
}

int rst_shared_error(const struct rst_shared *shared)
{
	if (!shared || shared->magic != RST_SHARED_MAGIC)
		return -EINVAL;
	return shared->error_code;
}
