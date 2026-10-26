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
The Wii's textures (.bdg / .cmg) are GX formats in tiles, without palettes: see wii_header and GX.

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


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def gx_i4(b, o):                                     # GX intensity formats give alpha the intensity too
    return [(v, v, v, v) for x in b[o:o + 32] for v in ((x >> 4) * 17, (x & 15) * 17)]


def gx_i8(b, o):
    return [(v, v, v, v) for v in b[o:o + 32]]


def gx_ia8(b, o):
    return [(b[k + 1], b[k + 1], b[k + 1], b[k]) for k in range(o, o + 32, 2)]


def gx_rgb5a3(b, o):
    out = []
    for c in struct.unpack_from('>16H', b, o):
        if c & 0x8000:
            out.append((((c >> 10) & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, (c & 31) * 255 // 31, 255))
        else:
            out.append((((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17, ((c >> 12) & 7) * 255 // 7))
    return out


def gx_rgba8(b, o):
    return [(b[o + 2 * k + 1], b[o + 32 + 2 * k], b[o + 33 + 2 * k], b[o + 2 * k]) for k in range(16)]


def gx_cmpr(b, o):
    """An 8x8 tile: four 4x4 DXT1 blocks (big endian colours, the first pixel in the top bits) in Z order."""
    out = [None] * 64
    for s in range(4):
        at = o + 8 * s
        c0, c1 = struct.unpack_from('>HH', b, at)
        p0, p1 = rgb565(c0), rgb565(c1)
        if c0 > c1:
            colours = [p0 + (255,), p1 + (255,), tuple((2 * x + y) // 3 for x, y in zip(p0, p1)) + (255,),
                       tuple((x + 2 * y) // 3 for x, y in zip(p0, p1)) + (255,)]
        else:
            colours = [p0 + (255,), p1 + (255,), tuple((x + y) // 2 for x, y in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
        base = (s >> 1) * 32 + (s & 1) * 4
        for y in range(4):
            bits = b[at + 4 + y]
            for x in range(4):
                out[base + y * 8 + x] = colours[(bits >> (6 - 2 * x)) & 3]
    return out


# GX texture formats: tile width, height, bits per pixel, tile decoder (the pixels of one tile, rows of tile width)
GX = {0: (8, 8, 4, gx_i4), 1: (8, 4, 8, gx_i8), 3: (4, 4, 16, gx_ia8), 5: (4, 4, 16, gx_rgb5a3),
      6: (4, 4, 32, gx_rgba8), 14: (8, 8, 4, gx_cmpr)}


def wii_header(data):
    """The Wii's texture data (0x68 bytes, big endian): u32 kind, u32 GX format, u16 width, u16 height, u16 mip
    levels, u16 0xFFFF (no palette), the rest zero."""
    kind, fmt, w, h, mips = struct.unpack_from('>IIHHH', data, 0)
    return dict(kind=kind, format=fmt, width=w, height=h, mips=mips)


def wii_size(t):
    """Bytes of all mip levels: each is whole tiles, the size halved down to 1."""
    tw, th, bits = GX[t['format']][:3]
    total = 0
    for k in range(t['mips']):
        w, h = max(1, t['width'] >> k), max(1, t['height'] >> k)
        total += -(-w // tw) * tw * -(-h // th) * th * bits // 8
    return total


def wii_image(data, pixels):
    """Mip level 0 of a Wii texture as a PIL image (RGBA)."""
    t = wii_header(data)
    if t['format'] not in GX:
        raise ValueError(f'GX format {t["format"]} not handled')
    if wii_size(t) != len(pixels):
        raise ValueError(f'{len(pixels)} pixel bytes, expected {wii_size(t)}')
    w, h = t['width'], t['height']
    tw, th, bits, decode = GX[t['format']]
    rows = [[(0, 0, 0, 0)] * (-(-w // tw) * tw) for _ in range(-(-h // th) * th)]
    o = 0
    for ty in range(0, len(rows), th):
        for tx in range(0, len(rows[0]), tw):
            tile = decode(pixels, o)
            for y in range(th):
                rows[ty + y][tx:tx + tw] = tile[y * tw:(y + 1) * tw]
            o += tw * th * bits // 8
    im = Image.new('RGBA', (w, h))
    im.putdata([p for r in rows[:h] for p in r[:w]])
    return im


def export(path, out):
    with open(path, 'rb') as f:
        b = bdp.Bundle(f.read())
    items = list(b.items(0))
    if b.endian == '>':
        os.makedirs(out, exist_ok=True)
        done = 0
        for e, a, d2 in items:
            if e.type == 9:
                name = b.name(e.name).lstrip('\x13').replace('/', '_').replace('|', '_')
                wii_image(a, d2).save(os.path.join(out, name + '.png'))
                done += 1
        return done
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
            if f.lower().endswith(bdp.EXTENSIONS):
                total += export(os.path.join(args[1], f), os.path.join(args[2], os.path.splitext(f)[0]))
        print(f'wrote {total} textures to {args[2]}')
    elif len(args) == 2:
        print(f'wrote {export(args[0], args[1])} textures to {args[1]}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
