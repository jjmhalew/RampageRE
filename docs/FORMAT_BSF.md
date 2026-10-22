# .bsf – compiled scripts

Most of Rampage's game logic is not in the ELF: it is in 138 scripts written in a small C-like language. Every
object type (`Boss_Tank`, `Crane`, `ElevatedTrain`, `Edible`), every monster (`George`, `Lizzie`, ...), every city
(`Chicago`, `HongKong`, ...) and every menu screen (`MainMenu`, `MonsterSelect`, `Pause`, `GameOver`) has one.
They ship in `RAMPAGE.VOL` as `.bsf` files, the game's "Compiled PPTokenStream": the token stream of the
*preprocessed* source (`#include` already expanded), with comments and spacing dropped but every token still on
its own source line. The game parses and compiles them itself when a level or screen loads.

`tools/bsf.py` turns them back into source and compiles source back into `.bsf`:

```
python tools/bsf.py --all build/vol build/scripts     # 138 .scr files + 5 headers in build/scripts/include
python tools/bsf.py --verify build/vol                # 138/138 scripts rebuilt byte for byte
python tools/bsf.py build/vol/GameOver.bsf            # print one script (and its headers)
```

`--verify` decompiles every script to text, lexes that text again, writes a new `.bsf` and compares it with the
original: all 138 are identical, so the decompiled source loses nothing that is in the file (token, value, file
and line of every token). The output has 38,509 lines. Comments and the original spacing and indentation are not
in the file; indentation is regenerated from the braces.

## In the game
| Address | |
|---|---|
| 0x1D4870 | opens `<name>.bsf` through the file system (0x10C220) and hands it to the parser |
| 0x1C3F90 | checks the magic (`strncmp`, 0x25 bytes) and the size field |
| 0x1C6220 | parser/compiler; 0x1C40A0 is the grammar driver it calls |
| 0x419358 | token name table, 8 bytes per entry (`"=="`, `"!="`, `"*"`, ...); the token code is the index |
| 0x2B02D0 | example of a native class's method lookup (`World`): `strcmp` against each method name and return a 16-byte descriptor `{method index, 0, slot, ...}` at 0x42AF30+ |
| 0x1BB620 | `POperatorNode` (vtable 0x445660) +0x30, an operator's evaluation: a jump table (0x4186C0) on its kind (+0x10). Kind 5 is `&&` / `\|\|` (token +0x1C, 0x0B or 0x0C) and short-circuits as in C (0x1BB7F0): the left operand (+0x20) is evaluated and tested, and the right (+0x24) only when it decides the result, so a `gWorld.frand()` on the right of `a && b` is not drawn when `a` is false, nor of `a \|\| b` when `a` is true |
| 0x1B9630 | `PFunctionCallNode` (vtable 0x445560) +0x30, a call's evaluation: its arguments from the list at +0x18, first to last, each into the next slot of the argument array (slot 0 the first parameter), then the callee's expression (+0x20), then the call. So the arguments are evaluated left to right, and `p.set(frand(), frand(), 0)` draws its x before its y |

## File layout (little endian)
| Offset | Type | |
|---|---|---|
| 0x00 | char[0x25] | `"Compiled PPTokenStream version 1.03.\x1a"`, zero padded to 0x28 |
| 0x28 | u32 | file size |
| 0x2C | u32 | number of tokens (line and file markers are not tokens) |
| 0x30 | u32 | offset of the file name table |
| 0x34 | | token stream, ended by `0xFD` |
| | | string pool: the text of every identifier and string literal once, NUL-terminated, in order of first use |
| names | | source file names, NUL-terminated, in order of first use (`Common.h`, ..., `ActorSwarm.scr`) |
| | | zero padding to a multiple of 4 bytes (included in the size field) |

### Tokens
Each token is one byte: its index in the token name table.

| Code | Token | Code | Token | Code | Token | Code | Token |
|---|---|---|---|---|---|---|---|
| 0x00 | `==` | 0x10 | `<` | 0x20 | `::` | 0x30 | `break` |
| 0x01 | `!=` | 0x11 | `>` | 0x21 | `->` | 0x31 | `enum` |
| 0x02 | `*` | 0x12 | `>=` | 0x22 | `++` | 0x32 | `continue` |
| 0x03 | `+` | 0x13 | `<=` | 0x23 | `--` | 0x33 | `extern` |
| 0x04 | `-` | 0x14 | `=` | 0x24 | `/**` | 0x34 | `namespace` |
| 0x05 | `/` | 0x15 | `(` | 0x25 | `**/` | 0x35 | `forceref` |
| 0x06 | `%` | 0x16 | `)` | 0x26 | STRING + u32 text offset | 0x36 | `const` |
| 0x07 | `&` | 0x17 | `{` | 0x27 | NUMBER + f32 | 0x37 | `while` |
| 0x08 | `~` | 0x18 | `}` | 0x28 | IDENTIFIER + u32 text offset | 0x38 | `do` |
| 0x09 | `\|` | 0x19 | `[` | 0x29 | `#` | 0x39 | `struct` |
| 0x0A | `^` | 0x1A | `]` | 0x2A | `##` | 0x3A | `public` |
| 0x0B | `&&` | 0x1B | `,` | 0x2B | (space) | 0x45 | INT + u32 |
| 0x0C | `\|\|` | 0x1C | `;` | 0x2C | `for` | 0xFD | end of stream |
| 0x0D | `!` | 0x1D | `.` | 0x2D | `if` | 0xFE n | line marker |
| 0x0E | `<<` | 0x1E | `?` | 0x2E | `else` | 0xFF n | file marker |
| 0x0F | `>>` | 0x1F | `:` | 0x2F | `return` | | |

0x3B–0x44 are names the compiler uses internally (`<Unknown Character>`, `<ERROR>`, `<End Token>`,
`<Unary Minus>`, `Pre ++`, `Pre --`, `Post ++`, `Post --`, `<Type declaration>`, `<Array reference>`) and do not
occur in files. Type names (`int`, `float`, `string`, `void`, `Point3`, ...) are plain identifiers. Negative
numbers are a `-` token followed by a positive literal.

### Line and file markers
Markers come *after* the token they place:
- `0xFF n`: this token is in file `n` (index into the name table). The stream starts in file 0.
- `0xFE n`: this token is `n` lines below the previous token of the same file.
Every file starts at line 1. A token without markers is on the same line, in the same file, as the token before
it. When the stream comes back to a file after an `#include` (for example `GameOver.scr` includes
`ConfirmQuit.h` in the middle), its lines continue from that file's last token. `bsf.py` puts the `#include`
line back on the first free line after the last token before the switch.

## The script language
C-like, statically typed, with value-type structs and engine classes:
- Types: `int`, `float`, `string`, `void`, structs, engine classes (`Point3`, `Color`, `GuiObject`, `Actor`,
  `Monster`, `Building`, `LineMover`, `FollowEffect`, ...), fixed arrays `int[6]` and growable arrays `string[]`
  (`names.increment()`, `names.size()`). Casts are C style: `(int) (gWorld.frand() * 8.0)`.
- Statements: `if`/`else` (3,977 `if`s), `for` (262), `break`, `continue`, `return`. The shipped scripts use no
  `while`, `do`, `switch`, `?:`, `enum` or `const`, although the compiler knows the keywords.
- `::` (10 uses) qualifies a name with its namespace (`void Effect::delayedStop(...)`, `Monsters::startRage(...)`); scripts also call each other by name in strings, for example
  `gWorld.installScriptFunction(0, "Amanda::onInit")` or `gGuiMgr.iCallScriptS("ControlRoom::showInterface", ...)`.
- Strings concatenate with `+` (`"Actor" + i`).
- Object scripts declare `struct Def { <engine class> object; ... }`, `string GetTypeName()`,
  `void Construct(Def this, WorldObject wob)` and event handlers that take `Def this`. Most used handlers:
  `onInit` (73 scripts), `onPulse` (55, timer set with `setPulseDelay`), `onLoadScript` (42), `onMoverCallback`
  (41), `onShutdown` (40), `onPickup`, `onThrow`, `onBuildingDestruction`, `onAnimComplete`, `onBeginFrame`.
  `forceref` (27 uses) probably keeps a function that is only called by name (`forceref int setState(Def this, int newState, ...)`).
- Screens are `namespace MainMenu { ... }` with a GUI event function (`doInterface(GuiObject obj, GuiEvent
  event)`), keyed on `event.getId()` (8192 = screen opened, 2048 = tick with `event.getFloat()` as the time
  step, 8/4 and 16/2 = up and down, 128/64 = confirm).
- Engine globals: `gWorld` (967 uses), `gCommon` (408), `gSndDev` (99), `gGuiMgr` (96), `gShell` (70),
  `gApp` (61), `gAIMgr` (25), `gCameraMgr` (21), `gUpgrades` (5).
- Headers: `Common.h` (`nVerbose`, `fmin`/`fmax`/`fabs`/`min`/`max`/`abs`), `GrowShrink.h`, `ObjectFader.h`,
  `ButtonSetup.h`, `ConfirmQuit.h`. Some scripts contain only part of `Common.h` (10 of its 78 lines; probably
  `#ifdef`'d by the includer), so `--all` keeps the fullest copy of each header in `include/`.

## Next
- Catalogue the engine API the scripts call (per class: method name → method index → native function), from the
  lookup functions like 0x2B02D0 and the dispatch behind them.
- A script compiler and VM in the native engine that runs these sources, checked against the recompiled game.
