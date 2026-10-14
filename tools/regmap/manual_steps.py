#!/usr/bin/env python3
"""Извлечение колонки «шаг» из краткого руководства INNOVERT и сверка с картой.

Таблица параметров в PDF набрана в две половины, у каждой свои полосы колонок
(координаты в пунктах):
    левая:  код x≈50..80,   диапазон x≈145..185, шаг x≈225..250, заводское x≈250..292
    правая: код x≈330..356, диапазон x≈432..492, шаг x≈500..530, заводское x≈530..570

У части параметров строка разбита на две ветви — «(до 37 кВт)» и «(от 45 кВт)» —
и шаг в них разный. Прибор ISD222M21E мощностью 2,2 кВт относится к ветви
«до 37 кВт», поэтому карта обязана использовать именно её шаг.

    python3 tools/regmap/manual_steps.py [pdf] [карта]
"""
import re
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import jsonc  # noqa: E402

WORD = (r'<word xMin="([\d.]+)" yMin="([\d.]+)" '
        r'xMax="([\d.]+)" yMax="([\d.]+)">([^<]+)</word>')

# половина таблицы → полосы: код, шаг, заводское значение
HALVES = {
    "left":  ((50, 80), (225, 250), (250, 292)),
    "right": ((330, 356), (500, 530), (530, 570)),
}
BASE = {"PA": 0, "PB": 100, "PC": 200, "PD": 300, "PE": 400,
        "PF": 500, "PG": 600, "PH": 700, "PI": 800}
CODE_RE = re.compile(r'(P[ABCDEFGHIabcd])(\d{2})')
BRANCH_RE = re.compile(r'^\((до|от)$')


def in_band(x: float, band) -> bool:
    return band[0] <= x <= band[1]


def parse_page(xml: str) -> dict:
    """→ {код: [(метка ветви, шаг), ...]} для одной страницы.

    Строка параметра может занимать несколько строк текста (ветви «до 37 кВт» и
    «от 45 кВт»). Ячейка шага стоит на базовой линии своей строки, а если она
    объединена по ветвям — по центру между ними. Поэтому сначала ищем строки по
    описанию, потом привязываем шаг к ближайшей строке, а несовпавший считаем
    общим для всего параметра.
    """
    ws = [(float(a), float(b), float(c), float(d), t)
          for a, b, c, d, t in re.findall(WORD, xml)]
    out = {}
    for half, (code_band, step_band, _) in HALVES.items():
        codes = sorted(((t, y0) for x0, y0, x1, y1, t in ws
                        if in_band(x0, code_band) and CODE_RE.fullmatch(t)),
                       key=lambda c: c[1])
        for i, (code, y) in enumerate(codes):
            nxt = codes[i + 1][1] if i + 1 < len(codes) else y + 40
            row = [(x0, yy, t) for x0, yy, x1, y1, t in ws if y - 3 <= yy < nxt]
            steps = sorted((yy, t) for x0, yy, t in row if in_band(x0, step_band))
            if not steps:
                continue
            # строки текста внутри параметра: по меткам ветвей, иначе по коду
            marks = sorted((yy, t) for x0, yy, t in row if BRANCH_RE.match(t))
            lines = [yy for yy, _ in marks] or [y]
            merged = None
            if len(steps) == 1 and len(lines) > 1:
                merged = steps[0][1]          # объединённая ячейка
            key = CODE_RE.fullmatch(code).group(1).upper() + CODE_RE.fullmatch(code).group(2)
            variants = []
            for k, ly in enumerate(lines):
                label = ("до 37 кВт" if marks[k][1] == "(до" else "от 45 кВт") \
                    if k < len(marks) else "все"
                near = next((t for sy, t in steps if abs(sy - ly) <= 3), None)
                variants.append((label, near or merged or steps[0][1]))
            out[key] = variants if len(set(v for _, v in variants)) > 1 else \
                [(variants[0][0] if len(variants) == 1 else "все", variants[0][1])]
    return out


def main(argv) -> int:
    pdf = argv[1] if len(argv) > 1 else "devices/innovert_isd/INNOVERT-ISD-mini-manual.pdf"
    mapfile = argv[2] if len(argv) > 2 else "main/flash_data/regmaps/innovert_isd222.jsonc"
    xml = subprocess.run(["pdftotext", "-bbox", pdf, "-"],
                         capture_output=True, text=True, check=True).stdout
    pages = re.findall(r'<page width="[\d.]+" height="[\d.]+">(.*?)</page>', xml, re.S)

    steps = {}
    for body in pages:
        steps.update(parse_page(body))

    mp, _ = jsonc.loads(open(mapfile, encoding="utf-8").read())
    regs = {r["addr"]: r for g in mp["groups"] for r in g["registers"]}

    print(f"параметров с шагом в мануале: {len(steps)}\n")
    print("  код    адрес  ветвь        шаг      масштаб карты  вердикт")
    bad, checked = [], 0
    for code in sorted(steps):
        grp, num = code[:2], int(code[2:])
        addr = BASE.get(grp, 0) + num
        r = regs.get(addr)
        if not r:
            continue
        checked += 1
        variants = steps[code]
        # берём ветвь «до 37 кВт», иначе общую
        pick = next((s for m, s in variants if m == "до 37 кВт"),
                    variants[0][1])
        # шаг мануала — это разрешение в инженерных единицах (0,1 В, 0,01 А),
        # то есть ровно множитель raw → значение
        num = re.match(r'[\d.,]+', pick)
        if not num:
            continue
        want = float(num.group().replace(",", "."))
        got = (r.get("value") or {}).get("scale", 1.0)
        label = next((m for m, s in variants if s == pick), "все")
        ok = abs(got - want) < 1e-12
        if not ok:
            bad.append((code, addr, label, pick, want, got, r.get("name", "")))
        print(f"  {code:6} {addr:5}  {label:11}  {pick:7}  {got:<13g}  "
              f"{'ок' if ok else 'РАСХОЖДЕНИЕ'}   {r.get('name','')[:34]}")

    print(f"\nсверено регистров: {checked}, расхождений: {len(bad)}")
    for code, addr, label, pick, want, got, name in bad:
        mult = got / want if want else 0
        print(f"  • {code} (адрес {addr}, {label}): мануал шаг {pick} → масштаб "
              f"{want:g}, в карте {got:g} (в {mult:g} раза)  — {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
