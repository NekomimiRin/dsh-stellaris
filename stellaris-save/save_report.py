# -*- coding: utf-8 -*-
"""Read a Stellaris save and report where each fleet calls home.

为什么从存档读：舰队母港（`fleet_template.home_base.orbitable.starbase`）在
stellaris-mcp 的接口里没有暴露，在内存里又是“持久引用”，猜偏移容易错；存档里
写的是明文，读它不会碰游戏进程，也就不会崩。

用法：
    python save_report.py [存档路径] [--owner 国家id] [--ships]
不给路径时用最新的一份。
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paradox_script as ps

SAVE_DIR = os.path.join(os.environ.get("USERPROFILE", ""), "Documents",
                        "Paradox Interactive", "Stellaris", "save games")
INVALID = 4294967295


def newest_save() -> str:
    files = glob.glob(os.path.join(SAVE_DIR, "**", "*.sav"), recursive=True)
    if not files:
        raise SystemExit(f"没有找到存档：{SAVE_DIR}")
    return max(files, key=os.path.getmtime)


def load_gamestate(path: str) -> str:
    if not path.lower().endswith(".sav"):
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            return f.read()
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if "gamestate" in n)
        return z.read(name).decode("utf-8", errors="ignore")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("save", nargs="?")
    ap.add_argument("--owner", type=int)
    ap.add_argument("--all", action="store_true", help="不按玩家过滤")
    args = ap.parse_args()

    path = args.save or newest_save()
    text = load_gamestate(path)
    print(f"存档：{path}")
    print(f"版本：{ps.value(text, 'version')}   日期：{ps.value(text, 'date')}")

    templates = ps.blocks(ps.section(text, 'fleet_template'))
    fleets = ps.blocks(ps.section(text, 'fleet'))
    starbases = ps.blocks(ps.section(text, 'starbase_mgr') and ps.sub(ps.section(text, 'starbase_mgr'), 'starbases'))
    systems = ps.blocks(ps.section(text, 'galactic_object'))

    # 星港 id → 所在星系 id
    starbase_system = {}
    for system_id, system in systems.items():
        body = ps.sub(system, 'starbases')          # 形如 " 0 1 4294967295 "
        for item in re.findall(r'\d+', body):
            value = int(item)
            if value != INVALID:
                starbase_system.setdefault(value, system_id)

    def system_name(system_id):
        system = systems.get(system_id)
        return ps.name_of(system) if system else ''

    def starbase_label(starbase_id):
        base = starbases.get(starbase_id)
        if not base:
            return f"星港 {starbase_id}（存档缺失）"
        level = ps.value(base, 'level')
        kind = ps.value(base, 'type')
        modules = ps.sub(base, 'modules')           # 形如 " 0=shipyard 1=solar_panel_network "
        shipyards = len(re.findall(r'\bshipyard\b', modules))
        system_id = starbase_system.get(starbase_id)
        place = system_name(system_id) if system_id is not None else '?'
        return f"{place}（{level}／{kind}，船坞 {shipyards}）"

    print()
    print(f"{'舰队':>22}  {'id':>4}  {'模板':>4}  母港")
    print('-' * 78)

    rows = 0
    for fleet_id, fleet in sorted(fleets.items()):
        template_id = ps.value(fleet, 'fleet_template')
        if not isinstance(template_id, int):
            continue
        template = templates.get(template_id)
        if not template:
            continue
        home = ps.sub(template, 'home_base')
        orbitable = ps.sub(home, 'orbitable') if home else ''
        if not orbitable:
            continue
        starbase_id = ps.value(orbitable, 'starbase')
        planet_id = ps.value(orbitable, 'planet')
        if isinstance(starbase_id, int) and starbase_id != INVALID:
            target = starbase_label(starbase_id)
        elif isinstance(planet_id, int) and planet_id != INVALID:
            target = f"行星 {planet_id}"
        else:
            target = '（无）'
        name = ps.name_of(fleet) or ps.value(fleet, 'name') or ''
        print(f"{str(name)[:20]:>22}  {fleet_id:>4}  {template_id:>4}  {target}")
        rows += 1

    print()
    print(f"共 {rows} 支有模板的舰队；模板 {len(templates)} 个，星港 {len(starbases)} 座，星系 {len(systems)} 个")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

