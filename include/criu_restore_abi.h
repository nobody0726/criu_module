#ifndef CRIU_RESTORE_ABI_H
#define CRIU_RESTORE_ABI_H

/* B1 /dev/criu_restore UAPI. No CRIU protobuf reaches the kernel. */
#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#if defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif
#endif

#if defined(__GNUC__)
#define CRIU_RESTORE_PACKED __attribute__((packed))
#else
#define CRIU_RESTORE_PACKED
#endif

#define CRIU_RESTORE_ABI_VERSION 1U
#define CRIU_RESTORE_MAX_VMAS 4096U
#define CRIU_RESTORE_VMA_RECORD_SIZE 40U
#define CRIU_RESTORE_VALIDATE_V1_SIZE 104U
#define CRIU_RESTORE_COMMIT_V1_SIZE 16U

#define CRIU_RESTORE_PLAN_F_NONE 0U
#define CRIU_RESTORE_PLAN_F_VDSO_SAME_ADDRESS (1U << 0)
#define CRIU_RESTORE_PLAN_F_KNOWN \
	(CRIU_RESTORE_PLAN_F_VDSO_SAME_ADDRESS)

#define CRIU_RESTORE_VMA_F_NONE 0U
#define CRIU_RESTORE_VMA_F_GROWSDOWN (1U << 0)
#define CRIU_RESTORE_VMA_F_FILE_CLEAN (1U << 1)
#define CRIU_RESTORE_VMA_F_STAGING_WRITABLE (1U << 2)
#define CRIU_RESTORE_VMA_F_KNOWN \
	(CRIU_RESTORE_VMA_F_GROWSDOWN | CRIU_RESTORE_VMA_F_FILE_CLEAN | \
	 CRIU_RESTORE_VMA_F_STAGING_WRITABLE)

enum criu_restore_vma_kind {
	CRIU_RESTORE_VMA_ANON_PRIVATE = 1,
	CRIU_RESTORE_VMA_FILE_PRIVATE = 2,
	CRIU_RESTORE_VMA_STACK = 3,
	CRIU_RESTORE_VMA_VDSO = 4,
};

/* A VMA is copied from vmas_user_ptr while VALIDATE is running. */
struct criu_restore_vma_v1 {
	__u64 staging_start;
	__u64 target_start;
	__u64 length;
	__u32 prot;
	__u32 map_flags;
	__u32 kind;
	__u32 flags;
} CRIU_RESTORE_PACKED;

/*
 * VALIDATE input. vmas_user_ptr is VALIDATE only; the kernel copies the
 * fixed-width plan and VMA array into the transaction before returning.
 */
struct criu_restore_plan_v1 {
	__u32 version;
	__u32 size;
	__u32 vma_count;
	__u32 flags;
	__u32 target_pid;
	__u32 reserved0;
	__u64 vmas_user_ptr;
	__u64 bootstrap_code_start;
	__u64 bootstrap_code_end;
	__u64 bootstrap_pc;
	__u64 bootstrap_stack_start;
	__u64 bootstrap_stack_end;
	__u64 bootstrap_sp;
	__u64 sigframe_staging_sp;
	__u64 sigframe_final_sp;
	__u64 tls;
} CRIU_RESTORE_PACKED;

/*
 * COMMIT input. It deliberately contains no address, VMA, or user pointer:
 * COMMIT must not dereference user memory. The kernel operates only on the
 * transaction-owned plan copied by VALIDATE.
 */
struct criu_restore_commit_v1 {
	__u32 version;
	__u32 size;
	__u32 flags;
	__u32 reserved0;
} CRIU_RESTORE_PACKED;

#define CRIU_RESTORE_IOC_MAGIC 'R'
#define CRIU_RESTORE_VALIDATE_V1 \
	_IOW(CRIU_RESTORE_IOC_MAGIC, 0x01, struct criu_restore_plan_v1)
#define CRIU_RESTORE_COMMIT_V1 \
	_IOW(CRIU_RESTORE_IOC_MAGIC, 0x02, struct criu_restore_commit_v1)

#ifdef __cplusplus
static_assert(sizeof(struct criu_restore_vma_v1) == CRIU_RESTORE_VMA_RECORD_SIZE,
	      "restore VMA ABI size");
static_assert(sizeof(struct criu_restore_plan_v1) == CRIU_RESTORE_VALIDATE_V1_SIZE,
	      "restore plan ABI size");
static_assert(sizeof(struct criu_restore_commit_v1) == CRIU_RESTORE_COMMIT_V1_SIZE,
	      "restore commit ABI size");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct criu_restore_vma_v1) == CRIU_RESTORE_VMA_RECORD_SIZE,
	       "restore VMA ABI size");
_Static_assert(sizeof(struct criu_restore_plan_v1) == CRIU_RESTORE_VALIDATE_V1_SIZE,
	       "restore plan ABI size");
_Static_assert(sizeof(struct criu_restore_commit_v1) == CRIU_RESTORE_COMMIT_V1_SIZE,
	       "restore commit ABI size");
#endif

#endif /* CRIU_RESTORE_ABI_H */
