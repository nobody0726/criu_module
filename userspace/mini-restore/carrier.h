#ifndef B1_CARRIER_H
#define B1_CARRIER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "restore.h"

typedef int (*b1_carrier_entry_fn)(void *arg);

struct b1_carrier_manager {
	pid_t *pids;
	size_t count;
	size_t capacity;
};

void b1_carrier_manager_init(struct b1_carrier_manager *manager);
void b1_carrier_manager_free(struct b1_carrier_manager *manager);
enum b1_restore_status b1_carrier_manager_record(struct b1_carrier_manager *manager,
						 pid_t pid,
						 struct b1_restore_image *diag);
void b1_carrier_manager_cleanup(struct b1_carrier_manager *manager);
enum b1_restore_status b1_create_exact_pid_carrier(struct b1_carrier_manager *manager,
						  pid_t target_pid,
						  uint64_t tls,
						  b1_carrier_entry_fn entry,
						  void *arg,
						  struct b1_restore_image *diag);
enum b1_restore_status b1_wait_carrier(pid_t pid, int *status,
				       struct b1_restore_image *diag);

#endif
