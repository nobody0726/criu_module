#include <stdint.h>
#include <stdio.h>

#include "criu_restore_abi.h"

#define PAGE_SIZE_U64 4096ULL

struct commit_case {
	const char *name;
	uint64_t staging_start;
	uint64_t target_start;
	uint64_t length;
	uint64_t expected_guard;
	int high_to_low;
};

static const struct commit_case commit_cases[] = {
	{
		.name = "non-overlap",
		.staging_start = 0x6000000000ULL,
		.target_start = 0x4000000000ULL,
		.length = PAGE_SIZE_U64,
		.expected_guard = 0,
		.high_to_low = 1,
	},
	{
		.name = "overlap-low-to-high",
		.staging_start = 0x5000000000ULL,
		.target_start = 0x5000001000ULL,
		.length = PAGE_SIZE_U64 * 2,
		.expected_guard = 0x5000002000ULL,
		.high_to_low = 0,
	},
	{
		.name = "overlap-high-to-low",
		.staging_start = 0x5000001000ULL,
		.target_start = 0x5000000000ULL,
		.length = PAGE_SIZE_U64 * 2,
		.expected_guard = 0x5000000000ULL,
		.high_to_low = 1,
	},
};

static int ranges_overlap(uint64_t a_start, uint64_t a_len,
			  uint64_t b_start, uint64_t b_len)
{
	uint64_t a_end = a_start + a_len;
	uint64_t b_end = b_start + b_len;

	return a_start < b_end && b_start < a_end;
}

int main(void)
{
	size_t i;
	int saw_non_overlap = 0;
	int saw_low_to_high = 0;
	int saw_high_to_low = 0;

	for (i = 0; i < sizeof(commit_cases) / sizeof(commit_cases[0]); i++) {
		const struct commit_case *c = &commit_cases[i];
		struct criu_restore_vma_v1 vma = {
			.staging_start = c->staging_start,
			.target_start = c->target_start,
			.length = c->length,
			.prot = 3,
			.map_flags = 2,
			.kind = CRIU_RESTORE_VMA_ANON_PRIVATE,
			.flags = 0,
		};

		if (vma.length == 0)
			return 1;
		if (!ranges_overlap(vma.staging_start, vma.length,
				    vma.target_start, vma.length)) {
			if (c->expected_guard != 0)
				return 2;
			saw_non_overlap = 1;
		} else {
			if (c->expected_guard == 0)
				return 3;
			if (c->high_to_low)
				saw_high_to_low = 1;
			else
				saw_low_to_high = 1;
		}
		printf("%s guard=0x%llx direction=%s\n", c->name,
		       (unsigned long long)c->expected_guard,
		       c->high_to_low ? "high-to-low" : "low-to-high");
	}

	if (!saw_non_overlap || !saw_low_to_high || !saw_high_to_low)
		return 4;
	return 0;
}
