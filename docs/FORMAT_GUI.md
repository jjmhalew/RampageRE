# GUI layouts, bitmap fonts and the in-play HUD

How the game draws its 2D interface, and the in-play HUD that `src/hud.c` rebuilds from the game's files (the text
banners, the challenge status and the results screens it also runs are in [SCREENS_NOTES.md](SCREENS_NOTES.md)). The
layout files (`.ifc`, type 24) are introduced in [FORMAT_EDF.md](FORMAT_EDF.md). `tools/gui.py` lists layouts and fonts
(`list BUNDLE [NAME]`) and parses all of them (`--verify build/vol`: 38 layouts and 6 fonts in the bundles).
Code addresses refer to `RAMPAGE.ELF`. **[R]** means read in the code or files. **[G]** means inferred.

## Where the HUD lives
| What | Where |
|---|---|
| Per-player HUD | `Hud.ifc` (interface `Hud`, with child interfaces `HUD0..3.IFC`) and `Hud0.ifc`..`Hud3.ifc` in `WorldGui.bdp`. Each is the same tree with its group at x 48, 158, 392 or 502, y 60 |
| Its logic | `HUD.BSF`, `Hud::doInterface` (`build/scripts/Hud.scr`) |
| Game clock | `Clock.ifc` + `CLOCK.BSF` (`Clock.scr`), loaded by `WorldGuiDriver` |
| Monster heads | `Hud_<Monster>_Head` (30 textures, 40 × 62 used of 48 × 64, PSMT8 + palette) in `WorldGui.bdp` |
| Bars | `SliderMask` (104 × 10 used: a white bar with a dark inner outline), tinted |
| King-mode block icon | `BlockIcon` (32 × 32) |
| Fonts | `ArialBlack24` (`CommonGUI.bdp`, every HUD text). `PointsBanner` (`WorldGui.bdp`, digits only, the floating numbers). `Atrox60` (`CommonGUI.bdp`, the RAMPAGE banner of `Rage.ifc`). `ArialRound24Numbers.fdf` is in `WorldGui.bdp` and draws the bitmap `ArialBlack24Numbers` |
| Floating score numbers | C++ class `PointsBanner` (vtable 0x44B6A0), font set by `gWorld.setBannerFont("PointsBanner")` in each city script |

## The reference screen
GUI coordinates are in a 640 × 480 screen with y pointing down. Full-screen items are written as `<p>320 240</p><d>640 480</d>` (or `639 480`)
(15 times across the layouts) **[R]**. The game maps this screen to the PS2 frame buffer. A viewer scales it to its window.

## Layout tree
The XML element names are listed in a table of 20-byte records at 0x3B3240: a string, then a value type (0x2B colour,
3 float, 2 string, 5 integer, 0xD vector, ...). The order of the records gives each tag its id. Objects:
`i` interface, `DD` the 2D root (`-.2D.-`), `o` group, `q` quad, `t` text, `sl` slider, `r` rect (GuiRect), `lb` list
box (GuiListBox), `X` child interface. Properties:

| Tag | Meaning |
|---|---|
| `n` (attribute) | name (`findObject`) |
| `p` | position x [y [z]] in the parent's frame **[R]** |
| `m` | scale of the object's frame: it multiplies the frame's 3×3 as it is when read (0x22DDF0), so it scales the children too **[R]**. Scripts call `setScale` |
| `z` | the object's fade (+0x5C, the handler 0x22C450 of `fade`): it multiplies the fade of the object and its children, and at 0.01 or less nothing of it is drawn (0x2418A4) **[R]**. The bars' black backing (0.5) is half faded over the bar's frame; Rage's `Head` (0) is never seen |
| `d` | size. When `d` is absent, a quad takes the used size of its texture |
| `b` | texture (bitmap) name |
| `c` | colour `0xRRGGBBAA`. It multiplies the texture **[R: Hud.scr's rageColor (1, 0.5, 0.25) = maskColor 0xFF8040FF]** |
| `Q` | quad sprite flags. 0x2 centres the sprite across its origin and 0x4 centres it vertically (0x243C00 → 0x17C720 / 0x17C6D0 / 0x17C770) **[R]**. Without them the sprite spans from the origin to +d |
| `F` | object flags (0x22B2F0: the u16 at +0x5A replaced). The draw skips `F & 0x201` (0x241770): 0x1 marks objects that start hidden (`heads`/`blocks`, `numText`, `line2`, `challenge_group`) **[R]**. Interfaces carry 0x82: 0x80 gives the interface the frame event even when it is not the focused one (0x2416E8) **[R]** |
| `v`, `L` | text: a literal string, or a `.loc` key (0x235800): one starting with a digit is the text itself; else the string tables (LocalizationManager 0x1D4C60: the tables from the last loaded back, each a case-blind binary search, 0x1D5320; an empty text tries the next); a missing key shows `0x key x0` (0x4225C8) **[R]**. `gApp.localizeString` of a missing key gives "" |
| `g` | font name (0x22D960: only the first letter of each attribute counts, either case; a fresh OFont each time) **[R]**. `j` and `o` share a parser (0x2350E0): `l`/`c`/`r` set the justify, `t`/`m`/`b` the origin (a longer value only as left, center, right, top, middle, bottom); `s` the scale (+0xC, applied in the font draw); `c` the char spacing, `l` the line spacing; `w` the word wrap width (+0x8); `i` the slant (+0x44, over the font's: the glyphs' top edge moves by it, an italic: 32 on the Pause, GameOver and Results titles, 21 CitySelect's, 4 MonsterSelect's); `m` (+0x7) how each line sits in the block: `l`, `c`, `r`, default 3 each line by `j` |
| `maxwidth` | the text shrinks uniformly when its width after `s` is greater (0x1B6270, 0x1B6350): drawn min(s × w, maxwidth) **[R]** |
| `val`, `hasMaskBar`, `barColor`, `maskColor` | slider properties |
| `M` | sprite flags ORed in on a quad (0x22B210 → 0x17D0B0, the shader picked again by 0x17D100). 0x200 picks a depth-only shader (descriptor 0x410D50 / 0x4114E8: GS TEST 0x52001, alpha test NEVER with ZB_ONLY): not drawn **[R]**. 0x10 / 0x100 are renderer states 0 / 1, depth test and write; 0x400 depth ALWAYS for that sprite. 0x310 on Results' two white boxes at the scrolling title's ends: they write depth 0.001 behind the title's glyphs, which test GEQUAL and are drawn over them: the title is not cut **[R: SCREENS_NOTES.md]**; 0x80 (on 0x90: CitySelect's `basic`, Pause's icons, ChallengeText's `icon1`) the additive shader, GS state 0x17, ALPHA 0x48 Cs × As + Cd **[R]** |
| `st`, `ct`, `bkgdColor`, `bc`, `B`, `bbias`, `R` | rect: side and corner pictures (`FrameLine`, `FrameCorner` of CommonGUI.bdp), background, frame colour, frame width, the background's bias (−8: 4 in), flags |
| `c` / `b` / `d` with `highlight`, `normal`, `disabled`, `selector`, `endcap`; `sp`, `a`, `growth` | list box: item colours (selected, others), the highlight bar's pictures (`HighlightSegment`, `HighlightEndcap`) and size, spacing, flags, its growth |

### Rect and list box
**GuiRect** [R] (vtable 0x447A00, ctor 0x245650, draw 0x244500, rebuild 0x2449F0): the outer box is `d` from the
origin (`B`, default 5, would add 2 ΔB, but every layout gives `d` after `B`). The background (sprite +0x80) starts at
−bbias / 2 with the size d + bbias (0x243840): with bbias −8, 4 in on each side. Four B × B corners at the outer
corners, inside the box, `FrameCorner` turned 0°, 270°, 180° and 90° (0x3B3EC0, 0x3B3E90, 0x3B3E60; the picture is
symmetric about its diagonal, so the turns are mirrors **[R]**); four GuiLines B wide between (left and right at x = B/2 from B to H − B, top and bottom at y = B/2; drawn
left, bottom, right, top). The order: the background, the rect's own text (none in any layout), two more lines
(+0x178), the sides, the corners. `FrameLine`'s opaque rows (9 .. 15) are the outside, where they meet the corner's
arc **[R: the textures]**. The rebuild's points (0x2449F0: eight sets of four mirrored points from B/2, B, 3B/2,
2B) run the left down, the bottom left to right, the right up, the top right to left, so each line's v 0 is its
inner edge (0x23A8E0: v 0 on the +perp side, perp = (d.y, −d.x)): FrameLine's opaque rows the outer **[R]**. Without
a corner picture the corners are not drawn (0x244500 tests the first's texture) and the left and right lines run the
whole height, 0 to H, closing the frame **[R]**. The +0x178 lines were not decoded **[G]**; the draw puts them before
the sides.

**The list box** **[R]** (ctor 0x23E280, draw 0x23C250, layout 0x23CF60; [PAUSE_NOTES.md](PAUSE_NOTES.md) "The list
box"): the selection without wrap, the item colours set every frame, the selected item's growth, and the highlight
(a GuiLine with the segment and end cap pictures) across the selected item's text, in the GUI's pulse.

**Word wrap** [R 0x1B5AA0]: a line ends once Σ(a + w + c + spacing) × s reaches `w` (the trailing spacing counted, no
slant): at the last break (a space, or just after a `-`), else before the character that overflows (one at least);
the next line skips the spaces. `w` is read in the text's own frame after `s` (boss_message's x 20 + w 480 = its
rect's 500; cq_title's 460 centred in 480; the Results lines' w = the list's width).

### Slider (GuiSlider, layout 0x24A310)
Defaults: value 100, range 0..100, size 100 × 20 (constructor 0x24ACD0) **[R]**. The slider is always centred on its origin.
Take f = (value − min) / (max − min). With `hasMaskBar` (flag 0x10), the bar sprite covers the full size in
`barColor`. The mask sprite runs from x = w (f − ½) to the right end in `maskColor` (UV f..1). Without it, the bar runs from
the left end to w (f − ½) (UV 0..f) **[R]**. Neither sprite is textured in the HUD.

### Quad, text and fade
- A quad (`GuiQuad::draw` 0x2439E0) draws one sprite through the object's world matrix (0x240790). Its alpha is
  multiplied by the fade **[R]**. It is drawn **clamped** at its texture's edges **[R]**: the draw's pass setup
  (0x2439E0 → 0x17BCA0 → 0x17A940 → 0x25FF30 → 0x265840) hands each pass's byte +0xF (a pass is 36 bytes from the
  shader's +0x4, its texture slot at +0x4) to the texture's 0x26AA20, which stores it at +0x40 and rebuilds the
  texture's CLAMP_1 (+0x90; register 8 at +0x98): WMS = WMT = 1 (CLAMP) when bit 0 is clear, 0 (REPEAT) when set,
  the region fields 0. A texture loads repeating (0x26AA80 / 0x26B0A0: +0x40 = 0x11); every GUI shader template
  (copied by 0x17D100: 0x410918 … 0x415940, 15 of them) has 0 in its passes' +0xF, so a picture's bilinear taps
  never reach its other edge. render_gl.c `draw_hud` clamps (GL_CLAMP_TO_EDGE) for each HUD quad and repeats again
  after it, the world's draws sharing the texture objects (up to v0.11.3 the pictures repeated: thin lines along the
  city cards' frame and across their pictures, the frost tiles' seams in MonsterSelect).
- A text (`GuiText::draw` 0x234D30) multiplies the alpha of its colour and of its shadow by the fade, then calls the
  font's string draw with the object's matrix **[R]**.

## Bitmap fonts (`.fdf`, type 2)
OFontData loader 0x1B7CD0, read **[R]**. Field-by-field layout is in `tools/gui.py`. Main points:

- The bitmap is found by name: the path and extension of the name at 0x100 are dropped.
- Glyph u/v values are fractions of the bitmap's used size. The loader multiplies them by the texture's own UV scale.
  - The floats at texture +0x18 and +0x1C are used ÷ the GS's power-of-two size (156 / 256 for PointsBanner).
  - Over a decoded image of the header's width × height, the used part ends at used ÷ size.
- Cell height (+0x24) = texture height ÷ rows at 0x304, the used height (0x26AA80 sets +0x20 / +0x24 from the entry's
  +0x14 / +0x16; 0x1B801C) **[R]**. ArialBlack24: 210 / 7 = 30, PointsBanner: 21.
- Average height (+0x28) = mean `h` of the digits and lower case, or of the capitals if there are none.
- The OFont defaults (0x1B7200) come from the file: spacing, line spacing, shadow dx and dy, shadow colour, colour
  (`AABBGGRR`), slant, scales.
- ArialBlack24: spacing 0, shadow 3 3 in 0x9B000000, colour white.

**Measuring a line** (0x1B5AA0):
- Width = sum of (a + w + c + spacing) − spacing + slant. A character without a glyph adds nothing.
- Height = the largest glyph `h`. An empty line uses the average height.

**Placing the text** (0x1B6A80 for the placement, 0x1B64B0 for the quads):
- Scale k = maxwidth ÷ width when the text is wider than maxwidth, else 1.
- The text's own `s` goes into its matrix.
- Origin: y starts at 0 (top), −H/2 (middle) or −H (bottom). Before each line, y += h·k − cell·k.
- Justify (OFont +0x7 = 3): each line starts at 0, −w·k/2 or −w·k.
- Per glyph: x += a·k, then a quad from x to x + w·k and from y to y + cell·k, then x += (w + c + spacing)·k.
- A space only advances.
- The shadow pass is drawn first at (dx, dy)·k in the shadow colour (0x1B6EB8).

## HUD logic (Hud.scr, Clock.scr) [R]
- **Setup:**
  - Both `heads` and `blocks` take the texture `Hud_<Monster>_Head`.
  - `heads` shows unless the play type is 3–4 (King of the City/World). In that case `blocks` shows, with its
    `block` icon tinted grey 0.4 when the block count to win is greater than 0.
  - `numText` (`x<lives>`) shows only in the campaign (play type 2).
  - Player 1 of 2 moves to 502 60.
- **Every frame** (`updateStats`):
  - Health slider = 100 · HP / max HP. The bar is bright green and the mask, dark green, covers what was lost.
  - Rage slider = 100 − 100 · energy / 100. Its bar is dark brown. The orange mask is the rage, growing from the right end.
  - `score` = `"%d"` of `player.getPoints(6)`. It has no thousands separator. It is right-justified on the bottom at
    group + (86, −4), drawn at 0.8, and squeezed above maxwidth 100 after the scale (125 font pixels).
  - At full energy, the mask colour flashes: lerp(rage, white, (1 + cos(30 t)) / 2), where t sums the frame times.
  - The bars do not ease; they jump to the value.
- **Clock:**
  - mm:ss at 320 40, from five centred ArialBlack24 texts. Minutes are blank under 60 s, the tens of minutes under
    10 min, and the time is capped at 59:59.
  - When `gWorld.isGameClockVisible()` changes, the fade drops to 0. The clock then fades in at 1.5 per second.

## Floating score numbers (PointsBanner) [R]
- **Start** (0x331D50): text `sprintf("%d", min(amount, 99999999))`.
- **Who creates them:**
  - The building scorer (0x2BAAD0) uses colour 0xFF00F0F0 (yellow, `AABBGGRR`) for a whole building and 0xFFF0F0F0
    otherwise. Scale 0.8. It places the number at the award point less 5 in y.
  - `Edible.scr` uses scale 1.0.
- **Motion** (0x331B00), with a = t / 1.4:
  - It rises by −2 + 12 (2a − a³) in z.
  - It shakes by 1.5 (1 − a)² · sin(1.4 + 16πt) in x and · sin(2.7 + 20πt) in y.
  - It is removed after 1.4 s.
- **Drawing** (0x331960):
  - A billboard facing the camera (the camera matrix with y turned down) at that point.
  - Each font pixel is s = max(0.01, 0.75 · scale · a − a²) world units (0.75 at 0x44CA48). The number grows to
    0.09 at t = 0.42 s (scale 0.8) and shrinks to nothing by 0.84 s.
  - The string is centred both ways, with a shadow of 3 3 font pixels in 0xC4000000 (font setup 0x2B8510).
  - PointsBanner spacing is 2.

## Not traced
- `F` 0x20 (on the icon quads): no code tests it (no +0x5A & 0x20 in the ELF); what the game draws for a quad whose
  picture is missing (hud.c draws nothing). `F` 0x1 is hidden and 0x4 is never tested **[R: SHELL_NOTES.md "The GUI's
  3D objects"]**.
- The GUI draws each interface in tree order, the object, then its children, without a sort **[R 0x241770]**.
- Shift-JIS glyphs. (The special glyph images, the `.loc` texts' `\v1.25Controller_Back\v` pictures, are read:
  0x1B7AE0, 0x1B74A0, [SCREENS_NOTES.md](SCREENS_NOTES.md) "Pictures in a text".)
- The mapping of the 640 × 480 screen to the PAL/NTSC frame buffer.
