#ifndef B1_MINI_RESTORE_H
#define B1_MINI_RESTORE_H

#include <stddef.h>
#include <stdint.h>

#define B1_RESTORE_PAGE_SIZE 4096ULL
#define B1_RESTORE_MAX_VMAS 4096U
#define B1_RESTORE_MAX_PAGE_RUNS 65536U

enum b1_restore_status {
	B1_RESTORE_OK = 0,
	B1_RESTORE_FORMAT,
	B1_RESTORE_UNSUPPORTED,
	B1_RESTORE_IO,
};

enum b1_vma_kind {
	B1_VMA_ANON_PRIVATE = 1,
	B1_VMA_FILE_PRIVATE = 2,
	B1_VMA_STACK = 3,
	B1_VMA_VDSO = 4,
};

struct b1_vma_record {
	uint64_t start;
	uint64_t length;
	uint64_t pgoff;
	uint64_t shmid;
	uint32_t prot;
	uint32_t map_flags;
	enum b1_vma_kind kind;
	int shared;
	int dirty_file_private;
	int file_stable;
	int backing_fd;
	uint64_t file_size;
};

struct b1_page_run {
	uint64_t addr;
	uint64_t pages;
	uint64_t image_offset;
	char image[64];
};

struct b1_restore_image {
	char diagnostic[160];
	uint32_t target_pid;
	uint64_t tls;
	uint64_t sigmask;
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint8_t vregs[32][16];
	uint32_t fpsr;
	uint32_t fpcr;
	int have_core;
	int have_mm;
	int have_pagemap;
	int arch_aarch64;
	int tasks;
	int threads;
	int children;
	int namespaces;
	int shared_mm;
	int shared_mappings;
	int vdso_reloc;
	int pac;
	int sve;
	int gcs;
	size_t vma_count;
	struct b1_vma_record *vmas;
	size_t page_run_count;
	struct b1_page_run *page_runs;
};

const char *b1_restore_status_name(enum b1_restore_status status);
void b1_restore_set_diag(struct b1_restore_image *image,
			 enum b1_restore_status status, const char *message);

#endif
