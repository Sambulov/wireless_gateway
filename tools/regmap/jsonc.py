#!/usr/bin/env python3
"""Чтение и запись описаний устройств Modbus (формат modbus-regmap/1.0).

Синтаксис файла — JSONC: комментарии // и /* */, висячие запятые.
Пропущенные запятые синтаксически недопустимы, но `load(..., repair=True)`
умеет их починить (как это встречается в рукописных файлах) и сообщает,
сколько правок сделано.

    from jsonc import load, dump
    doc = load("devices/regmaps/innovert.jsonc")

Запуск как утилиты:

    python3 tools/regmap/jsonc.py <файл>            # проверить и нормализовать в JSON
    python3 tools/regmap/jsonc.py --fix <файл>      # починить и перезаписать
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


class JsoncError(ValueError):
    """Ошибка разбора с указанием места."""

    def __init__(self, message: str, line: int | None = None, col: int | None = None):
        self.line, self.col = line, col
        where = f" (строка {line}, позиция {col})" if line else ""
        super().__init__(f"{message}{where}")


def _strip_comments(text: str) -> str:
    """Убирает // и /* */ комментарии, не трогая содержимое строк."""
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")      # сохраняем нумерацию строк
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _strip_trailing_commas(text: str) -> str:
    """Убирает запятые перед закрывающими скобками (не трогая строки)."""
    out = []
    i, n = 0, len(text)
    in_str = False
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == ",":
            j = i + 1
            while j < n and text[j] in " \t\r\n":
                j += 1
            if j < n and text[j] in "}]":
                i += 1                     # запятую выбрасываем
                continue
        out.append(ch)
        i += 1
    return "".join(out)


_MISSING_COMMA = (
    # строка заканчивается } или ], а следующая начинается с { [ или " — типичная
    # опечатка при ручном добавлении элемента массива
)


def repair_text(text: str) -> tuple[str, int]:
    """Вставляет пропущенные запятые между элементами. Возвращает (текст, правок)."""
    lines = text.splitlines()
    fixed, count = [], 0
    for i, line in enumerate(lines):
        stripped = line.rstrip()
        nxt = next((l.strip() for l in lines[i + 1:] if l.strip()), "")
        if (
            not stripped.endswith(",")
            and stripped.endswith(("}", "]"))
            and nxt[:1] in ("{", "[", '"')
        ):
            stripped += ","
            count += 1
        fixed.append(stripped)
    return "\n".join(fixed) + "\n", count


def loads(text: str, repair: bool = False) -> tuple[object, int]:
    """Разбирает JSONC. Возвращает (объект, число правок при repair=True)."""
    prepared = _strip_trailing_commas(_strip_comments(text))
    fixes = 0
    try:
        return json.loads(prepared), fixes
    except json.JSONDecodeError as exc:
        if not repair:
            raise JsoncError(
                f"не удалось разобрать: {exc.msg}", exc.lineno, exc.colno
            ) from None
        repaired, fixes = repair_text(prepared)
        try:
            return json.loads(repaired), fixes
        except json.JSONDecodeError as exc2:
            raise JsoncError(
                f"не удалось разобрать даже после починки: {exc2.msg}",
                exc2.lineno,
                exc2.colno,
            ) from None


def load(path: str | Path, repair: bool = False) -> object:
    obj, _ = loads(Path(path).read_text(encoding="utf-8"), repair=repair)
    return obj


def dump(obj: object, header: str | None = None) -> str:
    """Канонический вид: строгий JSON, отступ 2, перевод строки в конце."""
    body = json.dumps(obj, ensure_ascii=False, indent=2)
    prefix = "".join(f"// {line}\n" for line in header.splitlines()) if header else ""
    return f"{prefix}{body}\n"


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith("-")]
    fix = "--fix" in argv
    if len(args) != 1:
        print(__doc__)
        return 2

    path = Path(args[0])
    text = path.read_text(encoding="utf-8")
    try:
        obj, fixes = loads(text, repair=fix)
    except JsoncError as exc:
        print(f"ОШИБКА: {path}: {exc}")
        if "починки" in str(exc) or not fix:
            print("Подсказка: запустите с --fix, чтобы вставить пропущенные запятые.")
        return 1

    if fixes:
        path.write_text(dump(obj, header=f"нормализовано из {path.name}"), encoding="utf-8")
        print(f"{path}: разобрано, исправлено запятых: {fixes} (файл перезаписан)")
    else:
        print(f"{path}: разобрано, замечаний нет")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
