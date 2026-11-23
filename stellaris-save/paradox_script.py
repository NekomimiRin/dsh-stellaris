# -*- coding: utf-8 -*-
"""Read Stellaris save data by slicing sections, not by parsing the whole file.

A 9 MB gamestate nests too deeply for a naive parser; and we only need a few
top-level sections (`fleet`, `fleet_template`, `starbase_mgr`, `galactic_object`,
`country`, `ships`). So this module:

  * `section(text, name)`   -> the raw text between the braces of a top-level `name=`
  * `blocks(body)`          -> {id: block_text} for `0={...} 1={...}` bodies
  * `value(block, key)`     -> the first scalar after `key=`
  * `sub(block, key)`       -> the raw text of `key={...}`

Everything stays string-based, so a malformed corner costs one field, not the file.
"""

from __future__ import annotations

import re

_SECTION = re.compile(r'(?m)^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\n?\{')


def _match_brace(text: str, open_index: int) -> int:
    """Index of the brace closing the one at open_index (ignores quoted text)."""
    depth = 0
    i = open_index
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
            i += 1
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def section(text: str, name: str) -> str:
    """The body of the first top-level `name={...}`."""
    for match in _SECTION.finditer(text):
        if match.group(1) != name:
            continue
        open_index = text.rfind('{', match.start(), match.end())
        close_index = _match_brace(text, open_index)
        if close_index < 0:
            return ''
        return text[open_index + 1:close_index]
    return ''


def blocks(body: str) -> dict[int, str]:
    """Split a `0={...} 1={...}` body into {id: block body}."""
    out: dict[int, str] = {}
    i = 0
    n = len(body)
    key = re.compile(r'(\d+)\s*=\s*\{')
    while i < n:
        match = key.search(body, i)
        if not match:
            break
        open_index = match.end() - 1
        close_index = _match_brace(body, open_index)
        if close_index < 0:
            break
        out[int(match.group(1))] = body[open_index + 1:close_index]
        i = close_index + 1
    return out


def value(block: str, key: str):
    """First scalar value of `key=...` (no braces). Returns str/int/None."""
    match = re.search(r'(?<![A-Za-z0-9_])' + re.escape(key) + r'\s*=\s*("(?:[^"]*)"|[^\s{}]+)', block)
    if not match:
        return None
    raw = match.group(1)
    if raw.startswith('"') and raw.endswith('"'):
        return raw[1:-1]
    if re.fullmatch(r'-?\d+', raw):
        return int(raw)
    return raw


def values(block: str, key: str) -> list:
    """Every scalar value of a repeated `key=...`."""
    out = []
    for match in re.finditer(r'(?<![A-Za-z0-9_])' + re.escape(key) + r'\s*=\s*("(?:[^"]*)"|[^\s{}]+)', block):
        raw = match.group(1)
        if raw.startswith('"') and raw.endswith('"'):
            out.append(raw[1:-1])
        elif re.fullmatch(r'-?\d+', raw):
            out.append(int(raw))
        else:
            out.append(raw)
    return out


def sub(block: str, key: str) -> str:
    """Body of the first `key={...}` inside a block."""
    match = re.search(r'(?<![A-Za-z0-9_])' + re.escape(key) + r'\s*=\s*\{', block)
    if not match:
        return ''
    open_index = match.end() - 1
    close_index = _match_brace(block, open_index)
    if close_index < 0:
        return ''
    return block[open_index + 1:close_index]


def name_of(block: str) -> str:
    """A CPersistentName block (`name={ key="..." }`) as text."""
    body = sub(block, 'name')
    if not body:
        return ''
    key = value(body, 'key')
    return str(key) if key not in (None, '') else ''
