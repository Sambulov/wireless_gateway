#!/usr/bin/env bash
set -e

# idf.py qemu берёт qemu-system-xtensa из PATH, а системный qemu из /usr/bin
# машину esp32 не знает: нужен Espressif'овский из IDF_TOOLS_PATH. Ставим его
# каталог в начало PATH и падаем с подсказкой, если его нет.
IDF_TOOLS_ROOT="${IDF_TOOLS_PATH:-$HOME/.espressif}/tools/qemu-xtensa"
QEMU_BIN="$(find "$IDF_TOOLS_ROOT" -name qemu-system-xtensa -type f 2>/dev/null | sort | tail -1)"
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-xtensa (Espressif) не найден в $IDF_TOOLS_ROOT" >&2
    echo "Установите: python \"\$IDF_PATH/tools/idf_tools.py\" install qemu-xtensa" >&2
    exit 1
fi
export PATH="$(dirname "$QEMU_BIN"):$PATH"

# Порт переадресации занят — эмулятор не стартует; понятное сообщение лучше,
# чем ошибка qemu в глубине вывода.
if ss -tlnH 2>/dev/null | awk '{print $4}' | grep -q ':8080$'; then
    echo "Порт 8080 уже занят — эмулятор, похоже, уже запущен:" >&2
    ss -tlnp 2>/dev/null | grep ':8080' >&2 || true
    exit 1
fi

idf.py qemu --qemu-extra-args "-nic user,model=open_eth,hostfwd=tcp::8080-:80 -serial null -serial pty" "${1:-}"
