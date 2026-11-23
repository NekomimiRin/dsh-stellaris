# -*- coding: utf-8 -*-
"""已研究科技：从存档的 `tech_status` 里读，不碰游戏进程。

存档里每个国家有一段：

    tech_status=
        {
            technology="tech_corvettes"
            level=1
            technology="tech_solar_panel_network"
            level=1
            ...
        }

用法：
    python tech_report.py [存档路径] [--country 0]
不给路径时用最新的一份；输出 JSON。
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paradox_script as ps

SAVE_DIR = os.path.join(os.environ.get("USERPROFILE", ""), "Documents",
                        "Paradox Interactive", "Stellaris", "save games")


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
        return z.read(name).decode("utf-8", "ignore")


def parse_tech_status(body: str) -> list[dict]:
    """`technology="x" level=N` 成对出现，顺序稳定。"""
    out: list[dict] = []
    tech = None
    for raw in body.splitlines():
        line = raw.strip().rstrip(",")
        if not line or line in ("{", "}"):
            continue
        m = re.match(r'^technology\s*=\s*"([^"]+)"', line)
        if m:
            tech = m.group(1)
            continue
        m = re.match(r"^level\s*=\s*(-?\d+)", line)
        if m and tech:
            out.append({"key": tech, "level": int(m.group(1))})
            tech = None
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("save", nargs="?", default=None)
    ap.add_argument("--country", type=int, default=0)
    args = ap.parse_args()

    path = args.save or newest_save()
    text = load_gamestate(path)

    country = ps.sub(ps.section(text, "country"), str(args.country))
    if not country:
        raise SystemExit(f"存档里没有国家 {args.country}")

    body = ps.sub(country, "tech_status")
    techs = [t for t in parse_tech_status(body) if t["level"] >= 1]

    print(json.dumps({
        "save": os.path.basename(path),
        "country": args.country,
        "count": len(techs),
        "technologies": techs,
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
