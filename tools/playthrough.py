"""The automated playthrough suite: RampageRE-dev.exe --autoplay over the whole game, the problems it finds.

Each run is one RampageRE-dev.exe --autoplay process (offscreen, simulated time; docs/NATIVE.md "Autoplay"): the campaign from
the front end to the credits, each city on its own, the other play modes, two and four players, the attract mode's
demos, with several seeds; and every monster on its own, its HP and its blows checked against its MONSTER_DATA. The
runs go in parallel; everything is written to the folder given (OUT): per run its log
(<name>.log, the watchdog's BLOCK / PROBLEM / EVENT / SUMMARY lines), its console (<name>.out), its screenshots
(<name>_shots/) and its profile; then report.md and report.json, a table per run and every problem.

Usage:
  python tools/playthrough.py OUT [--vol RAMPAGE.VOL] [--exe RampageRE-dev.exe] [--suite full|cities|modes|ci] [--jobs N]
                                  [--seeds 1,2] [--only NAME,...] [--timeout SECONDS]
  python tools/playthrough.py OUT --suite ci      one city, two blocks: exit 1 on a crash, a hang or a problem
  python tools/playthrough.py OUT --suite monsters   each of the 30 monsters, four of it in Chicago for three blocks
                                                  (RAMPAGE_TRACE on): its most HP (+0x138) and the damage of every blow
                                                  between them must be the disc's

The exit code: 0 no crash, hang or problem; 1 otherwise.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

import bdp
import monster
import pvol

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAMPAIGN = ['LasVegas', 'SanFrancisco', 'London', 'LosAngeles', 'Chicago', 'HongKong', 'NewYork']
STRONG = '986960,071767'                             # no monster can be hit, one-hit buildings

# console lines worth a look (numbers folded when they are counted); "N not found" of a block's line only when N > 0
NOISE = [re.compile(p) for p in (r'not found', r'(?i)\bassert', r'(?i)\berror', r'(?i)cannot ', r'(?i)\bfail',
                                 r'^no [A-Za-z]', r'(?i)nan\b', r'(?i)\binf\b', r'(?i)overflow', r'(?i)too many')]
# a first guess at what a problem is, for the triage (the report says it is a guess)
KIND_HINT = {
    'crash': 'port bug', 'nan_position': 'port bug', 'never_respawns': 'port bug', 'state_never_ends': 'port bug',
    'results_never_answer': 'port bug', 'shell_never_advances': 'port bug (or the autopilot)',
    'objects_runaway': 'port bug', 'memory': 'port bug', 'memory_growth': 'port bug', 'inside_building': 'port bug?',
    'under_ground': 'port bug?', 'outside_street': 'port bug?', 'monster_stuck': 'bot limit or port bug',
    'block_never_ends': 'bot limit or port bug', 'block_lost_repeatedly': 'bot limit (it does not eat or dodge)',
}
MONSTER_RUN = 'monster_'                             # the monsters suite's runs: traced, then checked against the disc
HIT = re.compile(r'^  hit by (.+?): ([\d.]+), HP (\d+)')   # main.c monster_take's trace (HP printed whole)
QUIET = [re.compile(p) for p in (r'\b0 not found', r'^\[\d\] |^[A-Z][A-Za-z_0-9]* -> ', r'^wrote ', r'^camera at ')]


def disc_monsters(vol):
    """each monster of the disc (an <M>_anims.cmp beside its <M>.cmp): {name: MonsterData}"""
    out = {}
    with open(vol, 'rb') as f:
        files = {name: (offset, size) for name, offset, size in pvol.read_index(f)}
        for name in sorted(files):
            if not name.endswith('_anims.cmp') or name[:-10] + '.cmp' not in files:
                continue
            offset, size = files[name[:-10] + '.cmp']
            f.seek(offset)
            b = bdp.Bundle(f.read(size))
            for e, a, _ in b.items(0):
                if b.name(e.name).lstrip('\x13').endswith('MONSTER_DATA'):
                    out[name[:-10]] = monster.MonsterData(a, b.names)
    return out


def monster_check(console, blocks, md):
    """a monsters-suite run against its MONSTER_DATA: the HP before every hit (HP after + the amount) at most +0x138 and
    once equal to it (the start), the HP at each block's end at most +0x138, and every blow between the monsters one of
    its attack records' damage. The trace prints the HP whole, so 0.5 either way."""
    hp_max, damages = md.hp_max(), {d for _, d in md.attacks()}
    hits, other, top, fail = {}, 0, 0.0, []
    with open(console, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = HIT.match(line)
            if not m:
                continue
            who, amount, hp = m.group(1), float(m.group(2)), int(m.group(3))
            top = max(top, hp + amount)
            if hp + amount > hp_max + 0.5:
                fail.append(f'{line.strip()}: more than the most HP {hp_max:g}')
            if not who.startswith('monster '):
                other += 1
                continue
            hits[amount] = hits.get(amount, 0) + 1
            if all(abs(amount - d) > 1e-3 for d in damages):
                fail.append(f'{line.strip()}: no attack record of that damage')
    if not hits:
        fail.append('no blow between the monsters: nothing checked')
    elif abs(top - hp_max) > 0.5:
        fail.append(f'the HP before a hit was at most {top:g}, the disc says {hp_max:g}')
    for b in blocks:
        hp = [float(x) for x in b.get('hp', '').split(',') if x]
        if any(x > hp_max + 0.5 for x in hp):
            fail.append(f"block {b.get('block')}: hp {b.get('hp')} past {hp_max:g}")
    if len(blocks) < 3:
        fail.append(f'{len(blocks)} of 3 blocks ended')
    return {'hp_max': hp_max, 'top': top, 'hits': {f'{k:g}': n for k, n in sorted(hits.items())}, 'other_hits': other,
            'damages': sorted(damages), 'fail': fail}


def runs(suite, seeds, monsters=()):
    """the runs of a suite: (name, arguments, minutes)"""
    r = []
    if suite in ('full', 'campaign'):
        for s in seeds:
            r.append((f'campaign_seed{s}', ['--shell', '--ap-seed', str(s)], 900))
        r.append(('campaign_strong', ['--shell', '--cheat', STRONG, '--ap-seed', str(seeds[0])], 900))
    if suite in ('full', 'cities'):
        for c in CAMPAIGN:
            for s in seeds:
                r.append((f'city_{c}_seed{s}', [c, '1', '--ap-cities', '1', '--ap-seed', str(s)], 300))
    if suite in ('full', 'modes'):
        s = str(seeds[0])
        r += [
            ('kingcity_shell_2p', ['--shell', '--play', 'kingcity', '--players', '2', '--bots', '1', '--ap-city',
                                   'Chicago', '--ap-cities', '1', '--ap-seed', s], 300),
            ('kingcity_4p', ['London', '1', '--play', 'kingcity', '--players', '4', '--bots', '2', '--ap-cities', '1',
                             '--ap-seed', s], 300),
            ('kingworld_2p', ['LasVegas', '1', '--play', 'kingworld', '--players', '2', '--bots', '1', '--ap-seed', s],
             900),
            ('timed_shell', ['--shell', '--play', 'timed', '--ap-city', 'LosAngeles', '--ap-cities', '1', '--ap-seed',
                             s], 300),
            ('timed_HongKong', ['HongKong', '1', '--play', 'timed', '--ap-cities', '1', '--ap-seed', s], 300),
            ('campaign_2p_London', ['London', '1', '--players', '2', '--ap-cities', '1', '--ap-seed', s], 300),
            ('campaign_4p_Chicago', ['Chicago', '1', '--players', '4', '--bots', '2', '--ap-cities', '1', '--ap-seed',
                                     s], 300),
            ('demos', ['--shell', '--ap-demos', '6'], 120),
        ]
    if suite == 'monsters':
        for m in monsters:
            r.append((MONSTER_RUN + m, ['Chicago', '1', '--players', '4', '--monster', m, '--monster2', m, '--monster3',
                                        m, '--monster4', m, '--ap-blocks', '3', '--ap-seed', str(seeds[0])], 12))
    if suite == 'ci':
        r.append(('ci_LasVegas', ['LasVegas', '1', '--ap-blocks', '2', '--ap-seed', str(seeds[0])], 30))
    return r


def parse_log(path):
    out = {'blocks': [], 'problems': [], 'events': [], 'summary': None, 'crash': None, 'lines': []}
    if not os.path.exists(path):
        return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            out['lines'].append(line)
            kind = line.split(' ', 1)[0]
            kv = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
            if kind == 'BLOCK':
                out['blocks'].append(kv)
            elif kind == 'PROBLEM':
                out['problems'].append(kv | {'line': line})
            elif kind == 'EVENT':
                out['events'].append(kv)
            elif kind == 'SUMMARY':
                out['summary'] = kv
            elif kind == 'CRASH':
                out['crash'] = line
            elif kind == 'CRASH_FRAME':
                out.setdefault('frames', []).append(line)
    return out


def scan_console(path):
    """the console's lines worth a look, folded: {line: count}"""
    seen = {}
    if not os.path.exists(path):
        return seen
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or any(q.search(line) for q in QUIET) or not any(n.search(line) for n in NOISE):
                continue
            key = re.sub(r'-?\d+(\.\d+)?', '#', line)[:160]
            seen[key] = seen.get(key, 0) + 1
    return seen


def tail(path, n=15):
    if not os.path.exists(path):
        return []
    with open(path, encoding='utf-8', errors='replace') as f:
        return [l.rstrip('\n') for l in f.readlines()[-n:]]


def run_one(exe, vol, outdir, name, args, minutes, timeout):
    shots = os.path.join(outdir, name + '_shots')
    os.makedirs(shots, exist_ok=True)
    log = os.path.join(outdir, name + '.log')
    con = os.path.join(outdir, name + '.out')
    prof = os.path.join(outdir, name + '.prof')
    if os.path.exists(prof):
        os.remove(prof)
    cmd = [exe, vol] + args + ['--autoplay', '--ap-minutes', str(minutes), '--ap-log', log, '--ap-shots', shots,
                              '--profile', prof, '--shot', os.path.join(outdir, name + '_end.png'), '--mute']
    env = os.environ | {'RAMPAGE_TRACE': '1'} if name.startswith(MONSTER_RUN) else None
    t0 = time.time()
    with open(con, 'w', encoding='utf-8', errors='replace') as f:
        try:
            p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, cwd=outdir, env=env)
            code, hung = p.returncode, False
        except subprocess.TimeoutExpired:
            code, hung = None, True
    res = {'name': name, 'cmd': cmd, 'exit': code, 'wall_s': round(time.time() - t0, 1), 'hung': hung}
    res |= parse_log(log)
    res['console'] = scan_console(con)
    if hung or code not in (0, 3):
        res['tail'] = tail(con)
    return res


def table(results):
    rows = ['| run | end | blocks won / ended | sim min | wall s | peak MB | problems | exit |', '|---|---|---|---|---|---|---|---|']
    for r in results:
        s = r['summary'] or {}
        end = 'HANG (wall clock)' if r['hung'] else ('CRASH' if r['exit'] not in (0, 3) else s.get('end', '-'))
        won = sum(1 for b in r['blocks'] if b.get('result', '').startswith('won'))
        last_t = [m.group(1) for l in r.get('lines', []) for m in [re.search(r' t=([\d.]+)', l)] if m]
        sim = float(s.get('t', 0) or (last_t[-1] if last_t else 0)) / 60
        rows.append(f"| {r['name']} | {end} | {won} / {len(r['blocks'])} | {sim:.0f} | {r['wall_s']:.0f} | "
                    f"{s.get('peak_mb', '-')} | {len(r['problems'])} | {r['exit']} |")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', help='the folder everything is written to')
    ap.add_argument('--vol', default=os.environ.get('RAMPAGE_VOL', os.path.join(ROOT, 'build', 'disc', 'RAMPAGE.VOL')))
    ap.add_argument('--exe', default=os.path.join(ROOT, 'RampageRE-dev.exe' if os.name == 'nt' else 'rampage'))
    ap.add_argument('--suite', default='full', choices=['full', 'campaign', 'cities', 'modes', 'ci',
                                                                'monsters'])
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--seeds', default='1,2')
    ap.add_argument('--only', default='', help='the runs to do, by name (comma separated prefixes)')
    ap.add_argument('--timeout', type=float, default=3600, help='wall-clock seconds a run may take')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    exe, vol = os.path.abspath(a.exe), os.path.abspath(a.vol)
    seeds = [int(s) for s in a.seeds.split(',') if s]
    mons = disc_monsters(vol) if a.suite == 'monsters' else {}
    todo = runs(a.suite, seeds, mons)
    if a.only:
        pre = a.only.split(',')
        todo = [t for t in todo if any(t[0].startswith(p) for p in pre)]
    print(f'{len(todo)} runs, {a.jobs} at a time, into {out}')
    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        futs = {pool.submit(run_one, exe, vol, out, n, args, m, a.timeout): n for n, args, m in todo}
        for f in concurrent.futures.as_completed(futs):
            r = f.result()
            if r['name'].startswith(MONSTER_RUN):
                r['monster'] = monster_check(os.path.join(out, r['name'] + '.out'), r['blocks'],
                                             mons[r['name'][len(MONSTER_RUN):]])
            results.append(r)
            s = r['summary'] or {}
            print(f"{r['name']}: exit {r['exit']}{' HUNG' if r['hung'] else ''}, {len(r['blocks'])} blocks, "
                  f"{len(r['problems'])} problems, end {s.get('end', '-')}, {r['wall_s']:.0f} s", flush=True)
    order = {n: i for i, (n, _, _) in enumerate(todo)}
    results.sort(key=lambda r: order[r['name']])
    rows = table(results)
    md = ['# Playthrough', '', *rows, '']
    bad = False
    for r in results:
        md.append(f"## {r['name']}")
        md.append('`' + ' '.join(r['cmd'][1:]) + '`')
        md.append('')
        if r['hung']:
            md.append('**hang (wall clock)**: the last log lines:')
            md += ['    ' + l for l in r['lines'][-5:]]
            bad = True
        if r['exit'] not in (0, 3) and not r['hung']:
            md.append(f"**crash**, exit {r['exit']}: {r['crash'] or ''}")
            md += ['    ' + l for l in r.get('frames', [])]
            bad = True
        if r.get('tail'):
            md.append('the console\'s last lines:')
            md += ['    ' + l for l in r['tail']]
        for b in r['blocks']:
            md.append(f"- {b.get('city')} {b.get('block')} ({b.get('type')}): {b.get('result')} in {b.get('dur')} s, "
                      f"score {b.get('score')}, lives {b.get('lives')}, hp {b.get('hp')}, boss {b.get('boss')}")
        if r.get('monster'):
            c = r['monster']
            md.append(f"- monster: most HP {c['hp_max']:g}, before a hit at most {c['top']:g}; blows between them "
                      f"{', '.join(f'{k} x{n}' for k, n in c['hits'].items()) or 'none'} (the disc's damages "
                      f"{', '.join(f'{d:g}' for d in c['damages'])}); {c['other_hits']} other hits")
            for x in c['fail']:
                md.append(f'- MONSTER CHECK {x}')
                bad = True
        for p in r['problems']:
            md.append(f"- PROBLEM ({KIND_HINT.get(p.get('kind'), '?')}, a guess) {p['line']}")
            bad = True
        for k, n in sorted(r['console'].items(), key=lambda x: -x[1])[:25]:
            md.append(f'- console x{n}: `{k}`')
        md.append('')
    with open(os.path.join(out, 'report.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    for r in results:
        r.pop('lines', None)
    with open(os.path.join(out, 'report.json'), 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=1)
    print('\n'.join(table(results)))
    print(f"report: {os.path.join(out, 'report.md')}")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
