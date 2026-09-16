#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for name in \
	CRIU_SNAPSHOT_REC_PIPE_ENDPOINT \
	CRIU_SNAPSHOT_REC_PIPE_DATA \
	CRIU_SNAPSHOT_REC_UNIX_SOCKET \
	CRIU_SNAPSHOT_REC_SOCKET_QUEUE \
	CRIU_SNAPSHOT_PIPE_ENDPOINT_RECORD_SIZE \
	CRIU_SNAPSHOT_PIPE_DATA_HEADER_SIZE \
	CRIU_SNAPSHOT_UNIX_SOCKET_RECORD_SIZE \
	CRIU_SNAPSHOT_SOCKET_QUEUE_HEADER_SIZE; do
	grep -q "$name" "$ROOT/include/criu_snapshot.h"
done

grep -q 'struct criu_snapshot_pipe_endpoint_record' "$ROOT/include/criu_snapshot.h"
grep -q 'struct criu_snapshot_pipe_data_record' "$ROOT/include/criu_snapshot.h"
grep -q 'struct criu_snapshot_unix_socket_record' "$ROOT/include/criu_snapshot.h"
grep -q 'struct criu_snapshot_socket_queue_record' "$ROOT/include/criu_snapshot.h"
grep -q 'pipe_endpoints' "$ROOT/userspace/criu-module-convert/criu_model.c"
grep -q 'unix_sockets' "$ROOT/userspace/criu-module-convert/criu_model.c"
grep -q 'build_fd_object_table' "$ROOT/userspace/criu-module-convert/criu_model.c"

cat >"$TMP/abi.c" <<'EOF'
#include "criu_snapshot.h"

_Static_assert(sizeof(struct criu_snapshot_pipe_endpoint_record) ==
	CRIU_SNAPSHOT_PIPE_ENDPOINT_RECORD_SIZE, "pipe endpoint ABI");
_Static_assert(sizeof(struct criu_snapshot_pipe_data_record) ==
	CRIU_SNAPSHOT_PIPE_DATA_HEADER_SIZE, "pipe data ABI");
_Static_assert(sizeof(struct criu_snapshot_unix_socket_record) ==
	CRIU_SNAPSHOT_UNIX_SOCKET_RECORD_SIZE, "unix socket ABI");
_Static_assert(sizeof(struct criu_snapshot_socket_queue_record) ==
	CRIU_SNAPSHOT_SOCKET_QUEUE_HEADER_SIZE, "socket queue ABI");

int main(void)
{
	struct criu_snapshot_pipe_endpoint_record pipe = {0};
	struct criu_snapshot_unix_socket_record unixsk = {0};

	pipe.object_id = 1;
	pipe.pipe_id = 2;
	unixsk.object_id = 3;
	unixsk.peer_object_id = 4;
	return !(pipe.object_id && pipe.pipe_id && unixsk.object_id &&
		 unixsk.peer_object_id);
}
EOF

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
	-I"$ROOT/include" "$TMP/abi.c" -o "$TMP/abi"
"$TMP/abi"

echo 'A5_OBJECT_RECORD_CONTRACT: PASS'
