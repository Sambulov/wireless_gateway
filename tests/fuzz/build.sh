#!/usr/bin/env bash
# Сборка фаззинг-харнессов ядра API.
#
# Есть clang — собираем настоящий libFuzzer (он входит в compiler-rt с clang 6).
# Нет clang — собираем те же харнессы с прогонщиком (runner.c) под ASan/UBSan:
# мутаций не будет, зато корпус и любая внешняя нарезка входов проверяются.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# clang может лежать не в системе, а в домашнем каталоге (например, распакованный
# без root): если так — подставляем его в PATH.
LLVM_BIN="${LLVM_BIN:-$HOME/.local/llvm/usr/bin}"
if [[ -d "$LLVM_BIN" ]]; then PATH="$LLVM_BIN:$PATH"; export PATH; fi
OUT="$ROOT/tests/fuzz/build"
mkdir -p "$OUT"

SRCS=(
  "$ROOT/api/src/api_core.c"
  "$ROOT/json/GwJson.c"
  "$ROOT/json/jsmn/jsmn.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/LinkedList.c"
  "$ROOT/CodeLibrary/CL/Workflow/Src/Event.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Mem.c"
  "$ROOT/CodeLibrary/CL/DataStructures/Src/Str.c"
  "$ROOT/CodeLibrary/CL/Converters/Src/Base64Encoding.c"
  "$ROOT/tests/unit/stubs/system_stub.c"
  "$ROOT/tests/unit/stubs/transport_stub.c"
)

TARGETS=(
  "ws_server_fuzz:ws_server_fuzzer"
  "ws_server_seq_fuzz:ws_server_seq_fuzzer"
)

INCLUDES=(
  -I"$ROOT/tests/unit/stubs"
  -I"$ROOT/main"
  -I"$ROOT/api/include"
  -I"$ROOT/transport/include"
  -I"$ROOT/platform/include"
  -I"$ROOT/CodeLibrary/CL"
  -I"$ROOT/json"
  -I"$ROOT/json/jsmn"
)

CC_BIN="${CC:-}"
if [[ -z "$CC_BIN" ]]; then
  if command -v clang >/dev/null 2>&1; then CC_BIN=clang; else CC_BIN=gcc; fi
fi

if "$CC_BIN" --version 2>/dev/null | grep -qi clang; then
  MODE=libfuzzer
else
  MODE=runner
fi

for t in "${TARGETS[@]}"; do
  src="${t%%:*}"
  bin="${t##*:}"
  if [[ "$MODE" == libfuzzer ]]; then
    "$CC_BIN" -g -O1 -fsanitize=fuzzer,address,undefined \
      "${INCLUDES[@]}" "${SRCS[@]}" "$ROOT/tests/fuzz/$src.c" -o "$OUT/$bin"
    echo "собран (libFuzzer, $CC_BIN): $OUT/$bin"
  else
    base="${bin%_fuzzer}_runner"
    "$CC_BIN" -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      "${INCLUDES[@]}" "${SRCS[@]}" \
      "$ROOT/tests/fuzz/$src.c" "$ROOT/tests/fuzz/runner.c" -o "$OUT/$base"
    echo "собран (прогонщик без clang, $CC_BIN): $OUT/$base"
  fi
done

if [[ "$MODE" == runner ]]; then
  echo
  echo "libFuzzer недоступен (нет clang): собранные бинарники прогоняют готовые входы,"
  echo "а не мутируют их. Для настоящего фаззинга поставь clang и пересобери."
fi
