# -*- coding: utf-8 -*-
"""Read the Galactic Community straight out of a save.

The community object is not reachable from a verified static pointer yet, so the
save is the source of truth: `galactic_community={...}` holds the member list,
the council, the queue, the calendar; `resolution={...}` holds every resolution
with its proposer, supporters, opponents, per-country vote dates and pass date.

    python community_report.py                  # newest save, readable text
    python community_report.py --json           # machine-readable
    python community_report.py <path.sav>       # a specific save

Names: country display names are stored as `name={ key="..." literal=yes }`
(or a generated key), so they come out of the save; resolution type keys are
looked up in the game's simp_chinese localisation. Both degrade to the raw key.
"""

from __future__ import annotations

import glob
import json
import os
import re
import sys
import zipfile

import paradox_script as ps

SAVE_ROOT = os.path.join(os.environ.get('USERPROFILE', ''), 'Documents',
                         'Paradox Interactive', 'Stellaris', 'save games')
GAME_LOC = r'<Steam 库>\steamapps\common\Stellaris\localisation\simp_chinese'

_ABSENT = '-'

# ---------------------------------------------------------------- save access

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


# ------------------------------------------------------------- localisation

_LOC_LINE = re.compile(r'^\s*([A-Za-z0-9_.\-]+):\d?\s*"((?:[^"]|"")*)"')


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


_COLOR = re.compile(r'§[A-Za-z!]|\$[A-Za-z0-9_.\-]+\$')


def clean(text: str) -> str:
    return _COLOR.sub('', text).strip()


# ------------------------------------------------------------------ parsing

def country_names(text: str, loc: dict[str, str]) -> dict[int, str]:
    """country 0..N -> display name (literal names come out verbatim).

    Generated names are a small template tree (`key="%ADJ%" variables={...}`),
    so the readable form is every non-placeholder key in document order, with
    each token pushed through localisation when a key exists for it.
    """
    body = ps.section(text, 'country')
    out: dict[int, str] = {}
    for cid, block in ps.blocks(body).items():
        name_body = ps.sub(block, 'name')
        key = ps.value(name_body, 'key')
        literal = ps.value(name_body, 'literal')
        if not key:
            continue
        key = str(key)
        if literal == 'yes':
            out[cid] = key
        elif key.startswith('%'):
            out[cid] = expand_name(name_body, loc) or 'country %d' % cid
        else:
            out[cid] = clean(loc.get(key, key)) or key
    return out


_PLACEHOLDER = re.compile(r'^%.*%$')


def expand_name(name_body: str, loc: dict[str, str]) -> str:
    tokens = []
    for tok in re.findall(r'key\s*=\s*"([^"]*)"', name_body):
        if not tok or _PLACEHOLDER.match(tok) or tok.isdigit() or tok == 'adjective':
            continue
        tokens.append(clean(loc.get(tok, tok)))
    return ' '.join(t for t in tokens if t)


def id_list(body: str) -> list[int]:
    """`0 1 16777223 ` -> [0, 1, 16777223]."""
    return [int(x) for x in re.findall(r'-?\d+', body or '')]


def dated_map(body: str) -> dict[int, str]:
    """`0=2301.01.01 54=2304.10.24` -> {0: '2301.01.01', ...}."""
    return {int(k): v for k, v in re.findall(r'(\d+)=\s*"?([\d.]+)"?', body or '')}


def resolutions(text: str) -> dict[int, dict]:
    body = ps.section(text, 'resolution')
    out: dict[int, dict] = {}
    for rid, block in ps.blocks(body).items():
        out[rid] = {
            'id': rid,
            'type': ps.value(block, 'type'),
            'country': ps.value(block, 'country'),
            'supporters': id_list(ps.sub(block, 'supporters')),
            'opponents': id_list(ps.sub(block, 'opponents')),
            'votes': dated_map(ps.sub(block, 'voting')),
            'pass_date': ps.value(block, 'pass_date'),
            'freeze_date': ps.value(block, 'freeze_date'),
        }
    return out


def community(text: str) -> dict:
    block = ps.section(text, 'galactic_community')
    timers = dated_map(ps.sub(block, 'category_timers'))
    return {
        'leader': ps.value(block, 'leader'),
        'members': id_list(ps.sub(block, 'members')),
        'council': id_list(ps.sub(block, 'council')),
        'proposed': id_list(ps.sub(block, 'proposed')),
        'passed': id_list(ps.sub(block, 'passed')),
        'voting': ps.value(block, 'voting'),
        'last': ps.value(block, 'last'),
        'vote_length': ps.value(block, 'vote_length'),
        'days': ps.value(block, 'days'),
        'cooldown': ps.value(block, 'cooldown'),
        'council_positions': ps.value(block, 'council_positions'),
        'could_end_senate_session': ps.value(block, 'could_end_senate_session'),
        'formed': ps.value(block, 'community_formed'),
        'council_established': ps.value(block, 'council_established'),
        'category_timers': timers,
    }


# ------------------------------------------------------------------- report

def build(path: str, with_loc: bool = True) -> dict:
    text = gamestate_text(path)
    loc = localisation() if with_loc else {}
    names = country_names(text, loc)
    com = community(text)
    res = resolutions(text)

    dupes = {n for n in set(names.values()) if list(names.values()).count(n) > 1}

    def who(cid):
        if cid is None:
            return _ABSENT
        name = names.get(cid, 'country %s' % cid)
        # generated names repeat across empires; the id keeps a list readable
        return '%s(#%s)' % (name, cid) if name in dupes else name

    def detail(rid: int) -> dict:
        r = res.get(rid, {'id': rid, 'type': None, 'country': None,
                          'supporters': [], 'opponents': [], 'votes': {},
                          'pass_date': None, 'freeze_date': None})
        tkey = r['type'] or ''
        return {
            'id': rid,
            'type': tkey,
            'name': clean(loc.get(tkey, tkey)) if tkey else None,
            'proposer': who(r['country']),
            'proposer_id': r['country'],
            'opposed': [who(c) for c in r['opponents']],
            'opposed_ids': r['opponents'],
            'votes': r['votes'],
            'pass_date': r['pass_date'],
        }

    acting = com['voting']
    acting_id = acting if isinstance(acting, int) and acting >= 0 else None
    active = detail(acting_id) if acting_id is not None else None
    if active is not None:
        r = res.get(acting_id, {})
        members = com['members']
        sup = [c for c in r.get('supporters', [])]
        opp = r.get('opponents', [])
        rest = [c for c in members if c not in sup and c not in opp]
        active['supported_ids'] = sup
        active['supported'] = [who(c) for c in sup]
        active['abstain_ids'] = rest
        active['abstain'] = [who(c) for c in rest]
        active['voted'] = [{'country': who(c), 'country_id': c, 'date': d}
                           for c, d in sorted(r.get('votes', {}).items())]

    queue = []
    for rid in com['proposed']:
        d = detail(rid)
        r = res.get(rid, {})
        d['supported_count'] = len(r.get('supporters', []))
        d['opposed_count'] = len(r.get('opponents', []))
        queue.append(d)

    passed = [detail(rid) for rid in com['passed']]

    return {
        'save': path,
        'community': {
            'formed': com['formed'],
            'council_established': com['council_established'],
            'leader': who(com['leader']),
            'leader_id': com['leader'],
            'members': [who(c) for c in com['members']],
            'member_ids': com['members'],
            'council': [who(c) for c in com['council']],
            'council_ids': com['council'],
            'council_positions': com['council_positions'],
            'vote_length': com['vote_length'],
            'days': com['days'],
            'cooldown': com['cooldown'],
            'days_left': (com['vote_length'] - com['days'])
            if isinstance(com['vote_length'], int) and isinstance(com['days'], int) else None,
            'can_end_session': com['could_end_senate_session'],
            'category_timers': {k: v for k, v in sorted(com['category_timers'].items())},
        },
        'voting': active,
        'queue': queue,
        'passed': passed,
    }


def text_report(d: dict) -> str:
    c = d['community']
    out = []
    out.append('共同体　成立于 %s　议会 %s　席位 %s' %
               (c['formed'], c['council_established'], c['council_positions']))
    out.append('领袖　%s' % c['leader'])
    out.append('成员 %d：%s' % (len(c['members']), ' / '.join(c['members'])))
    out.append('议会：%s' % ' / '.join(c['council']))
    if c['days_left'] is not None:
        out.append('本届 %s／%s 天，剩余 %s 天' % (c['days'], c['vote_length'], c['days_left']))
    out.append('')

    v = d['voting']
    if v is None:
        out.append('正在投票：无（休会）')
    else:
        out.append('正在投票　#%d　%s' % (v['id'], v['name'] or v['type']))
        out.append('  提案国 %s' % v['proposer'])
        out.append('  支持 %d：%s' % (len(v['supported']), ' / '.join(v['supported']) or '—'))
        out.append('  反对 %d：%s' % (len(v['opposed']), ' / '.join(v['opposed']) or '—'))
        out.append('  弃权 %d：%s' % (len(v['abstain']), ' / '.join(v['abstain']) or '—'))
        if v['voted']:
            out.append('  已投 %d 人：%s' % (len(v['voted']),
                                            ' / '.join('%s(%s)' % (x['country'], x['date'])
                                                       for x in v['voted'])))
    out.append('')

    out.append('排队 %d 条（先上先议）' % len(d['queue']))
    for i, q in enumerate(d['queue'], 1):
        out.append('  %2d. #%-3d %s　提案 %s　支持 %d／反对 %d' %
                   (i, q['id'], q['name'] or q['type'], q['proposer'],
                    q['supported_count'], q['opposed_count']))
    out.append('')

    out.append('已通过 %d 条' % len(d['passed']))
    for p in sorted(d['passed'], key=lambda x: str(x['pass_date'] or ''), reverse=True):
        out.append('  #%-3d %s　%s 通过' % (p['id'], p['name'] or p['type'], p['pass_date'] or '?'))
    out.append('')

    if c['category_timers']:
        out.append('类别冷却（可再提之日）')
        for k, v2 in c['category_timers'].items():
            out.append('  %s  %s' % (k, v2))
    return '\n'.join(out)


def main(argv: list[str]) -> int:
    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass
    raw = '--json' in argv
    args = [a for a in argv if not a.startswith('--')]
    path = args[0] if args else newest_save()
    data = build(path, with_loc='--no-loc' not in argv)
    if raw:
        print(json.dumps(data, ensure_ascii=False, indent=1))
    else:
        print(text_report(data))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
