#!/usr/bin/env bash
# Покрытие харнессов: собирает их со сбором профиля, прогоняет корпус и печатает
# отчёт. Требует clang (llvm-profdata/llvm-cov входят в него).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/tests/fuzz/build"
mkdir -p "$OUT"

if ! command -v clang >/dev/null 2>&1 || ! command -v llvm-profdata >/dev/null 2>&1; then
  echo "нужен clang с llvm-profdata/llvm-cov (для покрытия libFuzzer-сборки)" >&2
  exit 1
fi

SRCS=(
  "$ROOT/ws_server/src/ws_server.c"
  "$ROOT/json/GwJson.c"
  "$ROOT/json/jsmn/jsmn.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/LinkedList.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Mem.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Str.c"
  "$ROOT/CodeLibrary/CL/Converters/Src/Base64Encoding.c"
  "$ROOT/tests/unit/stubs/ws_hal_stub.c"
  "$ROOT/tests/unit/stubs/ws_transport_stub.c"
)
INCLUDES=(
  -I"$ROOT/tests/unit/stubs"
  -I"$ROOT/ws_server/include"
  -I"$ROOT/CodeLibrary/CL"
  -I"$ROOT/json"
  -I"$ROOT/json/jsmn"
)

CORPORA=(
  "ws_server_fuzz:$ROOT/tests/fuzz/seeds"
  "ws_server_seq_fuzz:$ROOT/tests/fuzz/seeds_seq"
)

PROFILES=()
for entry in "${CORPORA[@]}"; do
  src="${entry%%:*}"
  corpus="${entry##*:}"
  bin="$OUT/${src}_cov"
  clang -g -O1 -fprofile-instr-generate -fcoverage-mapping -DWS_SERVER_TEST \
    "${INCLUDES[@]}" "${SRCS[@]}" "$ROOT/tests/fuzz/$src.c" -o "$bin"
  LLVM_PROFILE_FILE="$OUT/${src}.profraw" "$bin" -max_total_time=60 "$corpus" ${CORPUS_EXTRA:-}
  PROFILES+=("$OUT/${src}.profraw")
done

llvm-profdata merge -sparse "${PROFILES[@]}" -o "$OUT/merged.profdata"
llvm-cov report "$OUT/ws_server_fuzz_cov" -instr-profile="$OUT/merged.profdata" \
  "$ROOT/ws_server/src/ws_server.c" "$ROOT/json/GwJson.c"

if [[ "${1:-}" == "--html" ]]; then
  llvm-cov show "$OUT/ws_server_fuzz_cov" -instr-profile="$OUT/merged.profdata" \
    -format=html -output-dir="$ROOT/tests/fuzz/coverage_html" \
    "$ROOT/ws_server/src/ws_server.c" "$ROOT/json/GwJson.c"
  echo "HTML: $ROOT/tests/fuzz/coverage_html/index.html"
fi
