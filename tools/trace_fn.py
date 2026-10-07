"""Temporarily log entry into generated functions (debug aid; a full build.ps1 regenerate removes it).

Usage: python tools/trace_fn.py ADDR [ADDR ...]
Then: .\build.ps1 -Quick ; .\test.ps1   -> lines "[enter] 0x..." in build\test.err
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')
UNITY = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'CMakeFiles', 'ps2EntryRunner.dir', 'Unity')
OBJECTS = os.path.join(ROOT, 'build', 'cmake', 'ps2xRuntime', 'ps2EntryRunner.dir', 'Release')

for arg in sys.argv[1:]:
    addr = int(arg, 16)
    name = f'sub_{addr:08X}_0x{addr:x}'
    path = os.path.join(RUNNER, name + '.cpp')
    with open(path) as f:
        text = f.read()
    head = f'void {name}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{'
    probe = (f'\n    {{ static int n = 0; if (n++ < 20) fprintf(stderr, "[enter] 0x{addr:x} a0=0x%x ra=0x%x\\n", '
             f'(unsigned)getRegU32(ctx, 4), (unsigned)getRegU32(ctx, 31)); }}')
    if '[enter]' not in text:
        text = text.replace(head, head + probe, 1)
        if '#include <cstdio>' not in text:
            text = '#include <cstdio>\n' + text
        with open(path, 'w', newline='\n') as f:
            f.write(text)
    for bundle in glob.glob(os.path.join(UNITY, '*.cxx')):
        with open(bundle) as f:
            if name + '.cpp' in f.read():
                obj = os.path.join(OBJECTS, os.path.basename(bundle)[:-4] + '.obj')
                if os.path.exists(obj):
                    os.remove(obj)
    print('probe in', name)
