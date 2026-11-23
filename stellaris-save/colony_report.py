# -*- coding: utf-8 -*-
"""一次存档，解析出所有殖民地的完整配置（JSON）。

用法：
    python colony_report.py <存档路径> [--planet 2174]

输出：{"date": "...", "colonies": [ {...}, ... ]}

字段来源（都在 gamestate 里，全部经过验证）：
    colony     stability / crime / amenities / free_amenities / amenities_usage /
               total_housing / housing_usage / free_housing / employable_pops /
               unemploy_pop / num_sapient_pops / final_designation / ascension_tier /
               pop_groups / pop_jobs / districts / buildings_cache / upkeep
    planets    name / planet_class / planet_size / colonize_date / colony
    districts  type / level
    buildings  type / position
    pop_groups size / species
    pop_jobs   type / workforce / max_workforce / pop_group
    species_db name / class
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import zipfile
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paradox_script as ps


def load_gamestate(path: str) -> str:
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if 'gamestate' in n)
        return z.read(name).decode('utf-8', errors='ignore')


def ids_of(body: str) -> list[int]:
    return [int(x) for x in re.findall(r'\d+', body or '')]


def num(value):
    """存档里的数值可能是 float 字符串，转成 int 或 float。"""
    if value is None:
        return None
    try:
        f = float(value)
    except (TypeError, ValueError):
        return value
    return int(f) if f == int(f) else round(f, 3)


def build_report(text: str, only_planet: int | None = None, include_all: bool = False) -> dict:
    planets = ps.blocks(ps.section(text, 'planets'))
    colonies = ps.blocks(ps.section(text, 'colony'))
    groups = ps.blocks(ps.section(text, 'pop_groups'))
    jobs = ps.blocks(ps.section(text, 'pop_jobs'))
    buildings = ps.blocks(ps.section(text, 'buildings'))
    districts = ps.blocks(ps.section(text, 'districts'))
    species = ps.blocks(ps.section(text, 'species_db'))

    # 玩家自己的殖民地：先读 player 块里记录的国家（切国家后就不是 0 了）
    player_id = 0
    m_player = re.search(r'player\s*=\s*\{', text)
    if m_player:
        depth = 0
        start = m_player.end() - 1
        for i in range(start, min(start + 20000, len(text))):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    body = text[start:i + 1]
                    mm = re.search(r'country\s*=\s*(\d+)', body)
                    if mm:
                        player_id = int(mm.group(1))
                    break

    own = set()
    country = ps.blocks(ps.section(text, 'country')).get(player_id, '')
    if country:
        own = set(ids_of(ps.sub(country, 'owned_planets')))
    if not own:
        include_all = True  # 读不到就退化为全列

    # planet -> colony
    planet_of = {}
    for pid, pblk in planets.items():
        cid = ps.value(pblk, 'colony')
        if isinstance(cid, int) and cid != 0xFFFFFFFF:
            planet_of[cid] = pid

    def species_name(sid):
        entry = species.get(sid, '')
        return ps.name_of(entry) or ps.value(entry, 'name') or str(sid)

    out = []
    for cid, cblk in colonies.items():
        pid = planet_of.get(cid)
        if pid is None:
            continue
        if only_planet is not None and pid != only_planet:
            continue
        if not include_all and cid not in own:
            continue

        pblk = planets.get(pid, '')

        # 人口组
        group_ids = ids_of(ps.sub(cblk, 'pop_groups'))
        group_rows = []
        total_pops = 0
        for gid in group_ids:
            g = groups.get(gid, '')
            size = num(ps.value(g, 'size')) or 0
            sid = ps.value(g, 'species')
            total_pops += size
            group_rows.append({'id': gid, 'species_id': sid,
                               'species': species_name(sid), 'size': size})

        # 岗位（按 colony 索引；pop_jobs 里的 planet 字段其实是 colony id）
        job_rows = []
        for jid, jblk in jobs.items():
            if ps.value(jblk, 'planet') != cid:
                continue
            wf = num(ps.value(jblk, 'workforce')) or 0
            mx = num(ps.value(jblk, 'max_workforce'))
            if wf <= 0 and (mx is None or mx <= 0):
                continue
            job_rows.append({'type': ps.value(jblk, 'type'), 'workforce': wf,
                             'max': mx, 'group': ps.value(jblk, 'pop_group')})
        job_rows.sort(key=lambda r: -(r['workforce'] or 0))

        # 区划
        district_rows = []
        for did in ids_of(ps.sub(cblk, 'districts')):
            d = districts.get(did, '')
            district_rows.append({'type': ps.value(d, 'type'), 'level': num(ps.value(d, 'level'))})
        district_summary = [
            {'type': t, 'count': c, 'levels': sorted({x['level'] for x in district_rows if x['type'] == t})}
            for t, c in Counter(d['type'] for d in district_rows).items()
        ]

        # 建筑
        building_rows = [
            {'type': ps.value(buildings.get(bid, ''), 'type')}
            for bid in ids_of(ps.sub(cblk, 'buildings_cache'))
        ]
        building_summary = [{'type': t, 'count': c} for t, c in Counter(b['type'] for b in building_rows).items()]

        # 维护费
        upkeep = {}
        upkeep_body = ps.sub(cblk, 'upkeep') or ''
        for m in re.finditer(r'([a-z_]+)=(-?[\d.]+)', upkeep_body):
            upkeep[m.group(1)] = num(m.group(2))

        out.append({
            'colony_id': cid,
            'planet_id': pid,
            'name': ps.name_of(pblk) or ps.value(pblk, 'name') or str(pid),
            'planet_class': ps.value(pblk, 'planet_class'),
            'planet_size': num(ps.value(pblk, 'planet_size')),
            'colonize_date': ps.value(pblk, 'colonize_date'),
            'designation': ps.value(cblk, 'final_designation'),
            'ascension_tier': num(ps.value(cblk, 'ascension_tier')),
            'pops': {
                'total': total_pops,
                'sapient': num(ps.value(cblk, 'num_sapient_pops')),
                'employable': num(ps.value(cblk, 'employable_pops')),
                'unemployed': num(ps.value(cblk, 'unemploy_pop')),
            },
            'stability': num(ps.value(cblk, 'stability')),
            'crime': num(ps.value(cblk, 'crime')),
            'amenities': {'value': num(ps.value(cblk, 'amenities')),
                          'usage': num(ps.value(cblk, 'amenities_usage')),
                          'free': num(ps.value(cblk, 'free_amenities'))},
            'housing': {'total': num(ps.value(cblk, 'total_housing')),
                        'usage': num(ps.value(cblk, 'housing_usage')),
                        'free': num(ps.value(cblk, 'free_housing'))},
            'upkeep': upkeep,
            'districts': district_summary,
            'buildings': building_summary,
            'pop_groups': group_rows,
            'jobs': job_rows,
        })

    out.sort(key=lambda c: ('capital' not in str(c['designation']), c['planet_id']))
    return {'date': ps.value(text, 'date'), 'colonies': out}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('save')
    ap.add_argument('--planet', type=int, default=None)
    ap.add_argument('--all', action='store_true')
    args = ap.parse_args()

    report = build_report(load_gamestate(args.save), args.planet, args.all)
    print(json.dumps(report, ensure_ascii=False, indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
