# -*- coding: utf-8 -*-
"""Read the construction queues of one's own starbases from a Stellaris save.

队列数据在 `construction.queue_mgr.queues.{id}`：
    items={ <ship id> ... }   ← 造船队列里排着的舰船
    owner=<country id>
    location={ type=0 id=<starbase id> }   (type 0 = 星港, 2 = 行星)
    type=ships / starbase / army / planet
星港的 `shipyard_build_queue` 字段就是这条队列的 id。
"""
from __future__ import annotations

import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paradox_script as ps
import save_report as sr


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else sr.newest_save()
    text = sr.load_gamestate(path)
    print(f"存档：{path}")
    print(f"版本：{ps.value(text, 'version')}   日期：{ps.value(text, 'date')}")

    construction = ps.sub(ps.section(text, 'construction'), 'queue_mgr')
    queues = ps.blocks(ps.sub(construction, 'queues') or construction)
    starbases = ps.blocks(ps.sub(ps.section(text, 'starbase_mgr'), 'starbases'))
    systems = ps.blocks(ps.section(text, 'galactic_object'))
    ships = ps.blocks(ps.section(text, 'ships'))
    designs = ps.blocks(ps.section(text, 'ship_design'))

    starbase_system = {}
    for system_id, system in systems.items():
        for item in re.findall(r'\d+', ps.sub(system, 'starbases')):
            if int(item) != 4294967295:
                starbase_system.setdefault(int(item), system_id)

    def system_name(system_id):
        system = systems.get(system_id)
        return ps.name_of(system) if system else ''

    print()
    print(f"{'星港':>6}  {'所在星系':<16}  {'队列':>4}  {'同时':>4}  建造中")
    print('-' * 78)
    shown = 0
    for starbase_id, base in sorted(starbases.items()):
        queue_id = ps.value(base, 'shipyard_build_queue')
        if not isinstance(queue_id, int) or queue_id == 4294967295:
            continue
        queue = queues.get(queue_id)
        items = []
        if queue:
            items = [int(x) for x in re.findall(r'\d+', ps.sub(queue, 'items'))]
        simultaneous = ps.value(queue, 'simultaneous') if queue else ''
        place = system_name(starbase_system.get(starbase_id))
        current = ''
        if items:
            ship = ships.get(items[0]) or ''
            design_id = ps.value(ship, 'ship_design') if ship else None
            name = ps.name_of(designs.get(design_id, '')) if isinstance(design_id, int) else ''
            current = f"{name or '?'}（{len(items)} 艘排队）"
        print(f"{starbase_id:>6}  {place:<16}  {queue_id:>4}  {str(simultaneous):>4}  {current}")
        shown += 1

    print()
    print(f"共 {shown} 座星港的造船队列")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
