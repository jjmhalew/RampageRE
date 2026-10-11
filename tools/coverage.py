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
                                                     (--fragment: the page without <html>/<head>/<body>)
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


WHAT = {'ported': 'cited in the port\'s code', 'read': 'cited only in the notes',
        'helper': 'only called from ported or read code', 'platform': 'PS2 SDK, C runtimes, devices: SDL, OpenGL, libc',
        'engine': 'engine machinery the port replaces with its own', 'unused': 'never reached by the game\'s data',
        'open': 'game behaviour still to read', 'unsorted': 'no rule yet'}


def write_html(path, funcs, rules, fragment=False):
    """The clickable map. fragment: no <!doctype>/<html>/<head>/<body> (for a host that adds its own)."""
    data = [[f['start'], f['end'] - f['start'], STATUSES.index(f['status']), f['name'], f['class'],
             ' '.join(f['src'] or f['doc'])] for f in funcs]
    t = totals(funcs)
    b_all = sum(f['end'] - f['start'] for f in funcs)
    present = [s for s in STATUSES if s in t]
    bar = ''.join(f'<i class="s-{s}" style="flex:{t[s][1]}" title="{s} {100 * t[s][1] / b_all:.1f} %"></i>'
                  for s in present)
    legend = ''.join(
        f'<li><i class="sw s-{s}"></i><span class="nm">{s}</span><span class="pc">{100 * t[s][1] / b_all:.1f} %</span>'
        f'<span class="ds">{WHAT[s]} &middot; {t[s][0]:,} functions, {t[s][1] / 1024:,.0f} KB</span></li>'
        for s in present)
    rows = []
    for status, sel, note, text in rules:
        if status == 'open':
            n = sum(f['end'] - f['start'] for f in funcs if f['group'] == text and f['status'] == 'open')
            rows.append(f'<tr><td class="mono">{html.escape(text)}</td><td class="num">{n:,}</td>'
                        f'<td>{html.escape(note)}</td></tr>')
    extra = ''.join(f'<li>{html.escape(n)}</li>' for n in NOTES)
    page = HTML.replace('@BAR@', bar).replace('@LEGEND@', legend).replace('@OPEN@', ''.join(rows)) \
        .replace('@NOTES@', extra).replace('@ALLKB@', f'{b_all / 1024:,.0f}').replace('@NFUNC@', f'{len(funcs):,}') \
        .replace('@NAMES@', json.dumps(STATUSES)).replace('@DATA@', json.dumps(data, separators=(',', ':')))
    if not fragment:
        page = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
                '<meta name="viewport" content="width=device-width,initial-scale=1">\n' + page + '\n</body></html>\n')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(page)


HTML = """<title>RampageRE coverage map</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@500;700;800&family=JetBrains+Mono:wght@400;600&display=swap">
<style>
/* layout: a header with the share bar, then the address map (one row = 16 KB), then the open list */
:root{--bg:#f6f7f5;--panel:#ffffff;--fg:#1d2321;--mut:#5d6763;--line:#dde2de;--hl:#1d2321;
--s-ported:#1f8a4c;--s-read:#62b47e;--s-helper:#a9d6b6;--s-platform:#7d8794;--s-engine:#b39a55;--s-unused:#d2d3cc;--s-open:#d2453c;--s-unsorted:#ee9a2f;
--display:"Archivo","Helvetica Neue",Arial,sans-serif;--body:system-ui,-apple-system,"Segoe UI",sans-serif;--mono:"JetBrains Mono",ui-monospace,Consolas,monospace}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#141816;--panel:#1b201e;--fg:#e4e8e5;--mut:#98a39e;--line:#2c3431;--hl:#ffffff;
--s-ported:#38b56a;--s-read:#4f9466;--s-helper:#2f5a3e;--s-platform:#5e6772;--s-engine:#8f7a3f;--s-unused:#3a3f3c;--s-open:#f0625a;--s-unsorted:#f2a640;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#141816;--panel:#1b201e;--fg:#e4e8e5;--mut:#98a39e;--line:#2c3431;--hl:#ffffff;
--s-ported:#38b56a;--s-read:#4f9466;--s-helper:#2f5a3e;--s-platform:#5e6772;--s-engine:#8f7a3f;--s-unused:#3a3f3c;--s-open:#f0625a;--s-unsorted:#f2a640;color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 var(--body)}
main{max-width:1120px;margin:0 auto;padding-block:28px 56px;padding-inline:16px;display:grid;gap:28px}
main>*{min-width:0}
h1{font:800 clamp(26px,4vw,38px)/1.05 var(--display);letter-spacing:-.01em;margin:0;text-wrap:balance}
h2{font:700 19px/1.2 var(--display);margin:0 0 10px}
.lede{color:var(--mut);max-width:68ch;margin:8px 0 0}
.eyebrow{font:600 12px var(--mono);letter-spacing:.08em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.bar{display:flex;height:18px;border-radius:4px;overflow:hidden;gap:1px;background:var(--line)}
.bar i{display:block;min-width:2px}
.legend{list-style:none;margin:14px 0 0;padding:0;display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:8px 22px}
.legend li{display:grid;grid-template-columns:14px auto 1fr;column-gap:8px;align-items:baseline;min-width:0}
.sw{width:14px;height:14px;border-radius:3px;display:block;align-self:center}
.nm{font-weight:600}.pc{font:600 13px var(--mono);font-variant-numeric:tabular-nums;justify-self:end}
.ds{grid-column:2/4;color:var(--mut);font-size:13px}
.s-ported{background:var(--s-ported)}.s-read{background:var(--s-read)}.s-helper{background:var(--s-helper)}.s-platform{background:var(--s-platform)}
.s-engine{background:var(--s-engine)}.s-unused{background:var(--s-unused)}.s-open{background:var(--s-open)}.s-unsorted{background:var(--s-unsorted)}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:6px}
#wrap{padding:10px 10px 10px 0;overflow:hidden}canvas{display:block;width:100%;cursor:crosshair;touch-action:manipulation}
#info{font:13px/1.5 var(--mono);padding:10px 12px;min-height:3.2em;border-top:1px solid var(--line);overflow-wrap:anywhere}
#info .st{font-weight:600}#info .m{color:var(--mut)}
.tw{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:14px}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--line);vertical-align:top}
th{font:600 12px var(--mono);letter-spacing:.06em;text-transform:uppercase;color:var(--mut)}
td.mono{font:12.5px var(--mono);white-space:nowrap}td.num{font:12.5px var(--mono);text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
tbody tr td:first-child{box-shadow:inset 3px 0 0 var(--s-open)}
ul.notes{margin:0;padding-left:20px;color:var(--mut);display:grid;gap:6px;max-width:90ch}
.how{color:var(--mut);font-size:13px;margin:8px 0 0}
code{font:12.5px var(--mono)}
</style>
<main>
<header>
<p class="eyebrow">Rampage Total Destruction &middot; SLES-53800 &middot; @NFUNC@ functions &middot; @ALLKB@ KB of code</p>
<h1>RampageRE coverage map</h1>
<p class="lede">Every function of the game's RAMPAGE.ELF, sorted by what the native port does with it. Shares are of the code's bytes.</p>
</header>
<section>
<div class="bar" aria-hidden="true">@BAR@</div>
<ul class="legend">@LEGEND@</ul>
</section>
<section>
<h2>The code by address</h2>
<div class="panel"><div id="wrap"><canvas id="c" role="img" aria-label="Map of every function by address, coloured by status"></canvas></div>
<div id="info"><span class="m">Point at or tap a function for its address, size, name, class and the files that cite it.</span></div></div>
<p class="how">One row is 16 KB; each function is as wide as it is long. The libraries sit at 0x10E000&ndash;0x12D000 (expat) and 0x13C000&ndash;0x166000 (Sony's SDK), newlib at 0x326C00&ndash;0x331900; the game and its engine fill the rest.</p>
</section>
<section>
<h2>Open</h2>
<div class="panel tw"><table><thead><tr><th>Range</th><th>Bytes</th><th>What the game does that the port does not yet</th></tr></thead><tbody>@OPEN@</tbody></table></div>
</section>
<section>
<h2>Also worth a look</h2>
<ul class="notes">@NOTES@</ul>
<p class="how">Made by <code>python tools/coverage.py --html</code> from <code>recomp/funcmap.csv</code>, the addresses <code>src/</code> and <code>docs/</code> cite, and <code>tools/coverage_groups.txt</code>.</p>
</section>
</main>
<script>
const D=@DATA@,N=@NAMES@,ROW=16384,H=7,G=1,LBL=64;
const cv=document.getElementById('c'),info=document.getElementById('info');
const rows=[...new Set(D.flatMap(f=>{const a=Math.floor(f[0]/ROW),b=Math.floor((f[0]+f[1]-1)/ROW),r=[];for(let k=a;k<=b;k++)r.push(k);return r}))].sort((a,b)=>a-b);
const ri=new Map(rows.map((r,k)=>[r,k]));let pinned=null;
const HINT='<span class="m">Point at or tap a function for its address, size, name, class and the files that cite it.</span>';
function css(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim()}
function draw(){const w=cv.parentNode.clientWidth-10,dpr=window.devicePixelRatio||1,h=rows.length*(H+G);
cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);cv.style.height=h+'px';const x=cv.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);
const col=N.map(s=>css('--s-'+s)),mut=css('--mut'),hl=css('--hl');x.font='9px '+css('--mono');x.fillStyle=mut;
rows.forEach((r,k)=>{if(k%8==0||(k>0&&rows[k-1]!=r-1))x.fillText('0x'+(r*ROW).toString(16).toUpperCase(),6,k*(H+G)+H)});
const sc=(w-LBL)/ROW;for(const f of D){let a=f[0],n=f[1];x.fillStyle=col[f[2]];while(n>0){const r=Math.floor(a/ROW),o=a-r*ROW,m=Math.min(n,ROW-o);
x.fillRect(LBL+o*sc,ri.get(r)*(H+G),Math.max(m*sc-0.35,0.6),H);a+=m;n-=m}}
if(pinned){let a=pinned[0],n=pinned[1];x.strokeStyle=hl;x.lineWidth=1.5;while(n>0){const r=Math.floor(a/ROW),o=a-r*ROW,m=Math.min(n,ROW-o);
x.strokeRect(LBL+o*sc-1,ri.get(r)*(H+G)-1,Math.max(m*sc,1)+2,H+2);a+=m;n-=m}}}
function at(e){const b=cv.getBoundingClientRect(),k=Math.floor((e.clientY-b.top)/(H+G));if(k<0||k>=rows.length)return null;
if(e.clientX-b.left<LBL)return null;const sc=(b.width-LBL)/ROW,a=rows[k]*ROW+(e.clientX-b.left-LBL)/sc;
let lo=0,hi=D.length-1,f=null;while(lo<=hi){const m=(lo+hi)>>1;if(D[m][0]<=a){f=D[m];lo=m+1}else hi=m-1}
return f&&a<f[0]+f[1]?f:null}
function esc(t){return String(t).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}
function show(f){if(!f){info.innerHTML=HINT;return}
info.innerHTML='<span class="st" style="color:var(--s-'+N[f[2]]+')">'+N[f[2]]+'</span> &middot; 0x'+f[0].toString(16).toUpperCase()+
' &middot; '+f[1].toLocaleString()+' bytes<br>'+esc(f[3]||'sub_'+f[0].toString(16).toUpperCase())+(f[4]?' <span class="m">'+esc(f[4])+'</span>':'')+
(f[5]?'<br><span class="m">cited in '+esc(f[5])+'</span>':'')}
cv.addEventListener('mousemove',e=>{if(!pinned)show(at(e))});
cv.addEventListener('mouseleave',()=>{if(!pinned)show(null)});
cv.addEventListener('click',e=>{const f=at(e);pinned=f&&f!==pinned?f:null;show(pinned);draw()});
let rt=0;addEventListener('resize',()=>{clearTimeout(rt);rt=setTimeout(draw,60)});
try{matchMedia('(prefers-color-scheme: dark)').addEventListener('change',draw)}catch(e){}
new MutationObserver(draw).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
draw();if(document.fonts)document.fonts.ready.then(draw);
</script>"""


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
    for opt, fn in (('--md', lambda p: write_md(p, funcs, rules)), ('--html', lambda p: write_html(p, funcs, rules, '--fragment' in args))):
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
