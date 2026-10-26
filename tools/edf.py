"""Decompile and compile Rampage effect files ("Text EDF file", bundle entry type 20: particles, swooshes, lens
flares, camera shakes, ... in fx.bdp and the city and GUI bundles).

An effect file is text, one item per line, with the words coded against a dictionary in the game: 342 strings
pointed to from 0x3B1C10 in RAMPAGE.ELF (word n is written as two capitals: n = (first - 'A') * 26 + (second - 'A')).
A line is a run of tokens:
  AA..ZZ      a dictionary word; several in a row are separate words (flags: +CycleLoops+NoRender...)
  ?text?      a literal word in the middle of a line (a key that is not in the dictionary: ?enum.fxPriority?)
  ?text       a literal to the end of the line (numbers, texture names, lists: ?0.5 0.5)
  =           between a key and its value
The file starts with "Text EDF file" (AAABAC) and "version 1"; ?{ and ?} open and close the parts of a
composite effect (one per emitter).

Readable form (this tool's output): the same tokens separated by spaces, dictionary words bare, literals bare
unless they hold spaces or would read as a dictionary word (then in double quotes), indented by { } depth:
    type = particle
    enum.fxPriority = High
    graph.gen_rate = const 30
    flags.emitter.def = +CycleLoops +EmitterRelativeParticles

Usage: python tools/edf.py --verify VOLDIR          decompile and recompile every effect, compare byte for byte
       python tools/edf.py VOLDIR OUTDIR            write OUTDIR/<bundle>/<effect>.edf.txt
       python tools/edf.py --compile FILE.edf.txt   write the coded form next to it (.edf)
"""
import os
import struct
import sys

import bdp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFF, BASE = 0x1000, 0x100000
TABLE, WORDS = 0x3B1C10, 342


def load_dictionary(elf_path=os.path.join(ROOT, 'build', 'disc', 'RAMPAGE.ELF')):
    with open(elf_path, 'rb') as f:
        elf = f.read()
    words = []
    for i in range(WORDS):
        at = struct.unpack_from('<I', elf, TABLE - BASE + OFF + 4 * i)[0] - BASE + OFF
        words.append(elf[at:elf.index(b'\0', at)].decode('latin1'))
    return words


def tokens(line):
    """[('word', n) | ('lit', text) | ('=',)] of one coded line."""
    out, i = [], 0
    while i < len(line):
        c = line[i]
        if c == '=':
            out.append(('=',))
            i += 1
        elif c == '?':
            j = line.find('?', i + 1)
            if j < 0:
                out.append(('lit', line[i + 1:]))
                i = len(line)
            else:
                out.append(('lit', line[i + 1:j]))
                i = j + 1
        elif 'A' <= c <= 'Z' and i + 1 < len(line) and 'A' <= line[i + 1] <= 'Z':
            out.append(('word', (ord(c) - 65) * 26 + ord(line[i + 1]) - 65))
            i += 2
        else:
            raise ValueError(f'unexpected {c!r} in {line!r}')
    return out


def code(toks):
    """The coded line of a token list (a literal is closed with ? unless it ends the line)."""
    out = []
    for k, t in enumerate(toks):
        if t[0] == '=':
            out.append('=')
        elif t[0] == 'word':
            n = t[1]
            out.append(chr(65 + n // 26) + chr(65 + n % 26))
        else:
            out.append('?' + t[1] + ('?' if k + 1 < len(toks) else ''))
    return ''.join(out)


def decompile(data, words):
    known = set(words)
    lines, depth = [], 0
    text = data.decode('latin1')
    if not text.endswith('\n'):
        raise ValueError('no final newline')
    for line in text[:-1].split('\n'):
        toks = tokens(line)
        if code(toks) != line:
            raise ValueError(f'{line!r} does not code back the same way')
        parts = []
        for t in toks:
            if t[0] == '=':
                parts.append('=')
            elif t[0] == 'word':
                if t[1] >= len(words):
                    raise ValueError(f'word {t[1]} outside the dictionary')
                parts.append(words[t[1]])
            else:
                s = t[1]
                plain = s and ' ' not in s and '"' not in s and s not in known and s != '='
                parts.append(s if plain else '"' + s + '"')
        if parts == ['}']:
            depth -= 1
        lines.append('    ' * depth + ' '.join(parts))
        if parts == ['{']:
            depth += 1
    return '\n'.join(lines) + '\n'


def split_readable(line):
    parts, i = [], 0
    while i < len(line):
        if line[i] == ' ':
            i += 1
        elif line[i] == '"':
            j = line.index('"', i + 1)
            parts.append(('q', line[i + 1:j]))
            i = j + 1
        else:
            j = line.find(' ', i)
            j = len(line) if j < 0 else j
            parts.append(('b', line[i:j]))
            i = j
    return parts


def compile_source(text, words):
    index = {}
    for n, w in enumerate(words):
        index.setdefault(w, n)
    out = []
    for line in text.split('\n')[:-1]:
        toks = []
        for kind, s in split_readable(line.strip()):
            if kind == 'b' and s == '=':
                toks.append(('=',))
            elif kind == 'b' and s in index:
                toks.append(('word', index[s]))
            else:
                toks.append(('lit', s))
        out.append(code(toks))
    return ('\n'.join(out) + '\n').encode('latin1')


def effects(voldir):
    for f in sorted(os.listdir(voldir)):
        if not f.lower().endswith(bdp.EXTENSIONS):
            continue
        with open(os.path.join(voldir, f), 'rb') as h:
            b = bdp.Bundle(h.read())
        for e, a, _ in b.items(0):
            if e.type == 20:
                yield f, b.name(e.name), a


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        words = load_dictionary()
        n = bad = 0
        used = set()
        for f, name, data in effects(args[1]):
            try:
                if compile_source(decompile(data, words), words) != data:
                    raise ValueError('recompiled bytes differ')
                used.update(t[1] for line in data.decode('latin1').split('\n') for t in tokens(line) if t[0] == 'word')
                n += 1
            except (ValueError, UnicodeError) as err:
                bad += 1
                print(f'{f} {name}: {err}')
        print(f'{n} effects decompiled and rebuilt byte for byte, {bad} failed; {len(used)} of {len(words)} '
              f'dictionary words in use')
        sys.exit(1 if bad else 0)
    elif len(args) == 2 and args[0] == '--compile':
        words = load_dictionary()
        with open(args[1], encoding='latin1') as f:
            data = compile_source(f.read(), words)
        out = args[1][:-4] if args[1].endswith('.txt') else args[1] + '.edf'
        with open(out, 'wb') as f:
            f.write(data)
        print(f'wrote {out}')
    elif len(args) == 2:
        words = load_dictionary()
        count = 0
        for f, name, data in effects(args[0]):
            d = os.path.join(args[1], os.path.splitext(f)[0])
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, bdp.safe_name(name) + '.edf.txt'), 'w', encoding='latin1', newline='\n') as out:
                out.write(decompile(data, words))
            count += 1
        print(f'wrote {count} effects to {args[1]}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
