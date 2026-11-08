r"""Side-by-side traces of the monsters' move runner: the recompiled game against the native port.

The game's side: `rampage_movelog` (patches/ps2recomp-runtime.diff, ps2_pad.cpp) is called on entry to the move
runner 0x2EBFE0 and writes one line per frame of player 1's monster (PS2X_MOVELOG=file): the state it starts the
frame in, its time, the transition picked, the pad variables the runner reads, dt, HP, energy, mode and place.
PS2X_PLAY="300:right/40;400:circle/90" holds pad buttons from that frame for that many frames, PS2X_PLAY_END=n ends
the run after frame n. The port's side: `RampageRE-dev.exe ... --replay FILE --movelog OUT` plays the game's logged
pad variables frame by frame with its dt and writes the same lines (main.c).

A run: `.\test.ps1 -ToGameplay -Play "100:left/30;170:circle/50" -PlayEnd 300 -MoveLog build\movelog_game.txt` (one
pad: PS2X_ONE_PAD; about 0.4 s a game frame, 25 game frames a second of play), then `RampageRE-dev.exe
build\disc\RAMPAGE.VOL LasVegas 1 --monster George --players NP --bots NB --do "replay build\movelog_game.txt"
--movelog build\movelog_port.txt --shot s.png` with the log's np / nb, then `diff`. The replay hands the port the
game's stick frame (+0xA18, +0xA24) too.

-ToGameplay's menu presses go by the clock, so a run may start with a bot partner (np 2 nb 1). The diff skips the
game's 2-frame switch wait (a pick without 0x1000), which the port leaves out on purpose (FORMAT_MONSTER.md "At run
time"). Found with it so far: the forced states' frame, the clip events before the runner (main.c), and the
recompiler's RSQRT.S (fs / sqrt(ft), not 1 / sqrt(fs)), which had left the game's monster unable to turn towards the
stick (its stick direction was never normalised: an angle of 0, or 180 degrees behind it).

Usage: python tools/movelog.py hook             put the call into the generated sub_002EBFE0 (then build.ps1 -Quick)
       python tools/movelog.py diff GAME PORT    compare two logs: the first frames where state, pick or time differ
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')
UNITY = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'CMakeFiles', 'ps2EntryRunner.dir', 'Unity')
OBJECTS = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'ps2EntryRunner.dir', 'Release')
NAME = 'sub_002EBFE0_0x2ebfe0'
MARK = '// RAMPAGE-MOVELOG'
DECL = ('void rampage_movelog(const uint8_t *rdram, uint32_t runner, uint32_t gp); ' + MARK + '\n')
CALL = '    rampage_movelog(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 28)); ' + MARK + '\n'


def hook():
    path = os.path.join(RUNNER, NAME + '.cpp')
    with open(path, encoding='utf-8') as f:
        lines = f.readlines()
    if any(MARK in line for line in lines):
        print(f'{NAME}.cpp already hooked')
        return
    head = next(i for i, line in enumerate(lines) if line.startswith('void ' + NAME + '('))
    lines.insert(head, DECL)
    # after the resume switch (a return into the middle jumps from it), so only a fresh call logs
    first = next(i for i in range(head, len(lines)) if lines[i].strip() == 'ctx->pc = 0x2ebfe0u;')
    lines.insert(first, CALL)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(lines)
    print(f'hooked {NAME}.cpp')
    for bundle in glob.glob(os.path.join(UNITY, '*.cxx')):
        with open(bundle) as f:
            if NAME + '.cpp' in f.read():
                obj = os.path.join(OBJECTS, os.path.basename(bundle)[:-4] + '.obj')
                if os.path.exists(obj):
                    os.remove(obj)
                print(f'dropped {os.path.basename(obj)}')


def read(path):
    rows = []
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w or w[0] != 'F':
                continue
            d = {w[i]: w[i + 1] for i in range(2, len(w) - 1, 2)}
            d['F'] = w[1]
            d['pos'] = w[w.index('pos') + 1:w.index('pos') + 4]
            rows.append(d)
    return rows


def diff(game, port, show=20):
    a, b = read(game), read(port)
    n = min(len(a), len(b))
    print(f'{len(a)} game frames, {len(b)} port frames')
    shown = 0
    for i in range(n):
        x, y = a[i], b[i]
        why = []
        if x['wait'] != '0':
            # the game's switch wait (no 0x1000: the runner holds the old state 2 frames, its clip already started;
            # FORMAT_MONSTER.md "At run time"): the port, which enters at once, is in the state the wait ends in
            j = i
            while j < len(a) and a[j]['wait'] != '0':
                j += 1
            if j < len(a) and y['st'] == a[j]['st']:
                continue
        if x['st'] != y['st']:
            why.append(f"state {x['st']} / {y['st']}")
        if x['pick'] != y['pick'] and x['wait'] == '0':
            why.append(f"pick {x['pick']} / {y['pick']}")
        if abs(float(x['t']) - float(y['t'])) > 0.002:
            why.append(f"time {x['t']} / {y['t']}")
        if why:
            print(f"frame {x['F']}: " + ', '.join(why) + f"   (btn {x['btn']} lx {x['lx']} ly {x['ly']})")
            shown += 1
            if shown >= show:
                break
    if not shown:
        print('no difference in state, pick or time')


def main():
    if len(sys.argv) == 2 and sys.argv[1] == 'hook':
        hook()
    elif len(sys.argv) == 4 and sys.argv[1] == 'diff':
        diff(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
