"""Скрипты страниц прибора синтаксически целы: правка страницы не должна молча
ломать весь её JS (в браузере это пустая страница без ошибки на приборе).
Проверка через node (new Function), без браузера. Нет node — пропуск.

Запуск:  pytest tests/web
"""
import pathlib
import re
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2] / "main" / "flash_data"
SCRIPT = re.compile(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", re.S | re.I)
NODE = shutil.which("node")


def sources():
    for p in sorted(ROOT.rglob("*.html")):
        code = "\n".join(SCRIPT.findall(p.read_text(encoding="utf-8")))
        if code.strip():
            yield p, code
    for p in sorted(ROOT.rglob("*.js")):
        if not p.name.endswith(".min.js"):
            yield p, p.read_text(encoding="utf-8")


@pytest.mark.skipif(NODE is None, reason="нет node")
@pytest.mark.parametrize("page,code", list(sources()), ids=lambda v: getattr(v, "name", ""))
def test_script_parses(page, code):
    # код — через stdin: большие файлы (xterm.js) не влезают в аргументы
    check = ("let s='';process.stdin.on('data',d=>s+=d).on('end',()=>{"
             "try{new Function(s);}catch(e){console.log(e.message);process.exit(1);}});")
    r = subprocess.run([NODE, "-e", check], input=code, capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, f"{page.relative_to(ROOT)}: {r.stdout.strip()}"
