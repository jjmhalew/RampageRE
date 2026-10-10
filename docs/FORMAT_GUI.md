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
| `m` | scale of the object's frame. It also scales the children **[R/G: the health and rage frames (104 and 90.25 wide at m 0.8) both end at x 136.2; a 100-wide slider fits inside the 104-wide frame only when scaled]**. Scripts call `setScale` |
| `z` | the object's fade (+0x5C, the handler 0x22C450 of `fade`): it multiplies the fade of the object and its children, and at 0.01 or less nothing of it is drawn (0x2418A4) **[R]**. The bars' black backing (0.5) is half faded over the bar's frame; Rage's `Head` (0) is never seen |
| `d` | size. When `d` is absent, a quad takes the used size of its texture |
| `b` | texture (bitmap) name |
| `c` | colour `0xRRGGBBAA`. It multiplies the texture **[R: Hud.scr's rageColor (1, 0.5, 0.25) = maskColor 0xFF8040FF]** |
| `Q` | quad sprite flags. 0x2 centres the sprite across its origin and 0x4 centres it vertically (0x243C00 → 0x17C720 / 0x17C6D0 / 0x17C770) **[R]**. Without them the sprite spans from the origin to +d |
| `F` | object flags. 0x1 marks objects that start hidden (`heads`/`blocks`, `numText`, `line2`, `challenge_group`) **[G]**. Interfaces carry 0x82 |
| `v`, `L` | text: a literal string, or a `.loc` key (looked up in the bundles' string tables, `tools/loc.py`; a missing key shows itself **[G]**) |
| `g` | font name, with attributes `j` (justify: `c` centre, `r` right, else left), `o` (origin: `m` middle, `b` bottom, else top), `s` (scale), `charspacing`, `linespacing`, `w` (word wrap width), `m`, `i` (not traced) **[G: the letter meanings; R: the three modes in the string draw]** |
| `maxwidth` | the text shrinks uniformly when its unscaled width is greater (0x1B6350) **[R]** |
| `val`, `hasMaskBar`, `barColor`, `maskColor` | slider properties |
| `M` | 0x310 on Results' two white boxes at the scrolling title's ends: taken as masks that cut the title to x 62..503, not drawn **[G]**; 0x90 on a visible icon |
| `st`, `ct`, `bkgdColor`, `bc`, `B`, `bbias`, `R` | rect: side and corner pictures (`FrameLine`, `FrameCorner` of CommonGUI.bdp), background, frame colour, frame width, its offset (−7: outside), flags |
| `c` / `b` / `d` with `highlight`, `normal`, `disabled`, `selector`, `endcap`; `sp`, `a`, `growth` | list box: item colours (selected, others), the highlight bar's pictures (`HighlightSegment`, `HighlightEndcap`) and size, spacing, flags, its growth |

### Rect and list box [G]
Neither draw is traced. `hud.c` fills the rect's `d` from its origin in `bkgdColor`, then draws the frame `B` wide
with its outer edge `bbias` from the rect's: `FrameCorner` (the top-left corner) mirrored to the four corners,
`FrameLine` (its lower rows are the outside, matching the corner's arc) stretched between. A list box gives its
selected item the highlight colour and the others the normal one, and draws the highlight bar (segment + end caps,
`d highlight` tall, pulsing) behind the selected item's text. Word wrap: the text breaks at the last space that fits
`w`, read in the text's parent frame (boss_message's x 20 + w 480 = its rect's 500; cq_title's 460 centred in 480; the
Results lines' w = the list's width).

### Slider (GuiSlider, layout 0x24A310)
Defaults: value 100, range 0..100, size 100 × 20 (constructor 0x24ACD0) **[R]**. The slider is always centred on its origin.
Take f = (value − min) / (max − min). With `hasMaskBar` (flag 0x10), the bar sprite covers the full size in
`barColor`. The mask sprite runs from x = w (f − ½) to the right end in `maskColor` (UV f..1). Without it, the bar runs from
the left end to w (f − ½) (UV 0..f) **[R]**. Neither sprite is textured in the HUD.

### Quad, text and fade
- A quad (`GuiQuad::draw` 0x2439E0) draws one sprite through the object's world matrix (0x240790). Its alpha is
  multiplied by the fade **[R]**.
- A text (`GuiText::draw` 0x234D30) multiplies the alpha of its colour and of its shadow by the fade, then calls the
  font's string draw with the object's matrix **[R]**.

## Bitmap fonts (`.fdf`, type 2)
OFontData loader 0x1B7CD0, read **[R]**. Field-by-field layout is in `tools/gui.py`. Main points:

- The bitmap is found by name: the path and extension of the name at 0x100 are dropped.
- Glyph u/v values are fractions of the bitmap's used size. The loader multiplies them by the texture's own UV scale.
  - The floats at texture +0x18 and +0x1C are used ÷ the GS's power-of-two size (156 / 256 for PointsBanner).
  - Over a decoded image of the header's width × height, the used part ends at used ÷ size.
- Cell height (+0x24) = texture height ÷ rows at 0x304. ArialBlack24: 210 / 7 = 30, PointsBanner: 21 **[G: the used height]**.
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
    group + (86, −4), drawn at 0.8, and squeezed above 100 font pixels (6 digits and more).
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
- The letters of `j`/`o`, and `F` 0x20 (an icon quad: drawn only with a picture). `F` 0x1 is hidden and 0x4 is never
  tested **[R: SHELL_NOTES.md "The GUI's 3D objects"]**.
- The GUI draws each interface in tree order, the object, then its children, without a sort **[R 0x241770]**.
- The game's word wrap (OFont +0x8), the slant (0 in every font), Shift-JIS glyphs and the special glyph images
  (OFont +0x25C: the `.loc` texts' `\v1.25Controller_Back\v` pictures, drawn here as a square the cell's height times
  the number).
- The mapping of the 640 × 480 screen to the PAL/NTSC frame buffer.
