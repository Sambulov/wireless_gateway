#!/usr/bin/env bash
# Builds source-based coverage variants of both ws_server libFuzzer harnesses
# (single-frame and sequence), replays each over its corpus + seeds, merges
# the profiles and prints a combined per-line/per-file coverage report.
# Optionally emits an HTML report.
#
# Usage:
#   tests/fuzz/coverage.sh            # text report
#   tests/fuzz/coverage.sh --html     # also write HTML report to tests/fuzz/coverage_html/
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

# harness source : binary : input dirs (comma separated)
TARGETS=(
  "ws_server_fuzz:ws_server_fuzzer_cov:corpus,seeds"
  "ws_server_seq_fuzz:ws_server_seq_fuzzer_cov:corpus_seq,seeds_seq"
)

INCLUDES=(
  -I"$ROOT/tests/unit/stubs"
  -I"$ROOT/ws_server/include"
  -I"$ROOT/CodeLibrary/CL"
  -I"$ROOT/json/cJSON"
  -I"$ROOT/json"
)

PROFDATA="$OUT/cov.profdata"
PROFRAWS=()
OBJECTS=()

for t in "${TARGETS[@]}"; do
  IFS=: read -r src bin dirs <<< "$t"
  BIN="$OUT/$bin"

  clang -g -O1 \
    -fsanitize=fuzzer \
    -fprofile-instr-generate -fcoverage-mapping \
    -DWS_SERVER_TEST \
    "${INCLUDES[@]}" \
    "${SRCS[@]}" "$ROOT/tests/fuzz/$src.c" \
    -o "$BIN"

  echo "Built $BIN"

  INPUTS=()
  IFS=, read -ra DIRS <<< "$dirs"
  for d in "${DIRS[@]}"; do
    [[ -d "$ROOT/tests/fuzz/$d" ]] && INPUTS+=("$ROOT/tests/fuzz/$d"/*)
  done

  # One process per input: an input that crashes the harness (a known,
  # not yet fixed bug) must not cut the replay of everything after it.
  PROFDIR="$OUT/prof_$bin"
  rm -rf "$PROFDIR"
  mkdir -p "$PROFDIR"
  for f in "${INPUTS[@]}"; do
    LLVM_PROFILE_FILE="$PROFDIR/%p.profraw" "$BIN" -close_fd_mask=3 "$f" >/dev/null 2>&1 || true
  done
  shopt -s nullglob
  PROFRAWS+=("$PROFDIR"/*.profraw)
  shopt -u nullglob

  if (( ${#OBJECTS[@]} )); then OBJECTS+=(-object "$BIN"); else OBJECTS+=("$BIN"); fi
done

llvm-profdata merge -sparse "${PROFRAWS[@]}" -o "$PROFDATA"

# Only report on our own sources, not cJSON/CodeLibrary/stubs/harness.
REPORT_SRCS=("$ROOT/ws_server/src/ws_server.c")

echo
echo "=== Line coverage (ws_server.c) ==="
llvm-cov report "${OBJECTS[@]}" -instr-profile="$PROFDATA" "${REPORT_SRCS[@]}"

if [[ "${1:-}" == "--html" ]]; then
  HTML_OUT="$ROOT/tests/fuzz/coverage_html"
  llvm-cov show "${OBJECTS[@]}" -instr-profile="$PROFDATA" "${REPORT_SRCS[@]}" \
    -format=html -output-dir="$HTML_OUT" -show-line-counts-or-regions
  echo
  echo "HTML report: $HTML_OUT/index.html"
fi
