# -*- coding: utf-8 -*-
"""读殖民地岗位分配（含格式塔/蜂巢/机械智能的那套“子个体”岗位）。

为什么需要它：桥里的 `get_planet_jobs` 返回的是**普通政体**岗位表（农夫、矿工、技工…），
对格式塔国家全是 0。存档里的 `pop_jobs` 才是真的：按**殖民地 id** 索引，字段是
`type` / `workforce` / `max_workforce`。

用法：
    python jobs_report.py [存档路径] [--all]

输出示例：
    水精之室（planet 14 / colony 0）
        技工子个体       technician_drone     450 / 450
        采矿子个体       mining_drone         600 / 600
        …
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paradox_script as ps

SAVE_DIR = os.path.join(os.environ.get("USERPROFILE", ""), "Documents",
                        "Paradox Interactive", "Stellaris", "save games")

# 常见岗位的中文名（格式塔那套优先，普通政体也顺手补几个）
NAMES = {
    # 格式塔
    "replicator": "复制者（繁殖子个体）",
    "coordinator": "协调者",
    "maintenance_drone": "维护子个体",
    "technician_drone": "技工子个体",
    "mining_drone": "采矿子个体",
    "agri_drone": "农业子个体",
    "artisan_drone": "工匠子个体",
    "fabricator": "铸造子个体",
    "calculator_physicist": "物理学子个体",
    "calculator_biologist": "生物学子个体",
    "calculator_engineer": "工程学子个体",
    "patrol_drone": "巡逻子个体",
    "soldier_drone": "作战子个体",
    "bio_trophy": "活体陈设",
    "bio_trophy_unemployment": "活体陈设（失业）",
    "synapse_drone": "突触子个体",
    "brain_drone": "脑部子个体",
    # 普通政体（对照）
    "farmer": "农夫",
    "miner": "矿工",
    "technician": "技工",
    "artisan": "工匠",
    "metallurgist": "冶金师",
    "researcher": "研究员",
    "soldier": "士兵",
    "enforcer": "执法者",
    "politician": "政客",
    "bureaucrat": "官僚",
    "clerk": "职员",
    "unemployed": "失业",
}


def newest_save() -> str:
    files = glob.glob(os.path.join(SAVE_DIR, "**", "*.sav"), recursive=True)
    if not files:
        raise SystemExit(f"没有找到存档：{SAVE_DIR}")
    return max(files, key=os.path.getmtime)


def gamestate(path: str) -> str:
    import zipfile
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if "gamestate" in n)
        return z.read(name).decode("utf-8", errors="ignore")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("save", nargs="?")
    ap.add_argument("--all", action="store_true", help="包含所有国家的殖民地")
    args = ap.parse_args()

    path = args.save or newest_save()
    text = gamestate(path)
    print(f"存档：{path}")
    print(f"日期：{ps.value(text, 'date')}")

    planets = ps.blocks(ps.section(text, "planets"))
    jobs = ps.blocks(ps.section(text, "pop_jobs"))

    colony_planet = {}
    for pid, block in planets.items():
        cid = ps.value(block, "colony")
        if isinstance(cid, int) and cid != 4294967295:
            colony_planet[cid] = (pid, ps.name_of(block) or ps.value(block, "name") or str(pid))

    country = ps.blocks(ps.section(text, 'country')).get(0, '')
    player_colonies = set()
    body = ps.sub(country, 'owned_planets')
    if body:
        import re
        player_colonies = {int(x) for x in re.findall(r'\d+', body)}
    if not player_colonies:
        print('（没读到自己国家的殖民地列表，改列全部）')
        args.all = True

    by_colony = defaultdict(list)
    for jid, block in jobs.items():
        cid = ps.value(block, "planet")          # 注意：这里其实是 colony id
        if not isinstance(cid, int):
            continue
        workforce = ps.value(block, "workforce") or 0
        limit = ps.value(block, "max_workforce") or 0
        if workforce <= 0 and limit <= 0:
            continue
        if not args.all and cid not in player_colonies:
            continue
        by_colony[cid].append((ps.value(block, "type"), workforce, limit))

    for cid in sorted(by_colony):
        planet, name = colony_planet.get(cid, ("?", f"colony {cid}"))
        rows = sorted(by_colony[cid], key=lambda r: -r[1])
        print(f"\n{name}（planet {planet} / colony {cid}）")
        for job, workforce, limit in rows:
            zh = NAMES.get(str(job), "")
            print(f"    {zh or job:<14} {str(job):<22} {workforce:>5} / {limit if limit >= 0 else '—'}")

    print(f"\n共 {len(by_colony)} 个殖民地有岗位数据")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
