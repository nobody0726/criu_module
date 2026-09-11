#ifndef CRIU_SNAPSHOT_H
#define CRIU_SNAPSHOT_H

/* A3 snapshot.bin on-disk ABI. All integer fields are little-endian. */
#include <stdint.h>

#if defined(__GNUC__)
#define CRIU_SNAPSHOT_PACKED __attribute__((packed))
#else
#define CRIU_SNAPSHOT_PACKED
#endif

#define CRIU_SNAPSHOT_MAGIC 0x43524955534e5033ULL /* "CRIUSNP3" */
#define CRIU_SNAPSHOT_VERSION 1U
#define CRIU_SNAPSHOT_HEADER_SIZE 64U
#define CRIU_SNAPSHOT_TLV_HEADER_SIZE 16U
#define CRIU_SNAPSHOT_FOOTER_SIZE 24U

#define CRIU_SNAPSHOT_MAX_RECORDS 65536U
#define CRIU_SNAPSHOT_MAX_RECORD_SIZE (64U * 1024U * 1024U)
#define CRIU_SNAPSHOT_MAX_TOTAL_SIZE (1024ULL * 1024ULL * 1024ULL)
#define CRIU_SNAPSHOT_HEADER_FLAGS 0U
#define CRIU_SNAPSHOT_TLV_FLAGS 0U
#define CRIU_SNAPSHOT_FOOTER_FLAGS 0U

/* The serialized ABI is little-endian; big-endian producers are unsupported. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "criu_snapshot.bin requires a little-endian producer"
#endif

enum criu_snapshot_record_type {
	CRIU_SNAPSHOT_REC_TASK = 1,
	CRIU_SNAPSHOT_REC_MM = 2,
	CRIU_SNAPSHOT_REC_VMA = 3,
	CRIU_SNAPSHOT_REC_REGS = 4,
	CRIU_SNAPSHOT_REC_FD = 5,
	CRIU_SNAPSHOT_REC_FS = 6,
	CRIU_SNAPSHOT_REC_CREDS = 7,
	CRIU_SNAPSHOT_REC_IDMAP = 8,
	CRIU_SNAPSHOT_REC_PAGE_RUN = 9,
	CRIU_SNAPSHOT_REC_END = 0xffff,
};

enum criu_snapshot_status {
	CRIU_SNAPSHOT_OK = 0,
	CRIU_SNAPSHOT_UNSUPPORTED = 1,
	CRIU_SNAPSHOT_INCONSISTENT = 2,
	CRIU_SNAPSHOT_IO_ERROR = 3,
	CRIU_SNAPSHOT_FORMAT_ERROR = 4,
};

/* Packed sizes are part of the ABI; do not use these as kernel structs. */
struct criu_snapshot_header {
	uint64_t magic;
	uint32_t version;
	uint16_t header_size;
	uint16_t flags;
	uint32_t arch;
	uint32_t page_size;
	uint32_t pid;
	uint32_t tgid;
	uint64_t freeze_generation;
	uint32_t record_count;
	uint32_t reserved;
	uint64_t total_size;
	uint64_t checksum;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_tlv {
	uint16_t type;
	uint16_t flags;
	uint32_t reserved;
	uint64_t length;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_footer {
	uint64_t magic;
	uint32_t version;
	uint32_t record_count;
	uint64_t checksum;
} CRIU_SNAPSHOT_PACKED;

#if defined(__cplusplus)
static_assert(sizeof(struct criu_snapshot_header) == CRIU_SNAPSHOT_HEADER_SIZE, "snapshot header ABI size");
static_assert(sizeof(struct criu_snapshot_tlv) == CRIU_SNAPSHOT_TLV_HEADER_SIZE, "snapshot TLV ABI size");
static_assert(sizeof(struct criu_snapshot_footer) == CRIU_SNAPSHOT_FOOTER_SIZE, "snapshot footer ABI size");
#else
_Static_assert(sizeof(struct criu_snapshot_header) == CRIU_SNAPSHOT_HEADER_SIZE, "snapshot header ABI size");
_Static_assert(sizeof(struct criu_snapshot_tlv) == CRIU_SNAPSHOT_TLV_HEADER_SIZE, "snapshot TLV ABI size");
_Static_assert(sizeof(struct criu_snapshot_footer) == CRIU_SNAPSHOT_FOOTER_SIZE, "snapshot footer ABI size");
#endif

/* Checksum covers header with checksum field zeroed followed by all TLVs; footer is excluded. */

#endif
