"""Coverage map: which of RAMPAGE.ELF's functions the native port (src/) covers, and what the rest is.

Every function of recomp/funcmap.csv gets one status:
  ported     an address in it (its start or an instruction inside) is cited in src/: the port has read it
  read       cited only in docs/: read and written down, no port code names it
  helper     not cited, but every function that calls it (jal, followed up) is ported or read: the port's
             code took it in with its callers (math, containers, small accessors), or it is dead
  platform   Sony's SDK, the C and C++ runtimes, expat, the PS2 devices: the port has SDL, OpenGL and libc
  engine     the engine's machinery the port replaces with its own design (resource managers, the script
             compiler and junctions, the GUI framework's plumbing), while it ports what that machinery runs
  unused     engine features the game's data never reaches (effect types no file has, unused classes)
  open       game behaviour not yet read
  unsorted   none of the above (no rule in tools/coverage_groups.txt says yet)

A function's class is its vtable's (tools/rtti.py), or for a non-virtual function the class whose methods
are nearest before it (functions of one source file are together), written "~Class". The rules in
tools/coverage_groups.txt give the uncited functions their status by class or address range.

Usage: python tools/coverage.py                     print the totals and the uncited groups
       python tools/coverage.py --md docs/COVERAGE.md --html OUT.html --csv OUT.csv
       python tools/coverage.py --group NAME         the uncited functions of one group
       python tools/coverage.py --range LO HI        every function in LO..HI: status, size, name, class, the number
                                                     of callers, the strings it builds (lui + addiu/ori)
"""
import bisect
import collections
import csv
import fnmatch
import glob
import html
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import rtti  # noqa: E402

OFF, BASE, SIZE = 0x1000, 0x100000, 0x34ca80
WORDS = rtti.WORDS
STATUSES = ['ported', 'read', 'helper', 'platform', 'engine', 'unused', 'open', 'unsorted']
COLOURS = {'ported': '#2e9d5b', 'read': '#7cc68f', 'helper': '#b9dcc2', 'platform': '#8a8f99',
           'engine': '#b8a46a', 'unused': '#d3d0c8', 'open': '#d9534f', 'unsorted': '#f0ad4e'}

with open(os.path.join(ROOT, 'recomp', 'funcmap.csv')) as f:
    FUNCS = sorted((int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(f))
STARTS = [s for s, _ in FUNCS]
with open(os.path.join(ROOT, 'recomp', 'config.toml'), encoding='utf-8') as f:
    NAMED = {int(a, 16): n for n, a in re.findall(r'"([^"@]+)@0x([0-9A-F]{8})"', f.read())}


def owner(a):
    i = bisect.bisect_right(STARTS, a) - 1
    return i if i >= 0 and FUNCS[i][0] <= a < FUNCS[i][1] else None


NOT_CITED = {'ARCADE_NOTES.md',                       # its addresses are JARVOS.ELF's and M1.ELF's
             'COVERAGE.md'}                          # this tool's own output


def citations(pattern):
    cited = collections.defaultdict(set)
    for p in glob.glob(os.path.join(ROOT, pattern)):
        if os.path.basename(p) in NOT_CITED:
            continue
        with open(p, encoding='utf-8', errors='replace') as f:
            for m in re.finditer(r'0x([0-9A-Fa-f]{5,8})\b', f.read()):
                v = int(m.group(1), 16)
                if v & (v - 1) == 0:                         # a flag bit (0x100000, 0x200000), not an address
                    continue
                i = owner(v)
                if i is not None:
                    cited[i].add(os.path.basename(p))
    return cited


def classes():
    """Each virtual function's class and slot: of the vtables that hold it, the class with most methods near it."""
    holders = collections.defaultdict(list)
    near = collections.defaultdict(list)
    for name, vt, methods in rtti.vtables():
        for k, m in enumerate(methods):
            i = owner(m)
            if i is not None:
                holders[i].append((name, 8 + 4 * k))
                near[name].append(FUNCS[i][0])
    out = {}
    for i, hs in holders.items():
        s = FUNCS[i][0]
        out[i] = max(hs, key=lambda h: (sum(abs(a - s) < 0x2000 for a in near[h[0]]), -len(near[h[0]])))
    return out


def callers():
    by = collections.defaultdict(set)
    for k, x in enumerate(WORDS):
        if x >> 26 == 3:
            t = owner((x & 0x3FFFFFF) << 2)
            c = owner(BASE + 4 * k)
            if t is not None and c is not None and c != t:
                by[t].add(c)
    return by


def string_at(a):
    if not BASE <= a < BASE + SIZE:
        return None
    o = OFF + a - BASE
    t = rtti.ELF[o:rtti.ELF.find(b'\0', o, o + 120)]
    return t.decode() if len(t) >= 3 and re.fullmatch(rb'[ -~]+', t) else None


def strings(s, e):
    out, hi = [], {}
    for a in range(s, e, 4):
        x = WORDS[(a - BASE) // 4]
        op, rs, rt, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
        if op == 0xF:
            hi[rt] = imm << 16
        elif op in (9, 0xD) and rs in hi:
            t = string_at(hi[rs] + (imm - 0x10000 if imm & 0x8000 else imm) if op == 9 else hi[rs] | imm)
            if t and t not in out:
                out.append(t)
    return out


NOTES = []                                           # the rules file's "note" lines: open items outside the functions


def load_rules():
    rules = []
    with open(os.path.join(ROOT, 'tools', 'coverage_groups.txt'), encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].rstrip() if not line.lstrip().startswith('#') else ''
            if not line.strip():
                continue
            if line.startswith('note '):
                NOTES.append(line[5:].strip())
                continue
            status, sel, note = (line.split(None, 2) + [''])[:3]
            assert status in STATUSES, line
            if re.fullmatch(r'0x[0-9A-Fa-f]+-0x[0-9A-Fa-f]+', sel):
                lo, hi = (int(x, 16) for x in sel.split('-'))
                rules.append((status, ('range', lo, hi), note.strip(), sel))
            else:
                rules.append((status, ('class', sel), note.strip(), sel))
    return rules


def analyse():
    src = citations('src/*.[ch]')
    doc = citations('docs/*.md')
    cls = classes()
    rules = load_rules()
    funcs = []
    last = None
    for i, (s, e) in enumerate(FUNCS):
        if i in cls:
            last = cls[i][0]
            name = NAMED.get(s) or f'{cls[i][0]}::+{cls[i][1]:#x}'
            klass = cls[i][0]
        else:
            name = NAMED.get(s, '')
            klass = '~' + last if last else ''
        funcs.append({'i': i, 'start': s, 'end': e, 'name': name, 'class': klass,
                      'src': sorted(src.get(i, ())), 'doc': sorted(doc.get(i, ()))})
    for f in funcs:
        f['rule'] = None
        for status, sel, note, text in rules:
            if sel[0] == 'range':
                hit = sel[1] <= f['start'] < sel[2]
            else:
                hit = fnmatch.fnmatchcase(f['class'].lstrip('~'), sel[1])
            if hit:
                f['rule'] = (status, text, note)
                break
        f['status'] = 'ported' if f['src'] else 'read' if f['doc'] else None
    # helper: not cited, but all callers (followed up through other helpers) ported or read
    by = callers()
    covered = {f['i'] for f in funcs if f['status']}
    changed = True
    while changed:
        changed = False
        for f in funcs:
            if f['status'] or f['i'] in covered:
                continue
            if f['rule'] and f['rule'][0] == 'platform':
                continue
            cs = by.get(f['i'])
            if cs and cs <= covered:
                covered.add(f['i'])
                f['status'] = 'helper'
                changed = True
    for f in funcs:
        if not f['status']:
            f['status'] = f['rule'][0] if f['rule'] else 'unsorted'
        f['group'] = f['rule'][1] if f['rule'] else (f['class'].lstrip('~') or 'start')
    return funcs, rules


def totals(funcs, key=lambda f: f['status']):
    t = collections.defaultdict(lambda: [0, 0])
    for f in funcs:
        t[key(f)][0] += 1
        t[key(f)][1] += f['end'] - f['start']
    return t


def md(text):
    return text.replace('|', r'\|')


def kb(n):
    return f'{n / 1024:,.0f} KB'


def write_md(path, funcs, rules):
    t = totals(funcs)
    n_all, b_all = len(funcs), sum(f['end'] - f['start'] for f in funcs)
    out = ['# Coverage map', '',
           'Which of the game\'s functions the native port covers, generated by `python tools/coverage.py --md '
           'docs/COVERAGE.md` from `recomp/funcmap.csv` (every function of RAMPAGE.ELF), the addresses cited in '
           '`src/` and `docs/`, and the rules in `tools/coverage_groups.txt`. The statuses are explained in the '
           'tool\'s header; in short: *ported* = cited in the port\'s code, *read* = only in the notes, *helper* = '
           'only called from those, *platform* / *engine* = replaced by the port\'s own (SDL, OpenGL, its readers), '
           '*unused* = the game\'s data never reaches it, *open* = still to read.', '',
           '| Status | Functions | Code | Share of the code |', '|---|---:|---:|---:|']
    for s in STATUSES:
        if s in t:
            out.append(f'| {s} | {t[s][0]:,} | {kb(t[s][1])} | {100 * t[s][1] / b_all:.1f} % |')
    out.append(f'| all | {n_all:,} | {kb(b_all)} | |')
    out += ['', '## Open', '', 'The game behaviour no port code has read yet (the `open` rules; the uncited bytes in each):', '',
            '| Range | Uncited | What |', '|---|---:|---|']
    for status, sel, note, text in rules:
        if status == 'open':
            b = sum(f['end'] - f['start'] for f in funcs if f['group'] == text and f['status'] == 'open')
            out.append(f'| `{text}` | {b:,} B | {md(note)} |')
    if NOTES:
        out += ['', 'Also open or worth a look (from the sort, not tied to one range):', '']
        out += [f'- {n}' for n in NOTES]
    out += ['', '## Groups', '', 'One line per rule of `tools/coverage_groups.txt` (and per class without a rule): '
            'the functions it holds by status, and why the uncited ones are what they are.', '',
            '| Group | Ported | Read | Helper | Uncited | Status of the uncited | Why |', '|---|---:|---:|---:|---:|---|---|']
    groups = collections.OrderedDict()
    for f in funcs:
        groups.setdefault(f['group'], []).append(f)
    notes = {text: (status, note) for status, _, note, text in rules}
    rows = []
    for g, fs in groups.items():
        c = collections.Counter(f['status'] for f in fs)
        unc = [f for f in fs if f['status'] not in ('ported', 'read', 'helper')]
        status, note = notes.get(g, ('unsorted', ''))
        rows.append((status, -sum(f['end'] - f['start'] for f in unc), g, c, unc, note))
    order = {s: k for k, s in enumerate(['open', 'unsorted', 'unused', 'engine', 'platform'])}
    for status, nb, g, c, unc, note in sorted(rows, key=lambda r: (order.get(r[0], 9), r[1])):
        out.append(f'| `{g}` | {c["ported"]} | {c["read"]} | {c["helper"]} | {len(unc)} ({kb(-nb)}) | '
                   f'{status if unc else "-"} | {md(note)} |')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


def write_html(path, funcs):
    data = [[f['start'], f['end'] - f['start'], STATUSES.index(f['status']), f['name'], f['class'],
             ' '.join(f['src'] or f['doc'])] for f in funcs]
    t = totals(funcs)
    b_all = sum(f['end'] - f['start'] for f in funcs)
    legend = ''.join(f'<span class="k"><i style="background:{COLOURS[s]}"></i>{s} <b>{100 * t[s][1] / b_all:.1f}%</b>'
                     f' <small>{t[s][0]:,} fn</small></span>' for s in STATUSES if s in t)
    page = HTML.replace('@LEGEND@', legend).replace('@DATA@', json.dumps(data, separators=(',', ':'))) \
        .replace('@COLOURS@', json.dumps([COLOURS[s] for s in STATUSES])) \
        .replace('@NAMES@', json.dumps(STATUSES))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(page)


HTML = '''<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RampageRE coverage map</title><style>
:root{--bg:#fbfaf7;--fg:#22252a;--mut:#6b7078;--line:#e3e0d8}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#17191c;--fg:#e6e4df;--mut:#9aa0a8;--line:#30343a}}
:root[data-theme="dark"]{--bg:#17191c;--fg:#e6e4df;--mut:#9aa0a8;--line:#30343a}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:20px 16px 40px}h1{font-size:20px;margin:0 0 4px}p{color:var(--mut);margin:0 0 14px}
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;margin:0 0 14px}.k{display:inline-flex;align-items:center;gap:6px}
.k i{width:12px;height:12px;border-radius:3px;display:inline-block}.k small{color:var(--mut)}
#wrap{position:relative;border:1px solid var(--line);border-radius:6px;overflow:hidden}canvas{display:block;width:100%}
#tip{position:fixed;pointer-events:none;background:var(--fg);color:var(--bg);padding:6px 8px;border-radius:5px;font:12px/1.35 ui-monospace,monospace;display:none;max-width:420px}
.row{color:var(--mut);font:11px ui-monospace,monospace}
</style></head><body><main>
<h1>RampageRE coverage map</h1>
<p>Every function of RAMPAGE.ELF (SLES-53800), by address. One row is 16 KB of code; each function is as wide as it is long. Hover a function for its name, class and the files that cite it.</p>
<div class="legend">@LEGEND@</div>
<div id="wrap"><canvas id="c"></canvas></div><div id="tip"></div>
</main><script>
const D=@DATA@,C=@COLOURS@,N=@NAMES@,ROW=16384;
const cv=document.getElementById('c'),tip=document.getElementById('tip');
const rows=[...new Set(D.map(f=>Math.floor(f[0]/ROW)))].sort((a,b)=>a-b),ri=new Map(rows.map((r,k)=>[r,k]));
const H=7,G=1,LBL=62;
function draw(){const w=cv.parentNode.clientWidth,dpr=devicePixelRatio||1;cv.width=w*dpr;cv.height=rows.length*(H+G)*dpr;
cv.style.height=rows.length*(H+G)+'px';const x=cv.getContext('2d');x.scale(dpr,dpr);
const fg=getComputedStyle(document.body).color;x.font='9px ui-monospace,monospace';x.fillStyle=fg;
const sc=(w-LBL)/ROW;rows.forEach((r,k)=>{if(k%8==0)x.fillText('0x'+(r*ROW).toString(16),2,k*(H+G)+H)});
for(const f of D){let a=f[0],n=f[1];while(n>0){const r=Math.floor(a/ROW),o=a-r*ROW,m=Math.min(n,ROW-o);
x.fillStyle=C[f[2]];x.fillRect(LBL+o*sc,ri.get(r)*(H+G),Math.max(m*sc-0.3,0.6),H);a+=m;n-=m}}}
draw();addEventListener('resize',draw);
cv.addEventListener('mousemove',e=>{const b=cv.getBoundingClientRect(),k=Math.floor((e.clientY-b.top)/(H+G)),
sc=(b.width-LBL)/ROW,a=rows[k]*ROW+(e.clientX-b.left-LBL)/sc;let f=null;
let lo=0,hi=D.length-1;while(lo<=hi){const m=(lo+hi)>>1;if(D[m][0]<=a){f=D[m];lo=m+1}else hi=m-1}
if(!f||a>=f[0]+f[1]||e.clientX-b.left<LBL){tip.style.display='none';return}
tip.innerHTML='0x'+f[0].toString(16)+' &middot; '+f[1]+' bytes &middot; <b>'+N[f[2]]+'</b><br>'+(f[3]||'sub_'+f[0].toString(16))+
(f[4]?'<br>'+f[4]:'')+(f[5]?'<br>cited in '+f[5]:'');tip.style.display='block';
tip.style.left=Math.min(e.clientX+12,innerWidth-430)+'px';tip.style.top=(e.clientY+14)+'px'});
cv.addEventListener('mouseleave',()=>tip.style.display='none');
</script></body></html>
'''


def main():
    args = sys.argv[1:]
    funcs, rules = analyse()
    if '--range' in args:
        k = args.index('--range')
        lo, hi = int(args[k + 1], 16), int(args[k + 2], 16)
        by = callers()
        for f in funcs:
            if lo <= f['start'] < hi:
                st = ' | '.join(strings(f['start'], f['end']))[:140]
                print(f'{f["start"]:#x} {f["end"] - f["start"]:6d} {f["status"]:8s} {len(by.get(f["i"], ())):3d} '
                      f'{f["name"] or "-"} {f["class"]} {" ".join(f["src"] or f["doc"])} {st}'.rstrip())
        return
    if '--group' in args:
        g = args[args.index('--group') + 1]
        for f in funcs:
            if f['group'] == g and f['status'] not in ('ported', 'read', 'helper'):
                print(f'{f["start"]:#x} {f["end"] - f["start"]:6d} {f["status"]:9s} {f["name"]} {f["class"]}')
        return
    for opt, fn in (('--md', lambda p: write_md(p, funcs, rules)), ('--html', lambda p: write_html(p, funcs))):
        if opt in args:
            fn(args[args.index(opt) + 1])
    if '--csv' in args:
        with open(args[args.index('--csv') + 1], 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['start', 'end', 'status', 'name', 'class', 'group', 'cited'])
            for x in funcs:
                w.writerow([f'{x["start"]:#x}', f'{x["end"]:#x}', x['status'], x['name'], x['class'], x['group'],
                            ' '.join(x['src'] or x['doc'])])
    t = totals(funcs)
    b_all = sum(f['end'] - f['start'] for f in funcs)
    for s in STATUSES:
        if s in t:
            print(f'{s:9s} {t[s][0]:5d} functions {t[s][1]:8d} bytes {100 * t[s][1] / b_all:5.1f} %')
    if not any(o in args for o in ('--md', '--html', '--csv')):
        g = totals([f for f in funcs if f['status'] in ('open', 'unsorted')], key=lambda f: f['group'])
        for name, (n, b) in sorted(g.items(), key=lambda kv: -kv[1][1])[:60]:
            print(f'  {b:7d} {n:4d}  {name}')


if __name__ == '__main__':
    main()
