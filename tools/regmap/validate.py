#!/usr/bin/env python3
"""Проверка файла описания устройства Modbus по схеме modbus-regmap/1.0.

    python3 tools/regmap/validate.py devices/regmaps/*.jsonc

Проверяет обязательные поля, допустимые значения и все перекрёстные ссылки
(словари, адреса в условиях, границах, командах и оформлении).
Возвращает 1, если есть ошибки.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jsonc import JsoncError, load  # noqa: E402

FORMAT_PREFIX = "modbus-regmap/"

TYPES = {
    "u16": 1, "s16": 1, "u32": 2, "s32": 2, "u64": 4, "s64": 4,
    "f32": 2, "f64": 4, "bcd": 1, "bits": 1,
    "string": None, "time": None, "struct": None,
}
ACCESS = {"R", "RW", "W", "WO"}
BYTE_ORDERS = {"ABCD", "CDAB", "BADC", "DCBA"}
PARITY = {"none", "even", "odd"}
POLL = {"fast", "slow", "on-demand", "never"}
COND_OPS = {"eq", "ne", "in", "gt", "lt", "between", "bit"}
WRITE_MODES = {"direct", "command"}

KNOWN_TOP = {"format", "device", "transport", "enums", "groups", "commands", "alarms", "ui"}
KNOWN_DEVICE = {"id", "vendor", "model", "series", "revision", "note"}
KNOWN_TRANSPORT = {"protocol", "defaults", "limits", "byte_order", "register_base", "functions"}
KNOWN_REG = {
    "addr", "name", "desc", "access", "write", "type", "count", "byte_order", "enum",
    "enum_from", "bits", "bcd", "fields", "value", "default", "volatile", "poll",
    "visible_if", "enable_if", "readonly_if", "tags",
}
KNOWN_VALUE = {"scale", "offset", "unit", "decimals", "min", "max", "min_from",
               "max_from", "step", "format"}


class Report:
    def __init__(self, path: Path):
        self.path = path
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.notes: list[str] = []

    def err(self, where: str, text: str) -> None:
        self.errors.append(f"{where}: {text}")

    def warn(self, where: str, text: str) -> None:
        self.warnings.append(f"{where}: {text}")

    def note(self, text: str) -> None:
        self.notes.append(text)

    def unknown(self, where: str, obj: dict, known: set[str]) -> None:
        for key in obj:
            if key not in known:
                self.warn(where, f"неизвестное поле '{key}' (будет проигнорировано)")


def _is_num(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def validate_condition(rep: Report, where: str, cond, addrs: set[int]) -> None:
    if not isinstance(cond, dict):
        rep.err(where, "условие должно быть объектом")
        return
    if "all" in cond or "any" in cond:
        key = "all" if "all" in cond else "any"
        if not isinstance(cond[key], list) or not cond[key]:
            rep.err(where, f"'{key}' должен быть непустым массивом условий")
            return
        for i, sub in enumerate(cond[key]):
            validate_condition(rep, f"{where}.{key}[{i}]", sub, addrs)
        return
    if "addr" not in cond:
        rep.err(where, "в условии нет 'addr'")
        return
    if cond["addr"] not in addrs:
        rep.err(where, f"условие ссылается на несуществующий адрес {cond['addr']}")
    ops = [k for k in cond if k in COND_OPS]
    if len(ops) != 1:
        rep.err(where, f"нужен ровно один оператор из {sorted(COND_OPS)}, найдено {ops}")
    if "in" in cond and not isinstance(cond["in"], list):
        rep.err(where, "'in' должен быть массивом")


def validate(path: Path) -> Report:
    rep = Report(path)
    try:
        doc = load(path, repair=False)
    except JsoncError as exc:
        rep.err(path.name, f"не разбирается: {exc}")
        return rep

    if not isinstance(doc, dict):
        rep.err(path.name, "верхний уровень должен быть объектом")
        return rep
    rep.unknown(path.name, doc, KNOWN_TOP)

    fmt = doc.get("format")
    if not isinstance(fmt, str) or not fmt.startswith(FORMAT_PREFIX):
        rep.err("format", f"ожидается строка вида '{FORMAT_PREFIX}1.0', получено {fmt!r}")
    elif fmt != "modbus-regmap/1.0":
        rep.warn("format", f"версия '{fmt}' — проверка рассчитана на 1.0")

    # ── device ────────────────────────────────────────────────────────────
    dev = doc.get("device")
    if not isinstance(dev, dict):
        rep.err("device", "обязательный раздел отсутствует")
    else:
        rep.unknown("device", dev, KNOWN_DEVICE)
        for key in ("id", "vendor", "model"):
            if not dev.get(key):
                rep.err("device", f"обязательное поле '{key}' не заполнено")

    # ── transport ─────────────────────────────────────────────────────────
    tr = doc.get("transport") or {}
    if not isinstance(tr, dict):
        rep.err("transport", "должен быть объектом")
        tr = {}
    else:
        rep.unknown("transport", tr, KNOWN_TRANSPORT)
        d = tr.get("defaults") or {}
        if not isinstance(d, dict):
            rep.err("transport.defaults", "должен быть объектом")
            d = {}
        if "address" in d and not (isinstance(d["address"], int) and 1 <= d["address"] <= 247):
            rep.err("transport.defaults.address", "адрес должен быть 1..247")
        if "parity" in d and d["parity"] not in PARITY:
            rep.err("transport.defaults.parity", f"допустимо {sorted(PARITY)}")
        if "stopbits" in d and d["stopbits"] not in (1, 2):
            rep.err("transport.defaults.stopbits", "допустимо 1 или 2")
        if "baud" in d and not (isinstance(d["baud"], int) and d["baud"] > 0):
            rep.err("transport.defaults.baud", "должно быть положительным целым")
        lim = tr.get("limits") or {}
        for key in ("max_regs_per_read", "max_regs_per_write"):
            if key in lim and not (isinstance(lim[key], int) and 1 <= lim[key] <= 125):
                rep.err(f"transport.limits.{key}", "допустимо 1..125")
        if tr.get("byte_order", "ABCD") not in BYTE_ORDERS:
            rep.err("transport.byte_order", f"допустимо {sorted(BYTE_ORDERS)}")
        if tr.get("register_base", 0) not in (0, 1):
            rep.err("transport.register_base", "допустимо 0 или 1")

    # ── enums ─────────────────────────────────────────────────────────────
    enums = doc.get("enums") or {}
    if not isinstance(enums, dict):
        rep.err("enums", "должен быть объектом")
        enums = {}
    for name, items in enums.items():
        if not isinstance(items, list) or not items:
            rep.err(f"enums.{name}", "должен быть непустым массивом")
            continue
        seen = set()
        for i, item in enumerate(items):
            if not isinstance(item, dict) or "v" not in item or "t" not in item:
                rep.err(f"enums.{name}[{i}]", "нужны поля 'v' и 't'")
                continue
            if not _is_num(item["v"]):
                rep.err(f"enums.{name}[{i}].v", "должно быть числом")
            if item["v"] in seen:
                rep.err(f"enums.{name}[{i}].v", f"значение {item['v']} повторяется")
            seen.add(item["v"])

    # ── регистры ──────────────────────────────────────────────────────────
    groups = doc.get("groups")
    if not isinstance(groups, list) or not groups:
        rep.err("groups", "обязательный непустой массив")
        groups = []

    addrs: dict[int, str] = {}          # addr -> "group/name"
    occupied: dict[int, str] = {}       # addr -> чей это регистр (для контроля перекрытий)
    reg_index: dict[int, dict] = {}
    group_ids = set()
    # Ссылки проверяем вторым проходом: регистр может ссылаться на адрес,
    # который описан ниже по файлу.
    pending_refs: list[tuple[str, str, int]] = []   # (где, что, адрес)
    pending_conds: list[tuple[str, dict]] = []      # (где, условие)

    for gi, group in enumerate(groups):
        gwhere = f"groups[{gi}]"
        if not isinstance(group, dict):
            rep.err(gwhere, "группа должна быть объектом")
            continue
        gid = group.get("id")
        if not gid:
            rep.err(gwhere, "нет 'id'")
        elif gid in group_ids:
            rep.err(gwhere, f"идентификатор группы '{gid}' повторяется")
        else:
            group_ids.add(gid)
        if not group.get("name"):
            rep.err(gwhere, "нет 'name'")

        regs = group.get("registers")
        if not isinstance(regs, list) or not regs:
            rep.err(gwhere, "нет непустого 'registers'")
            continue

        for ri, reg in enumerate(regs):
            rwhere = f"{gid or '?'}[{ri}]"
            if not isinstance(reg, dict):
                rep.err(rwhere, "регистр должен быть объектом")
                continue
            rep.unknown(rwhere, reg, KNOWN_REG)

            addr = reg.get("addr")
            if not isinstance(addr, int) or addr < 0:
                rep.err(rwhere, "нет корректного 'addr'")
                continue
            if addr in addrs:
                rep.err(rwhere, f"адрес {addr} уже описан в {addrs[addr]}")
                continue
            addrs[addr] = rwhere
            reg_index[addr] = reg

            if not reg.get("name"):
                rep.err(rwhere, "нет 'name'")

            rtype = reg.get("type")
            if rtype not in TYPES:
                rep.err(rwhere, f"неизвестный тип {rtype!r}, допустимо {sorted(TYPES)}")
                continue

            access = reg.get("access")
            if access not in ACCESS:
                rep.err(rwhere, f"доступ {access!r}, допустимо {sorted(ACCESS)}")

            count = reg.get("count", TYPES[rtype] or 0)
            if TYPES[rtype] is None and not reg.get("count"):
                rep.err(rwhere, f"для типа '{rtype}' нужно поле 'count'")
            if not isinstance(count, int) or count < 1:
                rep.err(rwhere, "'count' должен быть >= 1")
                count = max(1, TYPES[rtype] or 1)

            for a in range(addr, addr + count):
                if a in occupied:
                    rep.err(rwhere, f"адрес {a} уже занят регистром {occupied[a]}")
                occupied[a] = rwhere

            if reg.get("byte_order") and reg["byte_order"] not in BYTE_ORDERS:
                rep.err(rwhere, f"byte_order {reg['byte_order']!r} недопустим")

            wmode = (reg.get("write") or {}).get("mode")
            if wmode and wmode not in WRITE_MODES:
                rep.err(rwhere, f"write.mode {wmode!r}, допустимо {sorted(WRITE_MODES)}")

            if reg.get("poll") and reg["poll"] not in POLL:
                rep.err(rwhere, f"poll {reg['poll']!r}, допустимо {sorted(POLL)}")

            enum = reg.get("enum")
            if isinstance(enum, str) and enum not in enums:
                rep.err(rwhere, f"ссылка на неизвестный словарь '{enum}'")
            elif isinstance(enum, list):
                if not enum:
                    rep.err(rwhere, "пустой inline-словарь")
            elif enum is not None and not isinstance(enum, str):
                rep.err(rwhere, "'enum' должен быть именем словаря или массивом")

            if "enum_from" in reg:
                src = reg["enum_from"].get("addr") if isinstance(reg["enum_from"], dict) else None
                if src is None:
                    rep.err(rwhere, "в enum_from нет 'addr'")
                else:
                    pending_refs.append((rwhere, "enum_from", src))

            for bkey in ("bits",):
                for bi, bit in enumerate(reg.get(bkey) or []):
                    if not isinstance(bit, dict) or not isinstance(bit.get("mask"), int):
                        rep.err(f"{rwhere}.bits[{bi}]", "нужен целый 'mask'")
                        continue
                    if bit["mask"] <= 0:
                        rep.err(f"{rwhere}.bits[{bi}]", "'mask' должен быть положительным")
                    if isinstance(bit.get("enum"), str) and bit["enum"] not in enums:
                        rep.err(f"{rwhere}.bits[{bi}]", f"неизвестный словарь '{bit['enum']}'")

            if "bcd" in reg and (not isinstance(reg["bcd"], list)
                                 or not all(isinstance(x, int) and x > 0 for x in reg["bcd"])):
                rep.err(rwhere, "'bcd' должен быть массивом положительных чисел")

            for fi, fld in enumerate(reg.get("fields") or []):
                if not isinstance(fld, dict) or not fld.get("name"):
                    rep.err(f"{rwhere}.fields[{fi}]", "нужны 'name' и 'addr'")
                    continue
                if "addr" not in fld:
                    rep.err(f"{rwhere}.fields[{fi}]", "нет 'addr'")
                if "bits" in fld and (not isinstance(fld["bits"], list) or len(fld["bits"]) != 2):
                    rep.err(f"{rwhere}.fields[{fi}].bits", "ожидается [старший, младший]")

            val = reg.get("value") or {}
            if val:
                if not isinstance(val, dict):
                    rep.err(rwhere, "'value' должен быть объектом")
                    val = {}
                else:
                    rep.unknown(f"{rwhere}.value", val, KNOWN_VALUE)
                if "scale" in val and (not _is_num(val["scale"]) or val["scale"] == 0):
                    rep.err(rwhere, "value.scale не может быть нулём")
                if "min" in val and "max" in val and _is_num(val["min"]) and _is_num(val["max"]):
                    if val["min"] > val["max"]:
                        rep.err(rwhere, "value.min больше value.max")
                for bkey in ("min_from", "max_from"):
                    if bkey in val:
                        src = val[bkey].get("addr") if isinstance(val[bkey], dict) else None
                        if src is None:
                            rep.err(f"{rwhere}.value.{bkey}", "нет 'addr'")
                        else:
                            pending_refs.append((rwhere, f"value.{bkey}", src))

            if reg.get("volatile") and "default" in reg:
                rep.warn(rwhere, "у изменяемого (volatile) регистра задано 'default' — оно не имеет смысла")

            for ckey in ("visible_if", "enable_if", "readonly_if"):
                if ckey in reg:
                    pending_conds.append((f"{rwhere}.{ckey}", reg[ckey]))

    # ── второй проход: ссылки и условия (адреса уже собраны) ──────────────
    for where, kind, addr in pending_refs:
        if addr not in addrs:
            rep.warn(where, f"{kind} ссылается на адрес {addr}, которого нет в файле")
    for where, cond in pending_conds:
        validate_condition(rep, where, cond, set(addrs))

    # ── команды ───────────────────────────────────────────────────────────
    for ci, cmd in enumerate(doc.get("commands") or []):
        cwhere = f"commands[{ci}]"
        if not isinstance(cmd, dict):
            rep.err(cwhere, "команда должна быть объектом")
            continue
        if not cmd.get("id") or not cmd.get("name"):
            rep.err(cwhere, "нужны 'id' и 'name'")
        params = {p.get("name") for p in (cmd.get("params") or []) if isinstance(p, dict)}
        steps = cmd.get("steps")
        if not isinstance(steps, list) or not steps:
            rep.err(cwhere, "нет непустого 'steps'")
            continue
        for si, step in enumerate(steps):
            swhere = f"{cwhere}.steps[{si}]"
            if not isinstance(step, dict) or "write" not in step:
                rep.err(swhere, "поддерживается только шаг 'write'")
                continue
            w = step["write"]
            if not isinstance(w, dict) or "addr" not in w:
                rep.err(swhere, "в 'write' нет 'addr'")
                continue
            target = reg_index.get(w["addr"])
            if target is None:
                rep.err(swhere, f"адрес {w['addr']} не описан в файле")
            elif target.get("access") == "R":
                rep.err(swhere, f"адрес {w['addr']} только для чтения")
            if "value" not in w and "value_from" not in w:
                rep.err(swhere, "нужно 'value' или 'value_from'")
            vf = w.get("value_from")
            if isinstance(vf, str) and vf.startswith("param.") and vf[6:] not in params:
                rep.err(swhere, f"параметр '{vf[6:]}' не объявлен в команде")
        for cond in cmd.get("confirm") or []:
            validate_condition(rep, f"{cwhere}.confirm", cond, set(addrs))

    # ── аварии и оформление ───────────────────────────────────────────────
    for ai, alarm in enumerate(doc.get("alarms") or []):
        awhere = f"alarms[{ai}]"
        if not isinstance(alarm, dict):
            rep.err(awhere, "авария должна быть объектом")
            continue
        if alarm.get("addr") not in addrs:
            rep.err(awhere, f"адрес {alarm.get('addr')} не описан в файле")
        if isinstance(alarm.get("enum"), str) and alarm["enum"] not in enums:
            rep.err(awhere, f"неизвестный словарь '{alarm['enum']}'")

    ui = doc.get("ui") or {}
    for key in ("gauges", "charts"):
        for i, item in enumerate(ui.get(key) or []):
            if not isinstance(item, dict) or item.get("addr") not in addrs:
                rep.err(f"ui.{key}[{i}]", f"адрес {item.get('addr') if isinstance(item, dict) else item} не описан")

    # ── итог по объёму ────────────────────────────────────────────────────
    if not rep.errors:
        rep.note(f"регистров: {len(addrs)}, групп: {len(groups)}, словарей: {len(enums)}, "
                 f"команд: {len(doc.get('commands') or [])}")
    return rep


def main(argv: list[str]) -> int:
    files = [Path(a) for a in argv[1:]]
    if not files:
        print(__doc__)
        return 2

    bad = 0
    for path in files:
        rep = validate(path)
        status = "ОШИБКИ" if rep.errors else "ок"
        print(f"\n=== {path} — {status} ===")
        for e in rep.errors:
            print(f"  ОШИБКА  {e}")
        for w in rep.warnings:
            print(f"  замеч.  {w}")
        for n in rep.notes:
            print(f"  итог    {n}")
        if rep.errors:
            bad += 1
    print()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
