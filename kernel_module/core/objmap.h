/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CRIU_OBJMAP_H
#define CRIU_OBJMAP_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct criu_objmap;

struct criu_objmap *criu_objmap_new(void);
void criu_objmap_free(struct criu_objmap *map);
u32 criu_objmap_get(struct criu_objmap *map, const void *obj, bool *is_new);

#endif
