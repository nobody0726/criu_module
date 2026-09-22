#include "../../userspace/mini-restore/rst_pstree.h"
#include "../../userspace/mini-restore/image_reader.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	struct rst_pstree tree;
	struct b1_restore_image diag;
	enum b1_restore_status st;
	size_t i;

	if (argc != 2)
		return 2;
	b1_restore_image_init(&diag);
	st = rst_read_pstree(argv[1], &tree);
	if (st == B1_RESTORE_OK)
		st = rst_validate_pstree(&tree, &diag);
	if (st != B1_RESTORE_OK) {
		fprintf(stderr, "%s\n", diag.diagnostic);
		rst_free_pstree(&tree);
		return 1;
	}
	printf("root=%d count=%zu\n", tree.root->pid, tree.count);
	for (i = 0; i < tree.count; i++)
		printf("item=%d ppid=%d sid=%d pgid=%d born_sid=%d before_setsid=%d\n",
		       tree.items[i].pid, tree.items[i].ppid, tree.items[i].sid,
		       tree.items[i].pgid, tree.items[i].born_sid,
		       rst_before_setsid(&tree.items[i]));
	rst_free_pstree(&tree);
	return 0;
}
