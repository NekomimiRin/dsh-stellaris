# -*- coding: utf-8 -*-
"""联邦现状：直接从存档的 `federation=` 段读，不碰游戏进程。

存档里的样子（2026-11-21 实测）：

    federation=
    {
        0=
        {
            name={ key="%ADJ%" variables={ ... key="Pan-Galactic" ... } }
            federation_progression=
            {
                federation_type="research_federation"
                experience=9713.32
                base_cohesion=0
                cohesion=-100
                laws=
                {
                    law_category_succession_term=succession_term_years_20
                    law_category_centralization=centralization_high
                    ...
                }
                ...
            }
        }
    }

为什么走存档：2026-11-21 实测**法律切换不经过命令系统**（投票通过那一瞬 cmd_capture 只抓到
`CDiploActionVoteCommand`），内存里也还没找到「当前法律」的存放处；而存档里 laws 是明文键值。

当天晚些又补了一次阴性验证（两个法律不同的联邦做对照）：`CFederation` 对象 0x280 字节全量 diff，
32 处不同全是 id / 指针 / 成员数 / 经验团结度一类，没有「每个类别一个索引」的块；progression 前 0x800
里按 vtable 找 `CFederationLaw` 指针，候选 52 个命中 0。所以要读当前值，还要先反查触发器
`has_federation_law` 的实现。

用法：
    python federation_report.py [存档路径] [--raw] [--loc-dir DIR] [--no-loc]

不给路径时用最新的一份；默认输出可读文本，`--raw` 输出 JSON。

名字一律取自游戏的本地化文件（`localisation/simp_chinese/*.yml`），**不再维护手写对照表**：
类别 `law_category_centralization`、值 `centralization_high`、联邦类型 `research_federation`
在 `federations_l_simp_chinese.yml` 里都有，取到的就是界面措辞（如「联邦集权 = 高」）。
找不到键时退回脚本键本身；`--no-loc` 让全部退回键（写脚本、对账时用）。
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
import zipfile

SAVE_DIR = os.path.join(os.environ.get("USERPROFILE", ""), "Documents",
                        "Paradox Interactive", "Stellaris", "save games")

GAME_LOC_CANDIDATES = (
    r"<Steam 库>\steamapps\common\Stellaris\localisation\simp_chinese",
    r"C:\Program Files (x86)\Steam\steamapps\common\Stellaris\localisation\simp_chinese",
    r"D:\SteamLibrary\steamapps\common\Stellaris\localisation\simp_chinese",
)

_LOC_LINE = re.compile(r'^\s*([A-Za-z0-9_.\-]+):\d?\s*"((?:[^"]|"")*)"')
_COLOR = re.compile(r"§[A-Za-z!]|\$[A-Za-z0-9_.\-]+\$")


def loc_dir(explicit: str | None) -> str | None:
    """本地化目录：--loc-dir > 环境变量 STELLARIS_LOC > 已知安装路径。"""
    for cand in (explicit, os.environ.get("STELLARIS_LOC"), *GAME_LOC_CANDIDATES):
        if cand and os.path.isdir(cand):
            return cand
    return None


def load_loc(directory: str | None) -> dict[str, str]:
    """把整个 simp_chinese 目录的 yml 读成 key -> 原文（约 10 万条、一秒内）。"""
    out: dict[str, str] = {}
    if not directory:
        return out
    for name in os.listdir(directory):
        if not name.endswith(".yml"):
            continue
        try:
            with open(os.path.join(directory, name), "r", encoding="utf-8",
                      errors="replace") as fh:
                for line in fh:
                    m = _LOC_LINE.match(line)
                    if m:
                        out[m.group(1)] = m.group(2).replace('""', '"')
        except OSError:
            continue
    return out


def clean(text: str) -> str:
    return _COLOR.sub("", text).strip()


def name_of(key: str, loc: dict[str, str]) -> str:
    """键 -> 界面名；没有这条本地化就原样返回键。"""
    if not key:
        return key
    return clean(loc.get(key, "")) or key



def newest_save() -> str:
    files = glob.glob(os.path.join(SAVE_DIR, "**", "*.sav"), recursive=True)
    if not files:
        raise SystemExit(f"没有找到存档：{SAVE_DIR}")
    return max(files, key=os.path.getmtime)


def read_gamestate(path: str) -> bytes:
    with zipfile.ZipFile(path) as z:
        return z.read("gamestate")


def block_at(text: str, start: int) -> str:
    """从 start 处的 `{` 开始，按花括号配对取整段（跳过字符串里的括号）。"""
    assert text[start] == "{"
    depth = 0
    in_str = False
    i = start
    while i < len(text):
        c = text[i]
        if in_str:
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise ValueError("花括号不配对")


def compose_name(name_block: str) -> str:
    keys = re.findall(r'key="([^"]*)"', name_block)
    parts = [k for k in keys if k and not k.startswith("%") and not k.isdigit()]
    return " ".join(parts)


def parse_federations(text: str) -> list[dict]:
    m = re.search(r"(?m)^federation=\s*\n\{", text)
    if not m:
        return []
    brace = text.index("{", m.start())
    body = block_at(text, brace)
    out = []
    # 直接子节点：`\n\t<digits>=\n\t{`
    for child in re.finditer(r"(?m)^\t(\d+)=\s*\n\t\{", body):
        fid = int(child.group(1))
        child_brace = body.index("{", child.start())
        try:
            cbody = block_at(body, child_brace)
        except ValueError:
            continue
        item: dict = {"id": fid}
        nm = re.search(r"(?ms)^\t\tname=\s*\n\t\t\{", cbody)
        if nm:
            nb = cbody.index("{", nm.start())
            try:
                item["name"] = compose_name(block_at(cbody, nb))
            except ValueError:
                item["name"] = ""
        prog = re.search(r"(?ms)federation_progression=\s*\n\t\t\{", cbody)
        if not prog:
            out.append(item)
            continue
        pbrace = cbody.index("{", prog.start())
        try:
            pbody = block_at(cbody, pbrace)
        except ValueError:
            out.append(item)
            continue
        for key, field in (("federation_type", "type"), ("experience", "experience"),
                           ("base_cohesion", "base_cohesion"), ("cohesion", "cohesion")):
            fm = re.search(rf"(?m)^\t\t\t{key}=\"?([^\s\"]+)\"?", pbody)
            if fm:
                val = fm.group(1)
                try:
                    item[field] = float(val)
                except ValueError:
                    item[field] = val
        laws: dict = {}
        lm = re.search(r"(?ms)laws=\s*\n\t\t\t\{", pbody)
        if lm:
            lbrace = pbody.index("{", lm.start())
            try:
                lbody = block_at(pbody, lbrace)
            except ValueError:
                lbody = ""
            for cat, val in re.findall(r"(?m)^\t+([A-Za-z0-9_]+)=([A-Za-z0-9_]+)$", lbody):
                if cat.startswith("law_category_"):
                    laws[cat] = val
        if laws:
            item["laws"] = laws
        out.append(item)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?")
    ap.add_argument("--raw", action="store_true", help="输出 JSON")
    ap.add_argument("--loc-dir", help="本地化目录（默认自动找 simp_chinese）")
    ap.add_argument("--no-loc", action="store_true", help="不查本地化，键原样输出")
    args = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    path = args.path or newest_save()
    loc = {} if args.no_loc else load_loc(loc_dir(args.loc_dir))
    text = read_gamestate(path).decode("latin1")
    feds = parse_federations(text)
    for f in feds:
        f["type_name"] = name_of(str(f.get("type", "")), loc)
        f["laws_named"] = [
            {"category": cat, "category_name": name_of(cat, loc),
             "value": val, "value_name": name_of(val, loc)}
            for cat, val in (f.get("laws") or {}).items()
        ]
    if args.raw:
        print(json.dumps({"success": True, "save": path, "count": len(feds),
                          "localised": bool(loc), "items": feds},
                         ensure_ascii=False, indent=2))
        return
    if not args.no_loc and not loc:
        print("（找不到本地化目录，名字会退回脚本键；用 --loc-dir 指路，或 --no-loc 明确关掉）")
    print(f"存档：{os.path.basename(path)}")
    print(f"联邦 {len(feds)} 个")
    for f in feds:
        print(f"\n#{f['id']}「{f.get('name') or '（无名）'}」"
              f"｜{f['type_name']}｜经验 {f.get('experience', '?')}｜团结度 {f.get('cohesion', '?')}")
        for row in f["laws_named"]:
            print(f"    {row['category_name']:<8} = {row['value_name']}")


if __name__ == "__main__":
    main()
