#ifndef CRIU_SNAPSHOT_H
#define CRIU_SNAPSHOT_H

/* A3 snapshot.bin on-disk ABI. All integer fields are little-endian. */
#ifdef __KERNEL__
#include <linux/types.h>
typedef __u8 uint8_t;
typedef __u16 uint16_t;
typedef __u32 uint32_t;
typedef __u64 uint64_t;
#else
#include <stdint.h>
#endif

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
#define CRIU_SNAPSHOT_THREAD_REG_BYTES 512U
#define CRIU_SNAPSHOT_FD_RECORD_SIZE 560U
#define CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE 576U
#define CRIU_SNAPSHOT_PIPE_ENDPOINT_RECORD_SIZE 32U
#define CRIU_SNAPSHOT_PIPE_DATA_HEADER_SIZE 32U
#define CRIU_SNAPSHOT_UNIX_SOCKET_RECORD_SIZE 48U
#define CRIU_SNAPSHOT_SOCKET_QUEUE_HEADER_SIZE 32U
#define CRIU_SNAPSHOT_PIPE_ENDPOINT_VERSION 1U
#define CRIU_SNAPSHOT_PIPE_DATA_VERSION 1U
#define CRIU_SNAPSHOT_UNIX_SOCKET_VERSION 1U
#define CRIU_SNAPSHOT_SOCKET_QUEUE_VERSION 1U
#define CRIU_SNAPSHOT_HEADER_FLAGS 0U
#define CRIU_SNAPSHOT_F_SIGNAL_TIMERS (1U << 0)
#define CRIU_SNAPSHOT_F_PSTREE (1U << 1)
#define CRIU_SNAPSHOT_HEADER_KNOWN_FLAGS \
	(CRIU_SNAPSHOT_F_SIGNAL_TIMERS | CRIU_SNAPSHOT_F_PSTREE)
#define CRIU_SNAPSHOT_TLV_FLAGS 0U
#define CRIU_SNAPSHOT_TLV_F_PROCESS_SCOPE (1U << 0)
#define CRIU_SNAPSHOT_FOOTER_FLAGS 0U

#define CRIU_SNAPSHOT_SIGACTION_VERSION 1U
#define CRIU_SNAPSHOT_SIGNAL_QUEUE_VERSION 1U
#define CRIU_SNAPSHOT_ITIMERS_VERSION 1U
#define CRIU_SNAPSHOT_POSIX_TIMERS_VERSION 1U
#define CRIU_SNAPSHOT_SIGINFO_SIZE 128U
#define CRIU_SNAPSHOT_SIGACTION_ENTRY_SIZE 48U
#define CRIU_SNAPSHOT_SIGACTION_COUNT 64U
#define CRIU_SNAPSHOT_SIGACTION_HEADER_SIZE 16U
#define CRIU_SNAPSHOT_SIGNAL_QUEUE_ENTRY_SIZE 136U
#define CRIU_SNAPSHOT_SIGNAL_QUEUE_HEADER_SIZE 48U
#define CRIU_SNAPSHOT_ITIMER_ENTRY_SIZE 24U
#define CRIU_SNAPSHOT_ITIMER_COUNT 3U
#define CRIU_SNAPSHOT_ITIMER_HEADER_SIZE 16U
#define CRIU_SNAPSHOT_POSIX_TIMER_ENTRY_SIZE 56U
#define CRIU_SNAPSHOT_POSIX_TIMER_HEADER_SIZE 16U
#define CRIU_SNAPSHOT_PSTREE_VERSION 1U
#define CRIU_SNAPSHOT_PSTREE_RECORD_SIZE 48U
#define CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE 8U

#define CRIU_SNAPSHOT_PSTREE_F_ROOT (1U << 0)
#define CRIU_SNAPSHOT_PSTREE_F_EXTERNAL_PARENT (1U << 1)
#define CRIU_SNAPSHOT_PSTREE_F_SESSION_LEADER (1U << 2)
#define CRIU_SNAPSHOT_PSTREE_F_PGRP_LEADER (1U << 3)
#define CRIU_SNAPSHOT_PSTREE_NAMESPACE_CURRENT 1U

#define CRIU_SNAPSHOT_SIGNAL_SCOPE_SHARED 1U
#define CRIU_SNAPSHOT_SIGNAL_SCOPE_PRIVATE 2U
#define CRIU_SNAPSHOT_ITIMER_REAL 1U
#define CRIU_SNAPSHOT_ITIMER_VIRTUAL 2U
#define CRIU_SNAPSHOT_ITIMER_PROF 3U
#define CRIU_SNAPSHOT_POSIX_TIMER_F_ARMED (1U << 0)
#define CRIU_SNAPSHOT_POSIX_TIMER_F_HAS_NOTIFY_TID (1U << 1)

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
	CRIU_SNAPSHOT_REC_THREAD = 10,
	CRIU_SNAPSHOT_REC_PIPE_ENDPOINT = 11,
	CRIU_SNAPSHOT_REC_PIPE_DATA = 12,
	CRIU_SNAPSHOT_REC_UNIX_SOCKET = 13,
	CRIU_SNAPSHOT_REC_SOCKET_QUEUE = 14,
	CRIU_SNAPSHOT_REC_SIGACTION = 15,
	CRIU_SNAPSHOT_REC_SIGNAL_QUEUE = 16,
	CRIU_SNAPSHOT_REC_ITIMERS = 17,
	CRIU_SNAPSHOT_REC_POSIX_TIMERS = 18,
	CRIU_SNAPSHOT_REC_PSTREE = 19,
	CRIU_SNAPSHOT_REC_END = 0xffff,
};

enum criu_snapshot_status {
	CRIU_SNAPSHOT_OK = 0,
	CRIU_SNAPSHOT_UNSUPPORTED = 1,
	CRIU_SNAPSHOT_INCONSISTENT = 2,
	CRIU_SNAPSHOT_IO_ERROR = 3,
	CRIU_SNAPSHOT_FORMAT_ERROR = 4,
};

enum criu_snapshot_fd_type {
	CRIU_FD_TYPE_REG = 1,
	CRIU_FD_TYPE_PIPE = 2,
	CRIU_FD_TYPE_UNIX = 3,
	CRIU_FD_TYPE_FIFO = 4,
};

enum criu_snapshot_pipe_direction {
	CRIU_PIPE_DIRECTION_READ = 1,
	CRIU_PIPE_DIRECTION_WRITE = 2,
};

enum criu_snapshot_pipe_flags {
	CRIU_PIPE_FLAG_WRITE_CLOSED = 1U << 0,
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

/* Per-thread state is emitted once for each member of the thread group. */
struct criu_snapshot_thread_record {
	uint32_t tid;
	uint32_t tgid;
	uint32_t regs_size;
	uint32_t reserved;
	uint64_t tls;
	uint8_t blocked[8];
	uint8_t regs[CRIU_SNAPSHOT_THREAD_REG_BYTES];
} CRIU_SNAPSHOT_PACKED;

/* A5 appends metadata after the A3 path so 560-byte records stay readable. */
struct criu_snapshot_fd_record {
	uint32_t fd;
	uint32_t mode;
	uint64_t flags;
	uint64_t pos;
	uint64_t dev;
	uint64_t ino;
	uint64_t size;
	char path[512];
	uint64_t object_id;
	uint32_t type;
	/* Historical field name; these are descriptor flags, not struct file flags. */
	uint32_t object_flags;
} CRIU_SNAPSHOT_PACKED;

#define CRIU_FD_FLAG_CLOEXEC 1U

/* Pipe endpoint objects are distinct struct file instances referring to one
 * shared pipe_id. Pipe bytes follow a fixed pipe-data header in the TLV. */
struct criu_snapshot_pipe_endpoint_record {
	uint32_t version;
	uint32_t flags;
	uint64_t object_id;
	uint64_t pipe_id;
	uint32_t direction;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_pipe_data_record {
	uint32_t version;
	uint32_t flags;
	uint64_t pipe_id;
	uint64_t capacity;
	uint32_t data_len;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

/* Only connected AF_UNIX/SOCK_STREAM objects are representable in A5.
 * Receive-queue bytes follow a fixed socket-queue header in the TLV. */
struct criu_snapshot_unix_socket_record {
	uint32_t version;
	uint32_t flags;
	uint64_t object_id;
	uint64_t peer_object_id;
	uint32_t family;
	uint32_t socket_type;
	uint32_t state;
	uint32_t shutdown;
	uint64_t options;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_socket_queue_record {
	uint32_t version;
	uint32_t flags;
	uint64_t object_id;
	uint32_t data_len;
	uint32_t n_scm;
	uint64_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_sigaction_header {
	uint32_t version;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_sigaction_entry {
	uint32_t signo;
	uint32_t reserved;
	uint64_t handler;
	uint64_t flags;
	uint64_t restorer;
	uint64_t mask;
	uint64_t mask_extended;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_signal_queue_header {
	uint32_t version;
	uint32_t scope;
	uint32_t owner_tid;
	uint32_t total_count;
	uint32_t first_index;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t siginfo_size;
	uint64_t pending_mask;
	uint64_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_signal_queue_entry {
	uint32_t signo;
	uint32_t reserved;
	uint8_t siginfo[CRIU_SNAPSHOT_SIGINFO_SIZE];
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_itimers_header {
	uint32_t version;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_itimer_entry {
	uint32_t kind;
	uint32_t flags;
	uint64_t interval_ns;
	uint64_t remaining_ns;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_posix_timers_header {
	uint32_t version;
	uint32_t entry_count;
	uint32_t entry_size;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_posix_timer_entry {
	uint32_t timer_id;
	uint32_t clock_id;
	uint32_t signo;
	uint32_t sigev_notify;
	uint32_t flags;
	uint32_t overrun;
	uint32_t notify_tid;
	uint32_t reserved;
	uint64_t sival_ptr;
	uint64_t interval_ns;
	uint64_t remaining_ns;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_pstree_record {
	uint32_t version;
	uint32_t flags;
	uint32_t pid;
	uint32_t tgid;
	uint32_t ppid;
	uint32_t pgid;
	uint32_t sid;
	int32_t born_sid;
	uint32_t leader_pid;
	uint32_t thread_count;
	uint32_t namespace_scope;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

struct criu_snapshot_process_scope {
	uint32_t owner_pid;
	uint32_t reserved;
} CRIU_SNAPSHOT_PACKED;

#if defined(__cplusplus)
static_assert(sizeof(struct criu_snapshot_header) == CRIU_SNAPSHOT_HEADER_SIZE, "snapshot header ABI size");
static_assert(sizeof(struct criu_snapshot_tlv) == CRIU_SNAPSHOT_TLV_HEADER_SIZE, "snapshot TLV ABI size");
static_assert(sizeof(struct criu_snapshot_footer) == CRIU_SNAPSHOT_FOOTER_SIZE, "snapshot footer ABI size");
static_assert(sizeof(struct criu_snapshot_fd_record) == CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE, "snapshot fd ABI size");
static_assert(sizeof(struct criu_snapshot_pipe_endpoint_record) == CRIU_SNAPSHOT_PIPE_ENDPOINT_RECORD_SIZE, "snapshot pipe endpoint ABI size");
static_assert(sizeof(struct criu_snapshot_pipe_data_record) == CRIU_SNAPSHOT_PIPE_DATA_HEADER_SIZE, "snapshot pipe data ABI size");
static_assert(sizeof(struct criu_snapshot_unix_socket_record) == CRIU_SNAPSHOT_UNIX_SOCKET_RECORD_SIZE, "snapshot unix socket ABI size");
static_assert(sizeof(struct criu_snapshot_socket_queue_record) == CRIU_SNAPSHOT_SOCKET_QUEUE_HEADER_SIZE, "snapshot socket queue ABI size");
static_assert(sizeof(struct criu_snapshot_sigaction_header) == CRIU_SNAPSHOT_SIGACTION_HEADER_SIZE, "snapshot sigaction header ABI size");
static_assert(sizeof(struct criu_snapshot_sigaction_entry) == CRIU_SNAPSHOT_SIGACTION_ENTRY_SIZE, "snapshot sigaction entry ABI size");
static_assert(sizeof(struct criu_snapshot_signal_queue_header) == CRIU_SNAPSHOT_SIGNAL_QUEUE_HEADER_SIZE, "snapshot signal queue header ABI size");
static_assert(sizeof(struct criu_snapshot_signal_queue_entry) == CRIU_SNAPSHOT_SIGNAL_QUEUE_ENTRY_SIZE, "snapshot signal queue entry ABI size");
static_assert(sizeof(struct criu_snapshot_itimers_header) == CRIU_SNAPSHOT_ITIMER_HEADER_SIZE, "snapshot itimer header ABI size");
static_assert(sizeof(struct criu_snapshot_itimer_entry) == CRIU_SNAPSHOT_ITIMER_ENTRY_SIZE, "snapshot itimer entry ABI size");
static_assert(sizeof(struct criu_snapshot_posix_timers_header) == CRIU_SNAPSHOT_POSIX_TIMER_HEADER_SIZE, "snapshot posix timer header ABI size");
static_assert(sizeof(struct criu_snapshot_posix_timer_entry) == CRIU_SNAPSHOT_POSIX_TIMER_ENTRY_SIZE, "snapshot posix timer entry ABI size");
static_assert(sizeof(struct criu_snapshot_pstree_record) == CRIU_SNAPSHOT_PSTREE_RECORD_SIZE, "snapshot pstree ABI size");
static_assert(sizeof(struct criu_snapshot_process_scope) == CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE, "snapshot process scope ABI size");
#else
_Static_assert(sizeof(struct criu_snapshot_header) == CRIU_SNAPSHOT_HEADER_SIZE, "snapshot header ABI size");
_Static_assert(sizeof(struct criu_snapshot_tlv) == CRIU_SNAPSHOT_TLV_HEADER_SIZE, "snapshot TLV ABI size");
_Static_assert(sizeof(struct criu_snapshot_footer) == CRIU_SNAPSHOT_FOOTER_SIZE, "snapshot footer ABI size");
_Static_assert(sizeof(struct criu_snapshot_fd_record) == CRIU_SNAPSHOT_FD_EXT_RECORD_SIZE, "snapshot fd ABI size");
_Static_assert(sizeof(struct criu_snapshot_pipe_endpoint_record) == CRIU_SNAPSHOT_PIPE_ENDPOINT_RECORD_SIZE, "snapshot pipe endpoint ABI size");
_Static_assert(sizeof(struct criu_snapshot_pipe_data_record) == CRIU_SNAPSHOT_PIPE_DATA_HEADER_SIZE, "snapshot pipe data ABI size");
_Static_assert(sizeof(struct criu_snapshot_unix_socket_record) == CRIU_SNAPSHOT_UNIX_SOCKET_RECORD_SIZE, "snapshot unix socket ABI size");
_Static_assert(sizeof(struct criu_snapshot_socket_queue_record) == CRIU_SNAPSHOT_SOCKET_QUEUE_HEADER_SIZE, "snapshot socket queue ABI size");
_Static_assert(sizeof(struct criu_snapshot_sigaction_header) == CRIU_SNAPSHOT_SIGACTION_HEADER_SIZE, "snapshot sigaction header ABI size");
_Static_assert(sizeof(struct criu_snapshot_sigaction_entry) == CRIU_SNAPSHOT_SIGACTION_ENTRY_SIZE, "snapshot sigaction entry ABI size");
_Static_assert(sizeof(struct criu_snapshot_signal_queue_header) == CRIU_SNAPSHOT_SIGNAL_QUEUE_HEADER_SIZE, "snapshot signal queue header ABI size");
_Static_assert(sizeof(struct criu_snapshot_signal_queue_entry) == CRIU_SNAPSHOT_SIGNAL_QUEUE_ENTRY_SIZE, "snapshot signal queue entry ABI size");
_Static_assert(sizeof(struct criu_snapshot_itimers_header) == CRIU_SNAPSHOT_ITIMER_HEADER_SIZE, "snapshot itimer header ABI size");
_Static_assert(sizeof(struct criu_snapshot_itimer_entry) == CRIU_SNAPSHOT_ITIMER_ENTRY_SIZE, "snapshot itimer entry ABI size");
_Static_assert(sizeof(struct criu_snapshot_posix_timers_header) == CRIU_SNAPSHOT_POSIX_TIMER_HEADER_SIZE, "snapshot posix timer header ABI size");
_Static_assert(sizeof(struct criu_snapshot_posix_timer_entry) == CRIU_SNAPSHOT_POSIX_TIMER_ENTRY_SIZE, "snapshot posix timer entry ABI size");
_Static_assert(sizeof(struct criu_snapshot_pstree_record) == CRIU_SNAPSHOT_PSTREE_RECORD_SIZE, "snapshot pstree ABI size");
_Static_assert(sizeof(struct criu_snapshot_process_scope) == CRIU_SNAPSHOT_PROCESS_SCOPE_SIZE, "snapshot process scope ABI size");
#endif

/* Checksum covers header with checksum field zeroed followed by all TLVs; footer is excluded. */

#endif
