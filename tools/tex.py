"""Export the textures in Rampage bundles (entry types 9 = texture, 13 = palette) to PNG.

Texture entry data (0x104 bytes, little endian):
  0x00 u32 kind (1, 3 or 9)   0x04 u8 GS pixel format: 0x00 PSMCT32, 0x02 PSMCT16, 0x13 PSMT8
  0x07 u8 flags: 0x40 = pixels stored in GS memory order (swizzled)
  0x08 u16 width   0x0A u16 height   0x0C u32 mip levels   0x10 u32 pixel bytes (data2)
  0x14 u16 used width   0x16 u16 used height   0x18 f32 u scale   0x1C f32 v scale (used / full size)
  0x20 u32 name index of the palette entry (0xFFFF: none); the rest is zero
The pixels (all mip levels, largest first) are the entry's data2. A palette's data2 is 256 RGBA colours in GS
CSM1 order (entries 8-15 and 16-23 of each 32 swapped), alpha 0x80 = opaque. A texture can also name the
engine's own palettes `_Alpha8` (white, alpha 0-255: fonts) and `_Intensity8` (grey ramp).

Usage: python tools/tex.py BUNDLE OUTDIR             write every texture of one bundle
       python tools/tex.py --all VOLDIR OUTDIR       every bundle, one folder each
"""
import os
import struct
import sys

from PIL import Image

import bdp

PSMCT32, PSMCT16, PSMT8 = 0x00, 0x02, 0x13


def unswizzle8(src, w, h):
    """PSMT8 pixels written to GS memory as PSMCT32 back to rows."""
    out = bytearray(w * h)
    for y in range(h):
        block_row = (y & ~0xF) * w
        swap = (((y + 2) >> 2) & 1) * 4
        column = ((((y & ~3) >> 1) + (y & 1)) & 7) * w * 2
        byte_y = (y >> 1) & 1
        for x in range(w):
            out[y * w + x] = src[block_row + (x & ~0xF) * 2 + column + ((x + swap) & 7) * 4 + byte_y + ((x >> 2) & 2)]
    return out


def palette(data2):
    colours = [(data2[i], data2[i + 1], data2[i + 2], min(255, data2[i + 3] * 2)) for i in range(0, 1024, 4)]
    return [colours[(i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1)] for i in range(256)]


# Palettes the engine makes itself; textures name them like a palette entry (fonts use _Alpha8).
BUILTIN = {
    '_Alpha8': [(255, 255, 255, i) for i in range(256)],
    '_Intensity8': [(i, i, i, 255) for i in range(256)],
}


def header(data):
    kind, psm, flags, w, h, mips, size, uw, uh, us, vs, pal = struct.unpack_from('<IB2xBHHIIHHffI', data, 0)
    return dict(kind=kind, psm=psm, flags=flags, width=w, height=h, mips=mips, size=size, used=(uw, uh),
                scale=(us, vs), palette=pal)


def image(data, pixels, palettes):
    """Mip level 0 as a PIL image (RGBA)."""
    t = header(data)
    w, h = t['width'], t['height']
    if t['psm'] == PSMT8:
        index = unswizzle8(pixels, w, h) if t['flags'] & 0x40 else pixels[:w * h]
        pal_entry = palettes.get(t['palette'])
        if pal_entry is None:
            raise ValueError(f'palette entry {t["palette"]} missing')
        colours = BUILTIN[pal_entry] if isinstance(pal_entry, str) else palette(pal_entry[2])
        im = Image.new('RGBA', (w, h))
        im.putdata([colours[i] for i in index])
        return im
    if t['psm'] == PSMCT32:
        raw = bytearray(pixels[:w * h * 4])
        raw[3::4] = bytes(min(255, a * 2) for a in raw[3::4])
        return Image.frombytes('RGBA', (w, h), bytes(raw))
    if t['psm'] == PSMCT16:
        px = struct.unpack_from(f'<{w * h}H', pixels)
        im = Image.new('RGBA', (w, h))
        im.putdata([((p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3, 255 if p & 0x8000 else 0)
                    for p in px])
        return im
    raise ValueError(f'pixel format {t["psm"]:#x} not handled')


def export(path, out):
    with open(path, 'rb') as f:
        b = bdp.Bundle(f.read())
    items = list(b.items(0))
    palettes = {e.name: (e, a, d2) for e, a, d2 in items if e.type == 13}
    palettes.update({i: n for i, n in enumerate(b.names) if n in BUILTIN})
    os.makedirs(out, exist_ok=True)
    done = 0
    for e, a, d2 in items:
        if e.type != 9:
            continue
        name = b.name(e.name).lstrip('\x13').replace('/', '_').replace('|', '_')
        image(a, d2, palettes).save(os.path.join(out, name + '.png'))
        done += 1
    return done


def main():
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == '--all':
        total = 0
        for f in sorted(os.listdir(args[1])):
            if f.lower().endswith(('.bdp', '.cmp')):
                total += export(os.path.join(args[1], f), os.path.join(args[2], os.path.splitext(f)[0]))
        print(f'wrote {total} textures to {args[2]}')
    elif len(args) == 2:
        print(f'wrote {export(args[0], args[1])} textures to {args[1]}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
