/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_DUMP_H
#define CRIU_DUMP_H

#include <linux/types.h>

/* Run a complete synchronous A3 dump.  The path is a kernel NUL string. */
int criu_dump_process(pid_t vpid, const char *path);
int criu_dump_process_tree(pid_t vpid, const char *path);

#endif
