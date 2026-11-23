# -*- coding: utf-8 -*-
"""Read overlord/subject relations straight out of a Stellaris save — fast.

Two independent sources, cross-checked:

  country.{raw_id}.overlord      -> tagged ref (0x01000000 | id); absent = no overlord
  country.{raw_id}.subject_date  -> the day it became a subject (string date)
  country.{raw_id}.loyalty       -> loyalty as stored (integer, e.g. -100)
  agreements.agreements.{id}     -> owner / target / agreement_preset / date_added
                                    + discrete terms (protectorate, specialist, expand...)

The `overlord` field says who; the agreement says what kind — `preset_protectorate`
is what the game calls 保护国, `preset_vassal_*` / `preset_bulwark_*` /
`preset_scholarium_*` are the vassal presets. Both are reported so a mismatch shows.

Speed: the country section is split by the `\\t<id>=` block markers (one regex pass),
then each block is read with anchored two-tab regexes. No char-by-char brace
matching, so a 47 MB gamestate takes ~1 s instead of ~7 s.

    python subject_report.py                  # newest save
    python subject_report.py <path.sav>       # one save
    python subject_report.py --json           # machine-readable
    python subject_report.py --no-loc         # skip localisation
"""

from __future__ import annotations

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

SAVE_ROOT = os.path.join(os.environ.get('USERPROFILE', ''), 'Documents',
                         'Paradox Interactive', 'Stellaris', 'save games')
GAME_LOC = r'<Steam 库>\steamapps\common\Stellaris\localisation\simp_chinese'
NONE = 0xFFFFFFFF

_BLOCK = re.compile(r'(?m)^\t(\d+)=\s*$')
_LOC_LINE = re.compile(r'^\s*([A-Za-z0-9_.\-]+):\d?\s*"((?:[^"]|"")*)"')
_PLACEHOLDER = re.compile(r'^%.*%$')
_COLOR = re.compile(r'§[A-Za-z!]|\$[A-Za-z0-9_.\-]+\$')


# ------------------------------------------------------------------ save access

def newest_save() -> str:
    files = glob.glob(os.path.join(SAVE_ROOT, '**', '*.sav'), recursive=True)
    if not files:
        raise SystemExit('no save found under ' + SAVE_ROOT)
    return max(files, key=os.path.getmtime)


def gamestate_text(path: str) -> str:
    if path.endswith('.sav') or zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as zf:
            return zf.read('gamestate').decode('utf-8', 'replace')
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        return fh.read()


def localisation() -> dict[str, str]:
    out: dict[str, str] = {}
    if not os.path.isdir(GAME_LOC):
        return out
    for name in os.listdir(GAME_LOC):
        if not name.endswith('.yml'):
            continue
        try:
            with open(os.path.join(GAME_LOC, name), 'r', encoding='utf-8',
                      errors='replace') as fh:
                for line in fh:
                    m = _LOC_LINE.match(line)
                    if m:
                        out[m.group(1)] = m.group(2).replace('""', '"')
        except OSError:
            continue
    return out


def clean(text: str) -> str:
    return _COLOR.sub('', text).strip()


# ---------------------------------------------------------------------- ids

def ref_id(value) -> int | None:
    """Tagged country ref -> bare id; None for an empty / -1 field.

    Save block keys and refs both use the raw form 0x01000000 | id, so every id
    leaving this module is masked down to the bare number.
    """
    if value is None:
        return None
    if isinstance(value, str):
        if not re.fullmatch(r'-?\d+', value):
            return None
        value = int(value)
    value &= 0xFFFFFFFF
    if value == NONE:
        return None
    return value & 0xFFFFFF


# ------------------------------------------------------------------ parsing

def blocks_fast(body: str, tabs: int = 1) -> dict[int, str]:
    """Split a `\\t<id>={...}` body by its top-level block markers."""
    pat = re.compile(r'(?m)^' + '\t' * tabs + r'(\d+)=\s*$')
    marks = list(pat.finditer(body))
    out: dict[int, str] = {}
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
        out[int(m.group(1))] = body[m.end():end]
    return out


def top_field(block: str, key: str):
    """Top-level field of a country block (keys sit at exactly two tabs)."""
    m = re.search(r'(?m)^\t\t' + re.escape(key) + r'=([^\n]*)', block)
    if not m:
        return None
    raw = m.group(1).strip()
    if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
        return raw[1:-1]
    if re.fullmatch(r'-?\d+', raw):
        return int(raw)
    return raw or None


def country_name(block: str, loc: dict[str, str]) -> str:
    """Display name of a country block (literal, localised key, or template)."""
    m = re.search(r'(?m)^\t\tname=\s*$', block)
    if not m:
        return ''
    seg = block[m.end():]
    # keep only the name={...} block: cut at the next top-level key
    nxt = re.search(r'(?m)^\t\t[a-z_0-9]+=', seg)
    seg = seg[:nxt.start()] if nxt else seg[:4000]
    mk = re.search(r'key="([^"]*)"', seg)
    if not mk:
        return ''
    key = mk.group(1)
    if re.search(r'(?m)^\t\t\tliteral=yes', seg):
        return key
    if key.startswith('%'):
        toks = []
        for t in re.findall(r'key="([^"]*)"', seg):
            if not t or _PLACEHOLDER.match(t) or t.isdigit() or t == 'adjective':
                continue
            if t.startswith('SPEC_'):
                t = t[5:]
            toks.append(clean(loc.get(t, t)))
        return ' '.join(t for t in toks if t) or ''
    return clean(loc.get(key, key)) or key


def subject_terms(block: str) -> dict:
    """Discrete terms of a subject agreement (protectorate / specialist / ...)."""
    return dict(re.findall(r'key=(\w+)\s+value="?([^"\s}]+)', block))


def agreements(text: str) -> dict[int, dict]:
    body = ps.section(text, 'agreements')
    inner = ps.sub(body, 'agreements') or body
    out = {}
    for aid, block in blocks_fast(inner, tabs=2).items():
        out[aid] = {
            'id': aid,
            'owner': ref_id(ps.value(block, 'owner')),
            'target': ref_id(ps.value(block, 'target')),
            'preset': ps.value(block, 'agreement_preset'),
            'date_added': ps.value(block, 'date_added'),
            'active': ps.value(block, 'active_status'),
            'terms': subject_terms(block),
            'forced_initial_loyalty': ps.value(block, 'forced_initial_loyalty'),
        }
    return out


def game_date(text: str) -> str:
    m = re.search(r'(?m)^date="([^"]+)"', text)
    return m.group(1) if m else '?'


def player_country(text: str) -> int | None:
    v = ps.value(ps.section(text, 'player'), 'country')
    return ref_id(v) if v is not None else None


# ------------------------------------------------------------------- report

def section_fast(text: str, name: str) -> str:
    """Body of a top-level `name={...}` without char-by-char brace matching.

    Top-level sections start with a line `name=` and a `{` at column 0, and their
    content is indented by tabs, so the first line that is exactly `}` after the
    opening brace closes them. That is a plain str.find, ~10x faster than
    ps.section() on a 47 MB gamestate.
    """
    marker = '\n' + name + '=\n{'
    i = text.find(marker)
    if i < 0:
        return ps.section(text, name)
    start = i + len(marker)
    end = text.find('\n}\n', start)
    return text[start:end] if end > 0 else text[start:]


def relations(text: str, loc: dict[str, str]) -> dict:
    body = section_fast(text, 'country')
    agrs = agreements(text)
    subjects_of: dict[int, list[int]] = {}
    overlord_of: dict[int, int] = {}
    meta: dict[int, dict] = {}
    names: dict[int, str] = {}

    for raw_cid, block in blocks_fast(body).items():
        cid = ref_id(raw_cid)
        if cid is None:
            continue
        nm = country_name(block, loc)
        if nm.startswith('SPEC_'):
            nm = nm[5:]          # generated name stored as an English literal
        names[cid] = nm or 'country %d' % cid
        ov = ref_id(top_field(block, 'overlord'))
        meta[cid] = {
            'id': cid,
            'name': names[cid],
            'overlord': ov,
            'subject_date': top_field(block, 'subject_date'),
            'loyalty': top_field(block, 'loyalty'),
            'is_nomadic': top_field(block, 'is_nomadic') == 'yes',
            'country_type': top_field(block, 'type'),
        }
        if ov is not None:
            overlord_of[cid] = ov
            subjects_of.setdefault(ov, []).append(cid)

    by_pair = {(a['owner'], a['target']): a for a in agrs.values()}
    groups = []
    for owner, subs in sorted(subjects_of.items()):
        rows = []
        for sid in sorted(subs):
            a = by_pair.get((owner, sid), {})
            rows.append({
                'id': sid,
                'name': meta.get(sid, {}).get('name', 'country %d' % sid),
                'preset': a.get('preset'),
                'date_added': a.get('date_added') or meta.get(sid, {}).get('subject_date'),
                'loyalty': meta.get(sid, {}).get('loyalty'),
                'active': a.get('active'),
                'terms': a.get('terms', {}),
            })
        groups.append({
            'id': owner,
            'name': meta.get(owner, {}).get('name', 'country %d' % owner),
            'subjects': rows,
        })

    conflicts = [a for a in agrs.values()
                 if a['owner'] is not None and a['target'] is not None
                 and overlord_of.get(a['target']) != a['owner']]

    return {
        'groups': groups,
        'agreements': list(agrs.values()),
        'conflicts': conflicts,
        'meta': meta,
        'subject_count': len(overlord_of),
        'overlord_count': len(subjects_of),
    }


def main() -> int:
    args = sys.argv[1:]
    as_json = '--json' in args
    with_loc = '--no-loc' not in args
    paths = [a for a in args if not a.startswith('--')]
    path = paths[0] if paths else newest_save()

    text = gamestate_text(path)
    loc = localisation() if with_loc else {}
    data = relations(text, loc)
    data['save'] = os.path.basename(path)
    data['date'] = game_date(text)
    player = player_country(text)
    data['player'] = player

    if as_json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
        return 0

    pname = data['meta'].get(player, {}).get('name', 'country %s' % player)
    print('%s · %s · 玩家 #%s %s' % (data['save'], data['date'], player, pname))
    print('宗主-附庸 %d 组 · 附庸 %d 国（agreements %d 条）'
          % (data['overlord_count'], data['subject_count'], len(data['agreements'])))
    for g in data['groups']:
        mine = '（我们）' if g['id'] == player else ''
        print('\n#%d %s%s → %d 个附庸' % (g['id'], g['name'], mine, len(g['subjects'])))
        for s in g['subjects']:
            terms = s['terms'] or {}
            flags = []
            if terms.get('protectorate') == 'subject_is_protectorate':
                flags.append('保护国')
            if terms.get('can_subject_be_integrated') == 'no':
                flags.append('不可整合')
            if terms.get('subject_expand'):
                flags.append(terms['subject_expand'])
            print('    #%-5d %-26s ｜ %-28s ｜ 忠诚 %-5s ｜ 自 %s%s'
                  % (s['id'], s['name'], s['preset'] or '-', s['loyalty'],
                     s['date_added'] or '-',
                     (' ｜ ' + ' / '.join(flags)) if flags else ''))
    if data['conflicts']:
        print('\n⚠ agreements 与 overlord 字段不一致 %d 条：' % len(data['conflicts']))
        for a in data['conflicts']:
            print('    agreement %s: owner %s → target %s，但 target 的 overlord 是 %s'
                  % (a['id'], a['owner'], a['target'],
                     data['meta'].get(a['target'], {}).get('overlord')))
    else:
        print('\n一致性：agreements 与 overlord 字段逐条对上')
    return 0


if __name__ == '__main__':
    sys.exit(main())
