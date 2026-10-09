"""List and check Rampage GUI layouts (.ifc, bundle entry type 24) and bitmap fonts (.fdf, type 2). See docs/FORMAT_GUI.md.

Layouts are XML with one- or two-letter tags; `list` prints each as an indented tree of its objects (i interface,
o object, q quad, t text, sl slider, DD the 2D root) with their properties.

Fonts (read by OFontData 0x1B7CD0):
  0x000 char[0x100] font name      0x100 char[0x100] bitmap file ("ARIALBLACK24.BMP")
  0x200 char[0x100] Windows LOGFONT as text ("0;-13;0;0;0;400;...;MS Sans Serif")
  0x300 u32 0    0x304 u32 rows of glyphs in the bitmap    0x308 u32 size of the character list at 0x358
  0x30C u32 size of the alias list after it (pairs: character, character whose glyph it shares)
  0x310 f32 character spacing, line spacing, shadow dx, shadow dy
  0x320 u32 shadow colour, 0x324 colour (AABBGGRR), 0x328 u32 (not read), 0x32C f32 slant, 0x330 / 0x334 f32 x / y scale
  then to the end, 0x24 bytes per glyph: f32 u0 u1 v0 v1 (over the bitmap's used size), f32 a (space before),
  w (drawn width), c (space after), h (height; < 0: the cell's), char[4] the character (Shift-JIS: two bytes)

Usage: python tools/gui.py list BUNDLE [NAME]     the layouts (and fonts) of a bundle, or one of them
       python tools/gui.py --verify VOLDIR        parse every layout and font of every bundle
"""
import os
import struct
import sys
import xml.etree.ElementTree as ET

import bdp

NODES = {'i', 'o', 'q', 't', 'sl', 'DD', 'X'}


def font_info(data):
    rows, nchars, nalias = struct.unpack_from('<3I', data, 0x304)
    at = 0x358 + nchars + nalias
    if (len(data) - at) % 0x24:
        raise ValueError(f'glyphs at {at:#x} do not fill the entry ({len(data):#x})')
    head = {
        'name': data[:0x100].split(b'\0')[0].decode('latin1'),
        'bitmap': data[0x100:0x200].split(b'\0')[0].decode('latin1'),
        'rows': rows,
        'spacing': struct.unpack_from('<4f', data, 0x310),
        'colours': struct.unpack_from('<2I', data, 0x320),
        'slant_scale': struct.unpack_from('<3f', data, 0x32C),
        'chars': data[0x358:0x358 + nchars].split(b'\0')[0],
    }
    glyphs = []
    for r in range(at, len(data), 0x24):
        u0, u1, v0, v1, a, w, c, h = struct.unpack_from('<8f', data, r)
        glyphs.append((data[r + 0x20:r + 0x24].split(b'\0')[0], u0, u1, v0, v1, a, w, c, h))
    return head, glyphs


def print_tree(el, depth=0):
    props = []
    for ch in el:
        if ch.tag in NODES:
            continue
        attrs = ' '.join(f'{k}={v}' for k, v in ch.attrib.items())
        props.append(f'{ch.tag}={ch.text or ""}' + (f' [{attrs}]' if attrs else ''))
    print('  ' * depth + f'<{el.tag}> {el.get("n", "")}  ' + '  '.join(props))
    for ch in el:
        if ch.tag in NODES:
            print_tree(ch, depth + 1)


def entries(path):
    with open(path, 'rb') as f:
        b = bdp.Bundle(f.read())
    for e, a, _ in b.items(0):
        yield b.name(e.name), e.type, a


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--verify':
        layouts = fonts = bad = 0
        for f in sorted(os.listdir(args[1])):
            if not f.lower().endswith(('.bdp', '.cmp')):
                continue
            for name, typ, data in entries(os.path.join(args[1], f)):
                try:
                    if typ == 24:
                        ET.fromstring(data.rstrip(b'\0').decode('latin1'))
                        layouts += 1
                    elif typ == 2 and name.lower().endswith('.fdf'):
                        font_info(data)
                        fonts += 1
                except (ValueError, ET.ParseError, struct.error) as err:
                    bad += 1
                    print(f'{f} {name}: {err}')
        print(f'{layouts} layouts, {fonts} fonts; {bad} bad')
        sys.exit(1 if bad else 0)
    elif len(args) in (2, 3) and args[0] == 'list':
        for name, typ, data in entries(args[1]):
            if len(args) == 3 and name.lower() != args[2].lower():
                continue
            if typ == 24:
                print(f'== {name}')
                print_tree(ET.fromstring(data.rstrip(b'\0').decode('latin1')))
            elif typ == 2 and name.lower().endswith('.fdf'):
                head, glyphs = font_info(data)
                print(f'== {name}: {head["name"]} bitmap {head["bitmap"]}, {head["rows"]} rows, spacing/line/shadow '
                      f'{head["spacing"]}, shadow {head["colours"][0]:08x} colour {head["colours"][1]:08x}, '
                      f'slant/scale {head["slant_scale"]}, {len(glyphs)} glyphs')
                for g in glyphs:
                    print(f'   {g[0]!r:8} u {g[1]:.4f}-{g[2]:.4f} v {g[3]:.4f}-{g[4]:.4f}  a {g[5]:g} w {g[6]:g} '
                          f'c {g[7]:g} h {g[8]:g}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
