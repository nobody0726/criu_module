/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/slab.h>

#include "objmap.h"

struct criu_objmap_entry {
	struct list_head list;
	const void *obj;
	u32 id;
};

struct criu_objmap {
	struct mutex lock;
	struct list_head entries;
	u32 next_id;
};

struct criu_objmap *criu_objmap_new(void)
{
	struct criu_objmap *map;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return NULL;
	mutex_init(&map->lock);
	INIT_LIST_HEAD(&map->entries);
	map->next_id = 1;
	return map;
}

void criu_objmap_free(struct criu_objmap *map)
{
	struct criu_objmap_entry *entry, *tmp;

	if (!map)
		return;
	list_for_each_entry_safe(entry, tmp, &map->entries, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	kfree(map);
}

u32 criu_objmap_get(struct criu_objmap *map, const void *obj, bool *is_new)
{
	struct criu_objmap_entry *entry;

	if (is_new)
		*is_new = false;
	if (!map || !obj)
		return 0;
	mutex_lock(&map->lock);
	list_for_each_entry(entry, &map->entries, list) {
		if (entry->obj == obj) {
			mutex_unlock(&map->lock);
			return entry->id;
		}
	}
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry || !map->next_id) {
		kfree(entry);
		mutex_unlock(&map->lock);
		return 0;
	}
	entry->obj = obj;
	entry->id = map->next_id++;
	list_add_tail(&entry->list, &map->entries);
	if (is_new)
		*is_new = true;
	mutex_unlock(&map->lock);
	return entry->id;
}
