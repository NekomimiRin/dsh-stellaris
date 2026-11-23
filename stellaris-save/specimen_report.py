# -*- coding: utf-8 -*-
"""谁真的持有哪些标本——从存档读，不碰游戏进程。

**关键区分（2026-11-21 实测）**：标本实例块里的 `owner=` 是**发现者**，不是持有者；
持有者要看各国 `standard_grand_archive_module`（`exhibits` 按类别 + `storage_slots`）引用的实例 id。

    specimen_databases / 实例表：  <id>={ exhibit_state=… specimen={ specimen="key" … } owner=<发现者> }
    country.<id>.standard_grand_archive_module：
        exhibits={ aesthetic_wonder={ 16 37 … } historical_item={ … } xeno_geology={ … } }
        storage_slots={ 51 53 … }

用法：
    python specimen_report.py [存档路径] [--country N] [--all] [--json]

不给路径时用最新的一份；默认只列玩家/指定国家。`--all` 列所有非空国家。
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import paradox_script as ps  # noqa: E402

SAVE_DIR = os.path.join(os.environ.get("USERPROFILE", ""), "Documents",
                        "Paradox Interactive", "Stellaris", "save games")


def newest_save() -> str:
    files = glob.glob(os.path.join(SAVE_DIR, "**", "*.sav"), recursive=True)
    if not files:
        raise SystemExit(f"没有找到存档：{SAVE_DIR}")
    return max(files, key=os.path.getmtime)


def gamestate_text(path: str) -> str:
    with zipfile.ZipFile(path) as z:
        return z.read("gamestate").decode("latin1")


def instance_keys(raw: str) -> dict[int, str]:
    """实例 id -> 标本键（只认带 exhibit_state 的那些块）。"""
    out: dict[int, str] = {}
    for m in re.finditer(r"(?m)^\t(\d+)=\s*$", raw):
        seg = raw[m.start(): m.start() + 1200]
        key = re.search(r'specimen="([a-z_0-9]+)"', seg)
        if key and "exhibit_state" in seg:
            out.setdefault(int(m.group(1)), key.group(1))
    return out


def archive_block(block: str) -> str:
    m = re.search(r"standard_grand_archive_module\s*=\s*\{", block)
    if not m:
        return ""
    start = block.index("{", m.start())
    depth = 0
    for i in range(start, len(block)):
        if block[i] == "{":
            depth += 1
        elif block[i] == "}":
            depth -= 1
            if depth == 0:
                return block[start:i + 1]
    return ""


def archive_ids(block: str) -> list[int]:
    body = archive_block(block)
    return [int(x) for x in re.findall(r"(?<![=\w])(\d{1,8})(?=\s)", body)]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?")
    ap.add_argument("--country", type=int, help="只看这个国家（默认 0）")
    ap.add_argument("--all", action="store_true", help="列所有持有标本的国家")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    path = args.path or newest_save()
    raw = gamestate_text(path)
    inst = instance_keys(raw)
    countries = ps.blocks(ps.section(raw, "country"))

    rows = []
    for cid, blk in sorted(countries.items()):
        ids = archive_ids(blk)
        keys = [inst[i] for i in ids if i in inst]
        # 去重后保留重复计数（同一标本可能有多份）
        counts: dict[str, int] = {}
        for k in keys:
            counts[k] = counts.get(k, 0) + 1
        if counts:
            rows.append({"country_id": cid, "total": len(ids), "specimens": counts})

    if args.all:
        picked = rows
    else:
        want = 0 if args.country is None else args.country
        picked = [r for r in rows if r["country_id"] == want]

    if args.json:
        print(json.dumps({"success": True, "save": path, "countries": picked},
                         ensure_ascii=False, indent=2))
        return

    print(f"存档：{os.path.basename(path)}")
    if not picked:
        print("（这个国家没有持有标本，或段名变了）")
        return
    for r in picked:
        names = [k if n == 1 else f"{k}×{n}" for k, n in sorted(r["specimens"].items())]
        print(f"\ncountry {r['country_id']}：{r['total']} 个实例位，{len(names)} 种标本")
        for n in names:
            print("   ", n)


if __name__ == "__main__":
    main()
