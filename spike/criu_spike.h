/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_SPIKE_H
#define CRIU_SPIKE_H

#include <linux/types.h>

#ifndef CRIU_SPIKE_PROBE
#define CRIU_SPIKE_PROBE "2.2"
#endif

#define CRIU_SPIKE_PROBE_LEN 8
#define CRIU_SPIKE_STATUS_MAX 128
#define CRIU_SPIKE_REPORT_MAX 1024
#define CRIU_SPIKE_VMAS_MAX 8192

struct criu_spike_args {
	int target_pid;
	unsigned long anon_addr;
	unsigned long shared_addr;
	unsigned long guard_addr;
	unsigned long insert_addr;
	char probe[CRIU_SPIKE_PROBE_LEN];
};

extern struct criu_spike_args criu_spike_args;

#endif
