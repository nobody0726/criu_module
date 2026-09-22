#include "rst_fork.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	struct rst_item items[4] = {
		{ .pid = 1000, .ppid = 0, .sid = 1000, .pgid = 1000 },
		{ .pid = 1001, .ppid = 1000, .sid = 1001, .pgid = 1001 },
		{ .pid = 1002, .ppid = 1001, .sid = 1000, .pgid = 1000 },
		{ .pid = 1003, .ppid = 1000, .sid = 1000, .pgid = 1000 },
	};
	struct rst_pstree tree = {
		.items = items,
		.count = 4,
		.root = &items[0],
	};
	const struct rst_item *order[4];
	size_t first_count = 0;
	size_t i;

	items[1].parent = &items[0];
	items[2].parent = &items[1];
	items[3].parent = &items[0];
	items[0].children = &items[1];
	items[1].next_sibling = &items[3];
	items[1].children = &items[2];
	items[1].born_sid = 1000;
	items[2].born_sid = -1;
	items[3].born_sid = -1;
	if (rst_build_fork_order(&tree, order, 4, &first_count) != 0)
		return 1;
	printf("first=%zu order=", first_count);
	for (i = 0; i < tree.count; i++)
		printf("%s%d", i ? "," : "", order[i]->pid);
	printf("\n");
	return 0;
}
