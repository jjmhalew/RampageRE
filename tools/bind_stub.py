"""Bind a game function to a PS2Recomp runtime handler, without regenerating all C++.

Adds "handler@0xADDR" to the stubs in recomp/config.toml (so the next full regenerate keeps it), rewrites the
already generated sub_ADDR file to call the handler, and deletes the unity object that contains it so that
`build.ps1 -Quick` recompiles it.

Usage: python tools/bind_stub.py ADDR HANDLER "why"
  e.g. python tools/bind_stub.py 3293E8 memalign_r "_memalign_r trims chunks with newlib arithmetic"
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(ROOT, 'recomp', 'config.toml')
RUNNER = os.path.join(ROOT, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')
UNITY = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'CMakeFiles', 'ps2EntryRunner.dir', 'Unity')
OBJECTS = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'ps2EntryRunner.dir', 'Release')

GENERATED = '''// Rampage: {why}
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include <ps2_recompiled_functions.h>

void {name}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{
    ctx->pc = getRegU32(ctx, 31);
    ps2_stubs::{handler}(rdram, ctx, runtime);
}}
'''


def update_config(addr, handler, why):
    with open(CONFIG, encoding='utf-8') as f:
        text = f.read()
    tag = f'@0x{addr:08X}"'
    # drop any existing binding of this address (entry_points or an older stub)
    text = re.sub(r'\n  "[^"\n]*' + re.escape(tag) + r',?', '', text)
    stubs_start = text.index('stubs = [')
    stubs_end = text.index('\n]', stubs_start)
    text = text[:stubs_end] + f'\n  "{handler}@0x{addr:08X}", # {why}' + text[stubs_end:]
    with open(CONFIG, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    addr, handler, why = int(sys.argv[1], 16), sys.argv[2], sys.argv[3]
    update_config(addr, handler, why)
    print(f'config: {handler}@0x{addr:08X}')

    name = f'sub_{addr:08X}_0x{addr:x}'
    path = os.path.join(RUNNER, name + '.cpp')
    if not os.path.exists(path):
        print(f'{name}.cpp not generated (yet); run a full build.ps1')
        return
    with open(path, 'w', newline='\n') as f:
        f.write(GENERATED.format(why=why, name=name, handler=handler))
    print(f'rewrote {name}.cpp')

    for bundle in glob.glob(os.path.join(UNITY, '*.cxx')):
        with open(bundle) as f:
            if name + '.cpp' in f.read():
                obj = os.path.join(OBJECTS, os.path.basename(bundle)[:-4] + '.obj')
                if os.path.exists(obj):
                    os.remove(obj)
                print(f'dropped {os.path.basename(obj)}')


if __name__ == '__main__':
    main()
