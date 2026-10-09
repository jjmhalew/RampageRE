# Effects, string tables and GUI layouts

Three text-like bundle entry types: effect files (type 20), string tables (`.loc`, type 23) and GUI layouts
(`.ifc`, type 24). See [FORMAT_BDP.md](FORMAT_BDP.md) for the bundles that hold them.

## Effects (type 20, "Text EDF file")
297 effects: 287 in `fx.bdp` (explosions, debris, muzzle flashes, lightning, the helicopter's lights, ...), the
rest in `Shell.bdp` and `MemoryCard.bdp` (menu highlights). The bundle loader (0x1A8110) only records them by
name; an effect is parsed when it is first used.

The file is text, one item per line, with its words coded against a dictionary in the game: 342 strings pointed
to from 0x3B1C10 in `RAMPAGE.ELF` (the first are `Text`, `EDF`, `file`, `version`, `type`, `billboard`,
`camerashake`, `composite`, ...). Word n is written as two capitals, n = (first − 'A') × 26 + (second − 'A').

| Token | Meaning |
|---|---|
| `AA`..`ZZ` | a dictionary word; several in a row are separate words (a flag set: `+CycleLoops` `+NoRender`) |
| `?text?` | a literal inside a line, mostly a key that is not in the dictionary (`?enum.fxPriority?`) |
| `?text` | a literal to the end of the line: numbers, texture names, lists (`?0.5 0.5`) |
| `=` | between a key and its value |

A file starts with `AAABAC` ("Text EDF file") and `AD?1` ("version 1"), then `type = particle`, `composite` or
`camerashake` (257, 37 and 3 files). `?{` and `?}` enclose one emitter or part each (`int.emitter_count` of them).
Values that change over a particle's life are graphs: a graph type word and its numbers, `const 30`,
`lininterp "4 0 0 .15 1 .25 .5 1 0"` (a point count, then time/value pairs), `poly "1 1 3"`.

```
Text EDF file
version 1
type = particle
enum.fxPriority = High
int.emitter_count = 4
{
    enum.particle_type = Stretch
    flags.emitter.def = +CycleLoops +EmitterRelativeParticles +ProvidesCollisionRod +PredictableEmissionRate
    flags.particle.def = +AdditiveAlpha +NoRender
    graph.gen_rate = const 30
    graph.em_vel.x = const 150
    graph.p_lifetime.min = const 0.3
    textures.name = basic
    ...
}
```

`tools/edf.py` writes this readable form (dictionary words bare, literals bare or in double quotes) and compiles it
back; `--verify` rebuilds all 297 files byte for byte. The files use 128 of the 342 words.

## String tables (`.loc`, type 23)
13 tables: one per city (block names, challenge texts), `Shell.loc` (menus), `Common.loc`, `Game.loc` and
`MemoryCard.loc`; 574 keys in 9 languages.

| Offset | Type | |
|---|---|---|
| 0x00 | 0x1C bytes | runtime object: u32 0x00612D3C (`<-a`), 1, 1, zeros |
| 0x1C | u32 | 4 (not known) |
| 0x20 | u32 | 9, the number of languages |
| 0x24 | u32 | n, the number of keys |
| 0x28 | u32[11] | offset of each column of n string offsets: the keys, the 9 languages, and an unused column whose offsets are all 0 |

The columns follow at 0x54, then the strings, zero terminated and padded to 4 bytes; every byte of all 13 tables
is accounted for. Column order: keys, English, English (82 strings differ, mostly memory-card wording),
Japanese (Shift-JIS), French, German, Italian, Spanish, Dutch, Portuguese; the Western columns are Windows-1252.
The game picks a column with `getLanguage`; which of the two English columns a European console gets is not
checked. `tools/loc.py` checks the layout (`--verify`) and writes one TSV per table.

## GUI layouts (`.ifc`, type 24)
43 layouts in the GUI bundles, XML with one- or two-letter tags, read directly by the GUI code (the tag names
sit as plain strings near 0x421D80 and 0x422058; there is no dictionary). What the files show:

```xml
<?xml version="1.0"?><i n="AttractMode"><sc>ATTRACTMODE.BSF</sc><f>AttractMode::doInterface</f>
  <DD n="-.2D.-"><q n="Video"><p>320 240</p><d>640 480</d><M>0x410</M><Q>0x6</Q></q>
  <t n="PressStart"><p>587 418</p><L>AttractMode_PressStart</L><g j="r">ARIALBLACK24</g><c>0xFFFF00FF</c>
  <maxwidth>250</maxwidth></t></DD></i>
```

`<i n>` is an interface with its script (`<sc>`) and entry function (`<f>`); `<X n><xf>` names a child interface
file; items have a name (`n`) and a position `<p>` on what appears to be a 640 × 480 screen; `<t>` is a text whose string is a
`.loc` key (`<L>`) drawn in a font (`<g>`, `j` = justification) and colour `<c>` (RGBA); `<q>` is a quad (size
`<d>`, flags `<M>`, `<Q>`). These meanings are read off the files; the other tags (`b`, `m`, `o`, `v`, `bc`, `ep`,
`sl`, ...) are not decoded.
