#!/usr/bin/env bash
# Сборка фаззинг-харнессов ядра API.
#
# Есть clang — собираем настоящий libFuzzer (он входит в compiler-rt с clang 6).
# Нет clang — собираем те же харнессы с прогонщиком (runner.c) под ASan/UBSan:
# мутаций не будет, зато корпус и любая внешняя нарезка входов проверяются.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/tests/fuzz/build"
mkdir -p "$OUT"

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

TARGETS=(
  "ws_server_fuzz:ws_server_fuzzer"
  "ws_server_seq_fuzz:ws_server_seq_fuzzer"
)

INCLUDES=(
  -I"$ROOT/tests/unit/stubs"
  -I"$ROOT/ws_server/include"
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
    "$CC_BIN" -g -O1 -fsanitize=fuzzer,address,undefined -DWS_SERVER_TEST \
      "${INCLUDES[@]}" "${SRCS[@]}" "$ROOT/tests/fuzz/$src.c" -o "$OUT/$bin"
    echo "собран (libFuzzer, $CC_BIN): $OUT/$bin"
  else
    base="${bin%_fuzzer}_runner"
    "$CC_BIN" -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
      -DWS_SERVER_TEST "${INCLUDES[@]}" "${SRCS[@]}" \
      "$ROOT/tests/fuzz/$src.c" "$ROOT/tests/fuzz/runner.c" -o "$OUT/$base"
    echo "собран (прогонщик без clang, $CC_BIN): $OUT/$base"
  fi
done

if [[ "$MODE" == runner ]]; then
  echo
  echo "libFuzzer недоступен (нет clang): собранные бинарники прогоняют готовые входы,"
  echo "а не мутируют их. Для настоящего фаззинга поставь clang и пересобери."
fi
