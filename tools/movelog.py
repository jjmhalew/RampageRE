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
game's stick frame (+0xA18, +0xA24) too. The stick and the frame are logged to 6 decimals since s3p (2026-11-10); the
older logs' 3 can flip a turn's start by a frame where it sits on the steer's threshold.

-ToGameplay's menu presses go by the clock, so a run may start with a bot partner (np 2 nb 1): replay it with
--players 2 --bots 1, or the world's random numbers part at the bot's first draw. The other monsters have B lines
(player, their own count n, player 1's frame; state, time, pick, HP, mode, place, heading, AI flag), which the diff
pairs by player and n.

The game's 2-frame switch wait (a pick without 0x1000: the runner holds the old state while the new clip plays,
FORMAT_MONSTER.md "At run time") is in both logs (wait 1) and compared as any frame. Found with it so far: the forced states' frame, the clip events before the runner
(main.c), how long a press stays new (3 frames, MV_NEW_FOR) and that it is the press's value that counts (a tap let
go still does), the root's step after the turn and the face's hold before the runner; and in the recompiler RSQRT.S
(fs / sqrt(ft), not 1 / sqrt(fs)), which had left the game's monster unable to turn towards the stick (its stick
direction was never normalised: an angle of 0, or 180 degrees behind it), and SQRT.S (the root of ft, not fs) with
DIV.S by 0 (the largest float, not infinity), which had put the game's monster at NaN on a roof's edge.

The game logged here is the PAL one: 25 world steps a second (dt 0.04), where the port plays at 30; a replay takes
the log's step for the whole world (wrand.h world_step: the monster's per-step shares, the traffic's, people's and
objects' scripts) before the block is made.

The random numbers: the game's log has an R line a frame (the world's Park-Miller state, the lagged Fibonacci's i, j
and the sum of its table, GameStatus' two seeds), as does the port's; a replay starts from those seeds. Who draws:
PS2X_RNGLOG=file (the game: frame, P or L, the caller's address, then the return addresses on its stack: a script's
draw (0x279FC4) is under its VM's frames 0x18xxxx .. 0x1Dxxxx, the native caller that ran the script further up)
against RAMPAGE_RNGLOG=file (the port: frame, P or L, its callers by name, the first with its line, Windows). Found
with them: the traffic's type drawn from the wrong generator, the Park-Miller seeded after the block was made, the
block made in another order (the buildings' objects, the edibles, the attack objects, the actors, the AI, the
traffic, the crowds, then the objects' onInit), a building's actor (VegasCowboySign) the game never makes, a clip's
end told the frame after, and in the recompiler CVT.W.S to the nearest (the EE truncates).

Who picks for the AI: PS2X_PICKLOG=file (the game: frame, player, the runner's state, time and wait, the pick before
and the new one, then the callers as PS2X_RNGLOG's) logs every call of 0x2EBF40, which pickByAction and pickByState
end in. Found with it: pickByAction's last matching transition wins whatever its priority.

Usage: python tools/movelog.py hook             put the calls into the generated sub_002EBFE0, sub_00106BD0 and
                                                 sub_00106950 (then build.ps1 -Quick)
       python tools/movelog.py diff GAME PORT    compare two logs: the first frames where state, pick, the wait, time,
                                                 mode, mirroring, HP or energy differ; then the other monsters' (a bot's)
                                                 B lines, paired by player and count
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')
UNITY = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'CMakeFiles', 'ps2EntryRunner.dir', 'Unity')
OBJECTS = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'ps2EntryRunner.dir', 'Release')
MARK = '// RAMPAGE-MOVELOG'
# the move runner's entry (the move log) and the world's two random generators (PS2X_RNGLOG: who draws, P / L)
HOOKS = [
    ('sub_002EBFE0_0x2ebfe0', 0x2ebfe0, 'void rampage_movelog(const uint8_t *rdram, uint32_t runner, uint32_t gp);',
     'rampage_movelog(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 28));'),
    ('sub_00106BD0_0x106bd0', 0x106bd0,
     'void rampage_rnglog(const uint8_t *rdram, char kind, uint32_t a0, uint32_t ra, uint32_t gp, uint32_t sp);',
     "rampage_rnglog(rdram, 'P', GPR_U32(ctx, 4), GPR_U32(ctx, 31), GPR_U32(ctx, 28), GPR_U32(ctx, 29));"),
    ('sub_00106950_0x106950', 0x106950,
     'void rampage_rnglog(const uint8_t *rdram, char kind, uint32_t a0, uint32_t ra, uint32_t gp, uint32_t sp);',
     "rampage_rnglog(rdram, 'L', GPR_U32(ctx, 4), GPR_U32(ctx, 31), GPR_U32(ctx, 28), GPR_U32(ctx, 29));"),
    # where a monster's move takes it (PS2X_POSLOG): the update's start, the foot lock, the push apart, the box test and
    # the matrix set, each with the matrix copy it is handed
    ('sub_002DE530_0x2de530', 0x2de530,
     'void rampage_poslog(const uint8_t *rdram, char tag, uint32_t a0, uint32_t a1, uint32_t ra);',
     "rampage_poslog(rdram, 'U', GPR_U32(ctx, 4), 0, GPR_U32(ctx, 31));"),
    ('sub_0033BF10_0x33bf10', 0x33bf10,
     'void rampage_poslog(const uint8_t *rdram, char tag, uint32_t a0, uint32_t a1, uint32_t ra);',
     "rampage_poslog(rdram, 'F', GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 31));"),
    ('sub_002E4660_0x2e4660', 0x2e4660,
     'void rampage_poslog(const uint8_t *rdram, char tag, uint32_t a0, uint32_t a1, uint32_t ra);',
     "rampage_poslog(rdram, 'P', GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 31));"),
    ('sub_002E2C50_0x2e2c50', 0x2e2c50,
     'void rampage_poslog(const uint8_t *rdram, char tag, uint32_t a0, uint32_t a1, uint32_t ra);',
     "rampage_poslog(rdram, 'B', GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 31));"),
    ('sub_002DCCE0_0x2dcce0', 0x2dcce0,
     'void rampage_poslog(const uint8_t *rdram, char tag, uint32_t a0, uint32_t a1, uint32_t ra);',
     "rampage_poslog(rdram, 'S', GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 31));"),
    # who sets a runner's pick (PS2X_PICKLOG: pickByAction's, pickByState's, the pad pick's)
    ('sub_002EBF40_0x2ebf40', 0x2ebf40,
     'void rampage_picklog(const uint8_t *rdram, uint32_t runner, uint32_t pick, uint32_t ra, uint32_t sp);',
     'rampage_picklog(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 31), GPR_U32(ctx, 29));'),
]


def hook():
    for name, addr, decl, call in HOOKS:
        path = os.path.join(RUNNER, name + '.cpp')
        with open(path, encoding='utf-8') as f:
            lines = f.readlines()
        if any(MARK in line for line in lines):
            print(f'{name}.cpp already hooked')
            continue
        head = next(i for i, line in enumerate(lines) if line.startswith('void ' + name + '('))
        lines.insert(head, decl + ' ' + MARK + '\n')
        # after the resume switch (a return into the middle jumps from it), so only a fresh call logs
        first = next(i for i in range(head, len(lines)) if lines[i].strip() == f'ctx->pc = {addr:#x}u;')
        lines.insert(first, '    ' + call + ' ' + MARK + '\n')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.writelines(lines)
        print(f'hooked {name}.cpp')
        for bundle in glob.glob(os.path.join(UNITY, '*.cxx')):
            with open(bundle) as f:
                if name + '.cpp' in f.read():
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
        if x['F'] == '1':                                # the game's first line is the runner before the level's reset
            continue
        if x['st'] != y['st']:
            why.append(f"state {x['st']} / {y['st']}")
        if x['pick'] != y['pick']:
            why.append(f"pick {x['pick']} / {y['pick']}")
        if x['wait'] != y['wait']:
            why.append(f"wait {x['wait']} / {y['wait']}")
        if abs(float(x['t']) - float(y['t'])) > 0.002:
            why.append(f"time {x['t']} / {y['t']}")
        for key in ('mode', 'mir'):
            if x.get(key) != y.get(key):
                why.append(f"{key} {x.get(key)} / {y.get(key)}")
        for key in ('hp', 'en'):
            if key in x and key in y and abs(float(x[key]) - float(y[key])) > 0.01:
                why.append(f"{key} {x[key]} / {y[key]}")
        if why:
            print(f"frame {x['F']}: " + ', '.join(why) + f"   (btn {x['btn']} lx {x['lx']} ly {x['ly']})")
            shown += 1
            if shown >= show:
                break
    if not shown:
        print('no difference in state, pick, wait, time, mode, mirroring, HP or energy')


def read_bots(path):
    """The other monsters' B lines: {(player, n): fields}."""
    out = {}
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w or w[0] != 'B':
                continue
            d = {w[i]: w[i + 1] for i in range(2, len(w) - 1, 2)}
            d['F'] = w[1]
            d['pos'] = [float(x) for x in w[w.index('pos') + 1:w.index('pos') + 4]]
            out[(int(d['p']), int(d['n']))] = d
    return out


def diff_bots(game, port, show=10):
    a, b = read_bots(game), read_bots(port)
    if not a and not b:
        return
    print(f'{len(a)} game lines of the other monsters, {len(b)} port lines')
    shown = 0
    for key in sorted(a):
        if key not in b or key[1] == 1:              # (the first: the runner before the level's reset)
            continue
        x, y = a[key], b[key]
        why = []
        if x['st'] != y['st']:
            why.append(f"state {x['st']} / {y['st']}")
        if x['pick'] != y['pick']:
            why.append(f"pick {x['pick']} / {y['pick']}")
        if x['wait'] != y['wait']:
            why.append(f"wait {x['wait']} / {y['wait']}")
        if abs(float(x['t']) - float(y['t'])) > 0.002:
            why.append(f"time {x['t']} / {y['t']}")
        if x['mode'] != y['mode']:
            why.append(f"mode {x['mode']} / {y['mode']}")
        if abs(float(x['hp']) - float(y['hp'])) > 0.01:
            why.append(f"hp {x['hp']} / {y['hp']}")
        far = max(abs(p - q) for p, q in zip(x['pos'], y['pos']))
        if far > 0.05:
            why.append('pos ' + ' '.join(f'{p:.3f}' for p in x['pos']) + ' / ' +
                       ' '.join(f'{q:.3f}' for q in y['pos']))
        if why:
            print(f"player {key[0]} line {key[1]} (frame {x['F']} / {y['F']}): " + ', '.join(why))
            shown += 1
            if shown >= show:
                break
    if not shown:
        print('the other monsters: no difference in state, pick, wait, time, mode, HP or place')


def main():
    if len(sys.argv) == 2 and sys.argv[1] == 'hook':
        hook()
    elif len(sys.argv) == 4 and sys.argv[1] == 'diff':
        diff(sys.argv[2], sys.argv[3])
        diff_bots(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
