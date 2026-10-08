"""Decompile Rampage scripts (.bsf, "Compiled PPTokenStream version 1.03") to source text, and compile them back.

A .bsf is the preprocessed token stream of a C-like script (`ActorSwarm.scr` plus the headers it includes, such as
`Common.h`). The game parses and compiles it when a level or screen loads (0x1D4870 opens `<name>.bsf`, 0x1C3F90
checks the header, 0x1C6220 parses it). Comments and whitespace are gone, but every token keeps its line, so the
source comes back with its original line numbers. See docs/FORMAT_BSF.md.

Layout (little endian):
  0x00  char[0x25] "Compiled PPTokenStream version 1.03.\\x1a", zero padded to 0x28
  0x28  u32  file size
  0x2C  u32  number of tokens (line and file markers not counted)
  0x30  u32  offset of the file name table: NUL-terminated names in order of first use, up to the end of the file
  0x34  token stream, ended by 0xFD, then the string pool (every identifier and string once, in order of first use)
Token: one byte, the index into the token name table at 0x419358 in the ELF. STRING (0x26) and IDENTIFIER (0x28)
are followed by a u32 offset of their text, NUMBER (0x27) by an f32 and INT (0x45) by a u32.
After a token: 0xFF n = the token is in file n; 0xFE n = it is n lines below the previous token of its file.
Every file starts at line 1, in file 0.

Usage: python tools/bsf.py FILE.bsf                 print the decompiled source (all files)
       python tools/bsf.py --all VOLDIR OUTDIR      write every script as OUTDIR/<Name>.scr, headers in OUTDIR/include
       python tools/bsf.py --verify VOLDIR          decompile, recompile and compare every .bsf byte for byte
"""
import collections
import decimal
import os
import re
import struct
import sys

MAGIC = b'Compiled PPTokenStream version 1.03.\x1a'
TOKENS = [
    '==', '!=', '*', '+', '-', '/', '%', '&', '~', '|', '^', '&&', '||', '!', '<<', '>>',
    '<', '>', '>=', '<=', '=', '(', ')', '{', '}', '[', ']', ',', ';', '.', '?', ':',
    '::', '->', '++', '--', '/**', '**/', None, None, None, '#', '##', None, 'for', 'if', 'else', 'return',
    'break', 'enum', 'continue', 'extern', 'namespace', 'forceref', 'const', 'while', 'do', 'struct', 'public',
]
STRING, NUMBER, IDENT, INT = 0x26, 0x27, 0x28, 0x45
NEWLINE, FILE, END = 0xFE, 0xFF, 0xFD
KEYWORDS = {t: i for i, t in enumerate(TOKENS) if t and t[0].isalpha()}
OPERATORS = sorted(((t, i) for i, t in enumerate(TOKENS) if t and not t[0].isalpha()), key=lambda x: -len(x[0]))

Token = collections.namedtuple('Token', 'code value file line')  # value: text, int or float


def cstr(data, offset):
    return data[offset:data.index(b'\0', offset)].decode('latin1')


# ---------------------------------------------------------------- .bsf -> tokens

def read(data):
    """Returns (file names, [Token]) of a .bsf."""
    if not data.startswith(MAGIC):
        raise ValueError('not a compiled PPTokenStream')
    size, count, names_at = struct.unpack_from('<III', data, 0x28)
    if size != len(data):
        raise ValueError(f'size field {size} != file size {len(data)}')
    names = [n.decode('latin1') for n in data[names_at:].split(b'\0') if n]
    out = []
    last_line = collections.defaultdict(lambda: 1)
    i = 0x34
    while True:
        code = data[i]
        i += 1
        if code == END:
            break
        if code in (NEWLINE, FILE):
            if not out:
                raise ValueError(f'marker before the first token at {i - 1:#x}')
            t = out[-1]
            if code == FILE:
                t = t._replace(file=data[i], line=last_line[data[i]])
            else:
                t = t._replace(line=t.line + data[i])
            out[-1] = t
            last_line[t.file] = t.line
            i += 1
            continue
        if code in (STRING, NUMBER, IDENT, INT):
            raw = data[i:i + 4]
            i += 4
            if code in (STRING, IDENT):
                value = cstr(data, struct.unpack('<I', raw)[0])
            else:
                value = struct.unpack('<f' if code == NUMBER else '<I', raw)[0]
        elif code < len(TOKENS) and TOKENS[code] is not None:
            value = TOKENS[code]
        else:
            raise ValueError(f'unknown token {code:#x} at {i - 1:#x}')
        file = out[-1].file if out else 0
        out.append(Token(code, value, file, last_line[file]))
    if len(out) != count:
        raise ValueError(f'{len(out)} tokens, header says {count}')
    return names, out


# ---------------------------------------------------------------- tokens -> .bsf

def write(names, tokens):
    stream = bytearray()
    pool = bytearray()
    pool_index = {}
    refs = []  # (position in stream, pool offset)
    last_line = collections.defaultdict(lambda: 1)
    file = 0
    for t in tokens:
        stream.append(t.code)
        if t.code in (STRING, IDENT):
            if t.value not in pool_index:
                pool_index[t.value] = len(pool)
                pool += t.value.encode('latin1') + b'\0'
            refs.append((len(stream), pool_index[t.value]))
            stream += b'\0\0\0\0'
        elif t.code == NUMBER:
            stream += struct.pack('<f', t.value)
        elif t.code == INT:
            stream += struct.pack('<I', t.value)
        if t.file != file:
            stream += bytes((FILE, t.file))
            file = t.file
        delta = t.line - last_line[file]
        if not 0 <= delta < 256:
            raise ValueError(f'line step {delta} in {names[file]}:{t.line} does not fit')
        if delta:
            stream += bytes((NEWLINE, delta))
        last_line[file] = t.line
    stream.append(END)
    pool_at = 0x34 + len(stream)
    for pos, offset in refs:
        stream[pos:pos + 4] = struct.pack('<I', pool_at + offset)
    names_blob = b''.join(n.encode('latin1') + b'\0' for n in names)
    names_at = pool_at + len(pool)
    names_blob += b'\0' * (-(names_at + len(names_blob)) % 4)
    size = names_at + len(names_blob)
    return MAGIC + b'\0' * (0x28 - len(MAGIC)) + struct.pack('<III', size, len(tokens), names_at) + \
        bytes(stream) + bytes(pool) + names_blob


# ---------------------------------------------------------------- tokens -> source text

UNARY_AFTER = {t for t in TOKENS[:0x24] if t} - {')', ']', '++', '--'} | {'return', 'case'}
NO_SPACE_BEFORE = {')', ']', ',', ';', '.', '->', '::'}
NO_SPACE_AFTER = {'(', '[', '.', '->', '::', '!', '~'}


def token_text(t):
    if t.code == IDENT:
        return t.value
    if t.code == STRING:
        return '"' + t.value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'
    if t.code == INT:
        return str(t.value) if t.value < 0x80000000 else f'{t.value:#x}'
    if t.code == NUMBER:
        return float_text(t.value)
    return t.value


def float_text(v):
    """Shortest decimal that reads back as the same f32 (0.1, not 0.10000000149011612), always with a '.'"""
    if v != v or v in (float('inf'), float('-inf')):
        raise ValueError(f'float {v} has no source form')
    for digits in range(1, 10):
        t = f'{v:.{digits}g}'
        if struct.unpack('<f', struct.pack('<f', float(t)))[0] == v:
            break
    text = format(decimal.Decimal(t), 'f')
    return text if '.' in text else text + '.0'


def decompile(names, tokens):
    """Returns {file name: source text}. Tokens stay on their original lines; spacing and indentation are
    regenerated, and the #include lines are put back on a free line where the stream switched files."""
    main = next((i for i, n in enumerate(names) if n.lower().endswith('.scr')), len(names) - 1)
    lines = {i: collections.defaultdict(list) for i in range(len(names))}  # file -> line -> [(text, depth)]
    depth = collections.Counter()
    includes = []  # [parent file, parent's last line before the include, included file]
    stack = [main]
    seen = {main}
    prev_file = None
    last_line = collections.defaultdict(int)
    for t in tokens:
        if t.file != prev_file:
            if t.file not in seen:
                includes.append([stack[-1], last_line[stack[-1]], t.file])
                stack.append(t.file)
                seen.add(t.file)
            else:
                while stack and stack[-1] != t.file:
                    stack.pop()
                if not stack:
                    stack = [t.file]
            prev_file = t.file
        text = token_text(t)
        if text == '}':
            depth[t.file] = max(0, depth[t.file] - 1)
        lines[t.file][t.line].append((text, depth[t.file]))
        if text == '{':
            depth[t.file] += 1
        last_line[t.file] = t.line
    # A header that switched to another header and never resumed did not include it: its own includer did.
    resumed, entered = set(), {tokens[0].file} if tokens else set()
    for a, b in zip(tokens, tokens[1:]):
        if a.file != b.file:
            (resumed if b.file in entered else entered).add(b.file)
    by_child = {inc[2]: inc for inc in includes}
    for inc in includes:
        while inc[0] != main and inc[0] not in resumed and inc[0] in by_child:
            parent = by_child[inc[0]]
            inc[0], inc[1] = parent[0], parent[1]
    for parent, after, child in includes:
        line = after + 1
        while lines[parent].get(line):
            line += 1
        lines[parent][line].append((f'#include "{names[child]}"', 0))
    out = {}
    for f, by_line in lines.items():
        text = [join(by_line.get(n, [])) for n in range(1, max(by_line, default=0) + 1)]
        out[names[f]] = '\n'.join(text) + '\n'
    return out


def join(parts):
    if not parts:
        return ''
    line = '    ' * parts[0][1]
    prev = real = None
    for text, _ in parts:
        if real in ('-', '+') and text[:1] == real:  # "- -2.0" must not read back as "--"
            line += ' '
        elif prev is not None and (text.startswith('#include') or (
                text not in NO_SPACE_BEFORE and prev not in NO_SPACE_AFTER
                and not (text in ('(', '[') and (prev[:1].isalpha() or prev[:1] == '_') and prev not in KEYWORDS))):
            line += ' '
        line += text
        unary = text in ('-', '+', '!', '~') and (prev is None or prev in UNARY_AFTER or prev in KEYWORDS)
        prev = '(' if unary else text
        real = text
    return line.rstrip()


# ---------------------------------------------------------------- source text -> tokens

LEX = re.compile(r'''
    (?P<space>[ \t]+)
  | (?P<include>\#include[ \t]+"(?P<path>[^"]+)")
  | (?P<string>"(?:[^"\\]|\\.)*")
  | (?P<number>\d+\.\d*(?:[eE][-+]?\d+)?|\d+[eE][-+]?\d+)
  | (?P<int>0[xX][0-9a-fA-F]+|\d+)
  | (?P<name>[A-Za-z_]\w*)
''', re.X)


def compile_source(sources, main_name):
    """sources: {file name: text}. Returns (file names, [Token]) laid out as the game's compiler wrote them."""
    names = []
    tokens = []

    def lex(name):
        for n, line in enumerate(sources[name].split('\n'), 1):
            i = 0
            while i < len(line):
                m = LEX.match(line, i)
                if m and m.lastgroup == 'space':
                    i = m.end()
                    continue
                if m and m.group('include'):
                    lex(m.group('path'))
                    i = m.end()
                    continue
                if m:
                    kind, text = m.lastgroup, m.group()
                    if kind == 'string':
                        code = STRING
                        value = re.sub(r'\\(.)', lambda e: {'n': '\n', 't': '\t'}.get(e[1], e[1]), text[1:-1])
                    elif kind == 'number':
                        code, value = NUMBER, struct.unpack('<f', struct.pack('<f', float(text)))[0]
                    elif kind == 'int':
                        code, value = INT, int(text, 0)
                    else:
                        code, value = KEYWORDS.get(text, IDENT), text
                    i = m.end()
                else:
                    op = next(((t, c) for t, c in OPERATORS if line.startswith(t, i)), None)
                    if op is None:
                        raise ValueError(f'{name}:{n}: cannot read {line[i:i + 20]!r}')
                    value, code = op
                    i += len(value)
                if name not in names:
                    names.append(name)
                tokens.append(Token(code, value, names.index(name), n))

    lex(main_name)
    return names, tokens


# ---------------------------------------------------------------- command line

def main():
    args = sys.argv[1:]
    if len(args) == 1:
        with open(args[0], 'rb') as f:
            for name, text in decompile(*read(f.read())).items():
                print(f'// ==== {name}')
                print(text)
    elif len(args) == 3 and args[0] == '--all':
        out, inc = args[2], os.path.join(args[2], 'include')
        os.makedirs(inc, exist_ok=True)
        headers = collections.defaultdict(collections.Counter)
        count = 0
        for entry in sorted(os.listdir(args[1])):
            if entry.lower().endswith('.bsf'):
                with open(os.path.join(args[1], entry), 'rb') as f:
                    for name, text in decompile(*read(f.read())).items():
                        if name.lower().endswith('.scr'):
                            with open(os.path.join(out, name), 'w', encoding='latin1', newline='\n') as o:
                                o.write(text)
                            count += 1
                        else:
                            headers[name][text] += 1
        for name, versions in headers.items():
            # a header holds only the part its includer used (the rest was #ifdef'd out): keep the fullest one
            with open(os.path.join(inc, name), 'w', encoding='latin1', newline='\n') as o:
                o.write(max(versions, key=len))
        print(f'wrote {count} scripts to {out} and {len(headers)} headers to {inc}')
    elif len(args) == 2 and args[0] == '--verify':
        ok = 0
        files = sorted(e for e in os.listdir(args[1]) if e.lower().endswith('.bsf'))
        for entry in files:
            with open(os.path.join(args[1], entry), 'rb') as f:
                data = f.read()
            names, tokens = read(data)
            sources = decompile(names, tokens)
            main_name = next(n for n in names if n.lower().endswith('.scr'))
            again = write(*compile_source(sources, main_name))
            if again == data:
                ok += 1
            else:
                where = next((i for i, (a, b) in enumerate(zip(again, data)) if a != b), min(len(again), len(data)))
                print(f'{entry}: differs at {where:#x} (rebuilt {len(again)} bytes, original {len(data)})')
        print(f'{ok}/{len(files)} scripts rebuilt byte for byte')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
