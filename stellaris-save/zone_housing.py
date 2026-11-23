# -*- coding: utf-8 -*-
"""列出每种 zone 特化的住房消耗（静态解析游戏定义）。

用法：python zone_housing.py [zone_key ...]
"""
from __future__ import annotations

import glob
import os
import re
import sys

GAME = r'<Steam 库>\steamapps\common\Stellaris'
ZONES = os.path.join(GAME, 'common', 'zones')
INLINE = os.path.join(GAME, 'common', 'inline_scripts')
VARS = os.path.join(GAME, 'common', 'scripted_variables')


def read(path):
    with open(path, encoding='utf-8', errors='ignore') as f:
        return f.read()


def load_vars():
    table = {}
    for path in glob.glob(os.path.join(VARS, '*.txt')):
        for m in re.finditer(r'(?m)^\s*@(\w+)\s*=\s*(-?[\d.]+)', read(path)):
            table[m.group(1)] = float(m.group(2))
    return table


def blocks(text):
    out = {}
    for m in re.finditer(r'(?m)^([a-z][a-z_0-9]*)\s*=\s*\{', text):
        key = m.group(1)
        start = m.end() - 1
        depth = 0
        for i in range(start, len(text)):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    out[key] = text[start + 1:i]
                    break
    return out


def inline_refs(body):
    """取出 inline_script = { script = xxx  VAR = value ... }"""
    refs = []
    for m in re.finditer(r'inline_script\s*=\s*\{', body):
        start = m.end() - 1
        depth = 0
        for i in range(start, len(body)):
            if body[i] == '{':
                depth += 1
            elif body[i] == '}':
                depth -= 1
                if depth == 0:
                    inner = body[start + 1:i]
                    script = re.search(r'script\s*=\s*([\w/]+)', inner)
                    variables = dict(re.findall(r'(?m)^\s*([A-Z_]+)\s*=\s*([@\w.]+)', inner))
                    if script:
                        refs.append((script.group(1), variables))
                    break
    return refs


def housing_in(text, variables, depth=0):
    """递归读模板，收集 planet_housing_add 的值。"""
    for k, v in variables.items():
        if k == '__vars__' or not isinstance(v, str):
            continue
        text = text.replace('$%s$' % k, v)
    out = 0.0
    for m in re.finditer(r'planet_housing_add\s*=\s*(-?@?\w+)', text):
        raw = m.group(1)
        if raw.startswith('@'):
            out += variables.get('__vars__', {}).get(raw[1:], 0.0)
        else:
            try:
                out += float(raw)
            except ValueError:
                pass
    if depth < 3:
        for script, sub in inline_refs(text):
            path = os.path.join(INLINE, script + '.txt')
            if os.path.exists(path):
                out += housing_in(read(path), sub, depth + 1)
    return out


def main():
    vs = load_vars()
    wants = sys.argv[1:]
    rows = []
    for path in glob.glob(os.path.join(ZONES, '*.txt')):
        text = read(path)
        for key, body in blocks(text).items():
            if not key.startswith('zone_'):
                continue
            if wants and key not in wants:
                continue
            total = 0.0
            for m in re.finditer(r'planet_housing_add\s*=\s*(-?@?\w+)', body):
                raw = m.group(1)
                total += vs.get(raw[1:], 0.0) if raw.startswith('@') else float(raw)
            for script, sub in inline_refs(body):
                sub2 = dict(sub)
                sub2['__vars__'] = vs
                total += housing_in(read(os.path.join(INLINE, script + '.txt')), sub2) if os.path.exists(os.path.join(INLINE, script + '.txt')) else 0.0
            rows.append((key, total))

    seen = {}
    for key, val in rows:
        seen.setdefault(key, val)
    for key in sorted(seen):
        print('%-42s %+g' % (key, seen[key]))


if __name__ == '__main__':
    main()
