# -*- coding: utf-8 -*-
"""从游戏定义里提取每种殖民地设计的加成效果。

用法：python colony_effects.py [设计键...]
不加参数则列出全部。
"""
from __future__ import annotations

import glob
import os
import re
import sys

GAME = r'<Steam 库>\steamapps\common\Stellaris'
TYPES = os.path.join(GAME, 'common', 'colony_types')
INLINE = os.path.join(GAME, 'common', 'inline_scripts')


def read(path):
    with open(path, encoding='utf-8', errors='ignore') as f:
        return f.read()


def blocks(text):
    """顶层 `key = { ... }` 切块。"""
    out = {}
    for m in re.finditer(r'(?m)^([a-z][a-z_0-9]*)\s*=\s*\{', text):
        key = m.group(1)
        start = m.end() - 1
        depth = 0
        i = start
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    out[key] = text[start + 1:i]
                    break
            i += 1
    return out


def inline_subs(text):
    """把 inline_script 展开成 (模板名, 变量字典) 列表。"""
    out = []
    for m in re.finditer(r'inline_script\s*=\s*\{', text):
        start = m.end() - 1
        depth = 0
        i = start
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    body = text[start + 1:i]
                    script = re.search(r'script\s*=\s*([\w/]+)', body)
                    variables = dict(re.findall(r'(?m)^\s*([A-Z_]+)\s*=\s*([\w.]+)', body))
                    if script:
                        out.append((script.group(1), variables))
                    break
            i += 1
    return out


def resolve_inline(script, variables, depth=0):
    """读模板并代入变量，返回 (修饰符列表)。"""
    path = os.path.join(INLINE, script + '.txt')
    if not os.path.exists(path) or depth > 3:
        return []
    text = read(path)
    for k, v in variables.items():
        text = text.replace('$%s$' % k, v)
    out = []
    # planet_modifier = { key = value }
    for m in re.finditer(r'(?m)^\s*planet_modifier\s*=\s*\{([^}]*)\}', text):
        for mm in re.finditer(r'([a-z_0-9]+)\s*=\s*(-?[\d.]+)', m.group(1)):
            if mm.group(1) != 'add':
                out.append((mm.group(1), float(mm.group(2))))
    # triggered_planet_modifier 里的 modifier = { key = value }
    for m in re.finditer(r'triggered_planet_modifier\s*=\s*\{', text):
        start = m.end() - 1
        depth = 0
        i = start
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    body = text[start + 1:i]
                    for mm in re.finditer(r'modifier\s*=\s*\{([^}]*)\}', body):
                        for kv in re.finditer(r'([a-z_0-9]+)\s*=\s*(-?[\d.]+)', mm.group(1)):
                            if kv.group(1) != 'add':
                                out.append((kv.group(1), float(kv.group(2))))
                    break
            i += 1
    for sub_script, sub_vars in inline_subs(text):
        out += resolve_inline(sub_script, sub_vars, depth + 1)
    return out


def direct_modifiers(body):
    out = []
    for m in re.finditer(r'(?m)^\s*planet_modifier\s*=\s*\{([^}]*)\}', body):
        inner = m.group(1)
        for mm in re.finditer(r'([a-z_0-9]+)\s*=\s*(-?[\d.]+)', inner):
            out.append((mm.group(1), float(mm.group(2))))
    return out


def effects_of(key):
    for path in glob.glob(os.path.join(TYPES, '*.txt')):
        text = read(path)
        blk = blocks(text).get(key)
        if blk is None:
            continue
        mods = direct_modifiers(blk)
        for script, variables in inline_subs(blk):
            mods += resolve_inline(script, variables)
        return mods
    return None


def main():
    wants = sys.argv[1:]
    if not wants:
        keys = []
        for path in glob.glob(os.path.join(TYPES, '*.txt')):
            keys += list(blocks(read(path)).keys())
        wants = sorted(set(keys))
    for key in wants:
        mods = effects_of(key)
        if mods is None:
            print(key, '→ （找不到定义）')
            continue
        if not mods:
            print(key, '→ （无基础加成）')
            continue
        def fmt(m, v):
            # _mult 是倍率（0.15 = +15%）；_add 是绝对值（1000 就是 +1000）
            if m.endswith('_mult'):
                return '%s %+g%%' % (m, v * 100)
            return '%s %+g' % (m, v)

        print(key, '→', '；'.join(fmt(m, v) for m, v in mods))


if __name__ == '__main__':
    main()
