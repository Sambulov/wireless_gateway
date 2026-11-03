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
if ss -tlnH 2>/dev/null | awk '{print $4}' | grep -q ':8081$'; then
    echo "Порт 8081 уже занят — он нужен для WebSocket API (страницы ходят на ws://<host>:8081/api):" >&2
    ss -tlnp 2>/dev/null | grep ':8081' >&2 || true
    exit 1
fi

# UART1 и UART2 шлюза выведены наружу как pty с постоянными путями-симлинками:
# к ним можно привязать виртуальное устройство, напр.
#     bash devices/virt_innovert_freq/run.sh /tmp/wsgw-uart1
# (UART0 — консоль прошивки, её наружу не выводим.)
UART1_PTY=/tmp/wsgw-uart1
UART2_PTY=/tmp/wsgw-uart2
rm -f "$UART1_PTY" "$UART2_PTY"
echo "UART1 шлюза будет на $UART1_PTY, UART2 — на $UART2_PTY (появятся после старта qemu)"

# WebSocket API (порт 81 гостя) проброшен на хост-порт 8081: порт 81 на хосте
# без привилегий не связать (ip_unprivileged_port_start), поэтому страницы под
# QEMU берут WS-порт из HTTP-порта (8080 → 8081, см. flash_data/*.html).
idf.py qemu --qemu-extra-args \
    "-nic user,model=open_eth,hostfwd=tcp::8080-:80,hostfwd=tcp::8081-:81 \
     -chardev pty,id=uart1,path=$UART1_PTY -serial chardev:uart1 \
     -chardev pty,id=uart2,path=$UART2_PTY -serial chardev:uart2" "${1:-}"
