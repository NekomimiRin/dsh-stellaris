# -*- coding: utf-8 -*-
"""把存档里的舰船设计导出成模板，或把模板注入别的存档（新开一局也能用）。

为什么要它：设计只存在于存档里，新开一局没有；但设计定义在存档里是纯文本，
段模板与部件都按名字写，所以可以搬。

用法：
    python design_templates.py export <存档> <设计id> [<设计id> ...]
    python design_templates.py shell [--no-core] [模板文件名 ...]
    python design_templates.py inject <目标存档> <输出存档> [模板文件名 ...]

export 会写到同目录的 templates/ 下，一个设计一个 .txt，外加 manifest.json。
shell 会写出「空壳」版：段与槽原样保留，可选部件全清（--no-core 连 required_component 也清），
输出到 templates/shell/，另附 manifest.json。空壳是为了在局内用 create/update 按科技水平填部件——
段搬不过去，部件搬得动。
inject 会：写进 ship_design 段 → 登记到玩家国的设计列表 → 自检。

**inject 的 id 一律沿用原块，不要自己造**：实测自己造的新 id 会被游戏拒，
载入直接崩在 `Malformed token`（2026-11-22，三种造法都试过）。要换段就改
`template="..."` 那一行，别碰 id。细节见 README「注入存档：格式上踩过的坑」。

注意事项（都踩过）：
  * 块首通常是 "\\t1839="，正则必须允许前导空白，而且**不能用 `\\s`**（`\\s` 会吃掉换行，
    导出的块前面会多一个空行）
  * 名字（key）在同一个国家里不能重；字面量名字要带 `literal=yes`
  * 必须同时登记到玩家国的 ship_design_collection，否则设计在库里但不可用
  * 科技未解锁时设计会保留但不出现、不可造（get_ship_designs 按舰级过滤）
  * 控制台 rr 只填"已解锁"的资源；稀有水晶之类要先研究对应科技
"""

import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paradox_script as ps

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, "templates")


def gamestate(path):
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if "gamestate" in n)
        return z.read(name).decode("utf-8")


def write_sav(target, out, new_gamestate):
    with zipfile.ZipFile(target) as z:
        names = z.namelist()
        parts = {n: z.read(n) for n in names}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            z.writestr(n, new_gamestate.encode("utf-8") if n == "gamestate" else parts[n])


def design_block(text, design_id):
    section = ps.section(text, "ship_design")
    match = re.search(r"(?m)^[ \t]*%d[ \t]*=" % design_id, section)
    if not match:
        return None
    start = match.start()
    open_index = section.find("{", start)
    close_index = ps._match_brace(section, open_index)
    return section[start:close_index + 1]


def cmd_export(save, ids):
    text = gamestate(save)
    os.makedirs(TEMPLATES, exist_ok=True)
    manifest_path = os.path.join(TEMPLATES, "manifest.json")
    manifest = []
    if os.path.exists(manifest_path):
        manifest = json.load(open(manifest_path, encoding="utf-8"))

    for design_id in ids:
        block = design_block(text, design_id)
        if not block:
            print("设计 %d 不在这个存档里" % design_id)
            continue
        name = ps.value(ps.sub(block, "name"), "key") or str(design_id)
        size = ps.value(block, "ship_size") or ""
        file_name = "%s_%s.txt" % (design_id, re.sub(r"[^\w\-]+", "_", name))
        with open(os.path.join(TEMPLATES, file_name), "w", encoding="utf-8") as f:
            f.write(block)
        manifest = [m for m in manifest if m.get("file") != file_name]
        manifest.append({"file": file_name, "design_id": design_id, "name": name, "ship_size": size})
        print("导出 %d %s (%s) → %s" % (design_id, name, size, file_name))

    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)


SHELL_DIR = os.path.join(TEMPLATES, "shell")


def shell_out_dir(keep_cores):
    return SHELL_DIR if keep_cores else os.path.join(TEMPLATES, "shell-nocore")

# `component = { ... }` inside a section. Anchor on the exact key: `required_component=`
# must not match, so no `\s*` before it.
COMPONENT_BLOCK = re.compile(r"(?m)^[ \t]*component[ \t]*=[ \t]*\r?\n?[ \t]*\{")
REQUIRED_LINE = re.compile(r"(?m)^[ \t]*required_component[ \t]*=[ \t]*\"[^\"]*\"[ \t]*\r?\n")


def _strip_blocks(text):
    """Remove every `component = { ... }` block from `text` (brace matched, not regex)."""
    out = text
    while True:
        match = COMPONENT_BLOCK.search(out)
        if not match:
            break
        open_index = out.find("{", match.start())
        end = ps._match_brace(out, open_index) + 1
        if out[end:end + 2] == "\r\n":
            end += 2
        elif out[end:end + 1] == "\n":
            end += 1
        out = out[:match.start()] + out[end:]
    return out


def strip_components(block, keep_cores=True):
    """Drop every component block in the design; keep sections (template/slot) intact."""
    out = _strip_blocks(block)
    if not keep_cores:
        out = REQUIRED_LINE.sub("", out)
    return out


MUTATION_HDR = re.compile(r'(?m)^[ \t]*template[ \t]*=[ \t]*"mutation_section"')


def strip_mutation_only(block):
    """Fauna: the hull's own section keeps its components, only mutation_section's ANY_* go empty.

    游戏自己写出来的兽群空壳就是这个形状：本体段一个部件不少，`mutation_section` 整段留着、
    里面一条 component 都没有（2026-11-22 实测「空匣晶态实体」）。
    """
    out = block
    match = MUTATION_HDR.search(out)
    if not match:
        return out
    # 段头 `section=` 在 template 行之前，所以往回找它的 `{`，不能从 template 往后找
    section_at = out.rfind("section", 0, match.start())
    open_index = out.find("{", section_at if section_at >= 0 else match.start())
    close_index = ps._match_brace(out, open_index)
    inner = _strip_blocks(out[open_index + 1:close_index])
    return out[:open_index + 1] + inner + out[close_index:]


def cmd_shell(files, keep_cores=True, fauna=False):
    manifest_path = os.path.join(TEMPLATES, "manifest.json")
    manifest = []
    if os.path.exists(manifest_path):
        manifest = json.load(open(manifest_path, encoding="utf-8"))
    if not files:
        files = [m["file"] for m in manifest]
    out_dir = shell_out_dir(keep_cores)
    os.makedirs(out_dir, exist_ok=True)

    out_manifest = []
    for file_name in files:
        path = file_name if os.path.isabs(file_name) else os.path.join(TEMPLATES, file_name)
        if not os.path.exists(path):
            print("找不到 %s" % file_name)
            continue
        block = open(path, encoding="utf-8").read()
        shell = strip_mutation_only(block) if fauna else strip_components(block, keep_cores)
        name = os.path.basename(path)
        with open(os.path.join(out_dir, name), "w", encoding="utf-8") as f:
            f.write(shell)
        entry = next((m for m in manifest if m["file"] == name), {})
        out_manifest.append(dict(entry, file=name, shell=True, keep_cores=keep_cores,
                                 fauna=fauna, bytes_before=len(block), bytes_after=len(shell)))
        print("空壳 %s  %d → %d bytes%s" % (name, len(block), len(shell), "（只清 mutation_section）" if fauna else ""))

    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(out_manifest, f, ensure_ascii=False, indent=2)
    print("写出 %s（%d 个）" % (out_dir, len(out_manifest)))


def cmd_inject(target, out, files):
    text = gamestate(target)
    designs_section = ps.section(text, "ship_design")
    taken = set(int(x) for x in re.findall(r"(?m)^[ \t]*(\d+)[ \t]*=", designs_section))
    next_id = max(taken) + 1

    collection = None
    country = ps.section(text, "country")
    player = ps.blocks(country).get(0)
    if player:
        collection = ps.sub(player, "ship_design_collection")

    added = []
    for file_name in files:
        path = file_name if os.path.isabs(file_name) else os.path.join(TEMPLATES, file_name)
        block = open(path, encoding="utf-8").read()
        match = re.search(r"(?m)^[ \t]*(\d+)[ \t]*=", block)
        if not match:
            print("跳过 %s：找不到 id" % file_name)
            continue
        old_id = int(match.group(1))
        while next_id in taken:
            next_id += 1
        new_block = re.sub(r"^([ \t]*)%d([ \t]*)=" % old_id,
                           lambda m: "%s%d%s=" % (m.group(1), next_id, m.group(2)), block, count=1)
        assert ("%d=" % next_id) in new_block

        anchor = re.search(r"(?m)^ship_design=", text)
        a_open = text.find("{", anchor.start())
        a_close = ps._match_brace(text, a_open)
        text = text[:a_close] + "\n" + new_block + "\n" + text[a_close:]

        if collection:
            m2 = re.search(r"ship_design=\s*\n?\s*\{[^}]*\}", collection)
            if m2:
                old_list = m2.group(0)
                new_list = old_list.rstrip("}").rstrip() + "\n\t\t\t\t%d \n\t\t\t}" % next_id
                collection_new = collection.replace(old_list, new_list, 1)
                text = text.replace(collection, collection_new, 1)
                collection = collection_new

        added.append((old_id, next_id))
        taken.add(next_id)
        next_id += 1

    write_sav(target, out, text)
    print("写出 %s（%d bytes）" % (out, os.path.getsize(out)))
    for old_id, new_id in added:
        print("  设计 %d → %d" % (old_id, new_id))

    # 自检
    check = ps.blocks(ps.section(gamestate(out), "ship_design"))
    for _, new_id in added:
        print("  自检：%d 在存档里 = %s" % (new_id, new_id in check))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    if cmd == "export":
        if len(sys.argv) < 4:
            print(__doc__)
            return 1
        cmd_export(sys.argv[2], [int(x) for x in sys.argv[3:]])
    elif cmd == "shell":
        rest = sys.argv[2:]
        cmd_shell([x for x in rest if not x.startswith("--")],
                  keep_cores="--no-core" not in rest,
                  fauna="--fauna" in rest)
    elif cmd == "inject":
        if len(sys.argv) < 4:
            print(__doc__)
            return 1
        cmd_inject(sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
