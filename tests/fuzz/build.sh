#!/usr/bin/env bash
# Builds the ws_server libFuzzer harness on the host.
# Requires clang (libFuzzer ships built into compiler-rt since clang 6).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/tests/fuzz/build"
mkdir -p "$OUT"

SRCS=(
  "$ROOT/ws_server/src/ws_server.c"
  "$ROOT/json/cJSON/cJSON.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/LinkedList.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Mem.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Str.c"
  "$ROOT/CodeLibrary/CL/Converters/Src/Base64Encoding.c"
  "$ROOT/tests/unit/stubs/ws_hal_stub.c"
  "$ROOT/tests/unit/stubs/ws_conn_stub.c"
)

# harness source -> binary name
TARGETS=(
  "ws_server_fuzz:ws_server_fuzzer"
  "ws_server_seq_fuzz:ws_server_seq_fuzzer"
)

INCLUDES=(
  -I"$ROOT/tests/unit/stubs"
  -I"$ROOT/ws_server/include"
  -I"$ROOT/CodeLibrary/CL"
  -I"$ROOT/json/cJSON"
  -I"$ROOT/json"
)

for t in "${TARGETS[@]}"; do
  src="${t%%:*}"
  bin="${t##*:}"
  clang -g -O1 \
    -fsanitize=fuzzer,address,undefined \
    -DWS_SERVER_TEST \
    "${INCLUDES[@]}" \
    "${SRCS[@]}" "$ROOT/tests/fuzz/$src.c" \
    -o "$OUT/$bin"
  echo "Built $OUT/$bin"
done
