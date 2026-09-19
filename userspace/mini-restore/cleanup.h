#ifndef B1_CLEANUP_H
#define B1_CLEANUP_H

#include <sys/types.h>

#include "carrier.h"
#include "staging.h"

struct b1_cleanup {
	struct b1_carrier_manager carriers;
	struct b1_staging_plan *staging;
	int restore_fd;
};

void b1_cleanup_init(struct b1_cleanup *cleanup);
void b1_cleanup_set_staging(struct b1_cleanup *cleanup,
			    struct b1_staging_plan *staging);
void b1_cleanup_set_restore_fd(struct b1_cleanup *cleanup, int fd);
enum b1_restore_status b1_cleanup_record_pid(struct b1_cleanup *cleanup,
					     pid_t pid,
					     struct b1_restore_image *diag);
void b1_cleanup_run(struct b1_cleanup *cleanup);

#endif
