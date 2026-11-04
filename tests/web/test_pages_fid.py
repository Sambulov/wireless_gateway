"""Страницы прибора говорят на протоколе с именами FID.

С 5a7e9dc FID — имена ("uart2.modbus", "uart1.config"); числовой FID ядро
отвергает (NO_FID). innovert*.html долго оставались на числах (0x1123,
0x1010) и parseInt(obj.FID, 16) — страницы молча не работали. Статическая
проверка: без прибора и без браузера.

Запуск:  pytest tests/web
"""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2] / "main" / "flash_data"
PAGES = sorted(p for p in ROOT.rglob("*") if p.suffix in (".html", ".js"))

# FID числом: "FID":123, FID: 0x1010, "FID":${…} без JSON.stringify,
# разбор ответа parseInt(obj.FID, …).
BAD = [
    (re.compile(r'"FID"\s*:\s*(0x[0-9a-fA-F]+|\d+)'), "FID числом в JSON"),
    (re.compile(r'\bFID\s*:\s*(0x[0-9a-fA-F]+|\d+)\s*[,}]'), "FID числом в объекте"),
    (re.compile(r'"FID"\s*:\s*\$\{(?!JSON\.stringify)'), "FID в шаблоне без кавычек"),
    (re.compile(r'parseInt\(\s*\w+\.FID'), "разбор FID ответа как числа"),
]


def test_pages_exist():
    assert any(p.name == "innovert.html" for p in PAGES)


def test_no_numeric_fid_in_pages():
    problems = []
    for page in PAGES:
        for n, line in enumerate(page.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for rx, what in BAD:
                if rx.search(line):
                    problems.append(f"{page.relative_to(ROOT)}:{n}: {what}: {line.strip()[:100]}")
    assert not problems, "\n".join(problems)
