#!/usr/bin/env bash
# Покрытие харнессов: собирает их со сбором профиля, прогоняет корпус и печатает
# отчёт. Требует clang (llvm-profdata/llvm-cov входят в него).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# clang может лежать не в системе, а в домашнем каталоге (например, распакованный
# без root): если так — подставляем его в PATH.
LLVM_BIN="${LLVM_BIN:-$HOME/.local/llvm/usr/bin}"
if [[ -d "$LLVM_BIN" ]]; then PATH="$LLVM_BIN:$PATH"; export PATH; fi
OUT="$ROOT/tests/fuzz/build"
mkdir -p "$OUT"

if ! command -v clang >/dev/null 2>&1 || ! command -v llvm-profdata >/dev/null 2>&1; then
  echo "нужен clang с llvm-profdata/llvm-cov (для покрытия libFuzzer-сборки)" >&2
  exit 1
fi

SRCS=(
  "$ROOT/api/src/api_core.c"
  "$ROOT/json/GwJson.c"
  "$ROOT/json/jsmn/jsmn.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/LinkedList.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Mem.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Str.c"
  "$ROOT/CodeLibrary/CL/Converters/Src/Base64Encoding.c"
  "$ROOT/tests/unit/stubs/system_stub.c"
  "$ROOT/tests/unit/stubs/transport_stub.c"
)
INCLUDES=(
  -I"$ROOT/tests/unit/stubs"
  -I"$ROOT/api/include"
  -I"$ROOT/transport/include"
  -I"$ROOT/platform/include"
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
  clang -g -O1 -fsanitize=fuzzer -fprofile-instr-generate -fcoverage-mapping \
    "${INCLUDES[@]}" "${SRCS[@]}" "$ROOT/tests/fuzz/$src.c" -o "$bin"
  # Первый каталог — рабочий корпус (фаззер в него дописывает), второй — семена
  # только для чтения: иначе мутации засоряют seeds/ в репозитории.
  mkdir -p "$OUT/corpus_$src"
  LLVM_PROFILE_FILE="$OUT/${src}.profraw" "$bin" -max_total_time=60 \
    "$OUT/corpus_$src" "$corpus" ${CORPUS_EXTRA:-}
  PROFILES+=("$OUT/${src}.profraw")
done

llvm-profdata merge -sparse "${PROFILES[@]}" -o "$OUT/merged.profdata"
llvm-cov report "$OUT/ws_server_fuzz_cov" -instr-profile="$OUT/merged.profdata" \
  "$ROOT/api/src/api_core.c" "$ROOT/json/GwJson.c"

if [[ "${1:-}" == "--html" ]]; then
  llvm-cov show "$OUT/ws_server_fuzz_cov" -instr-profile="$OUT/merged.profdata" \
    -format=html -output-dir="$ROOT/tests/fuzz/coverage_html" \
    "$ROOT/api/src/api_core.c" "$ROOT/json/GwJson.c"
  echo "HTML: $ROOT/tests/fuzz/coverage_html/index.html"
fi
