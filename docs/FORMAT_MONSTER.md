# MONSTER_DATA: the monster's move graph

Every monster bundle (`<Monster>.cmp`) has a raw entry (type 2) `MONSTER_DATA`, 231 KB for George: how the monster
moves, what each pad input does in each state, its collision shapes, attachment nodes and effects. It is the
compiled form of the monster's move graph: states (each with a clip), transitions between them guarded by
conditions (pad inputs, analog values) and optional script callbacks (`Monsters::canMount`, `Monsters::hasRoarEnergy`
in `Monsters.bsf`, plus per-monster ones such as `George::doGrabPowerup`). `tools/monster.py` parses it
(`--verify`: all 30 monsters, every table where the previous one ends) and prints the graph.

## Loading
0x2F3A50 finds the entry by name, refuses any version but 11 and calls 0x2F3C10, which fixes it up in place:
name indices (into the bundle's name table, which holds every clip, state, callback, sound and effect name) become
string pointers, offsets become pointers. State nodes are found by name through a hash table (hash % 97, 0x2E8DB0).

## Layout
Offsets from the start of the entry. The tables follow each other from 0xAA8 in this order (George's offsets).

| Header | Table | George | |
|---|---|---|---|
| 0x000 | | 11 | version |
| 0x004..0x128 | | 74 × i32 | event slots: the start list (+0xA5C) of each event, the defaults for every mode |
| 0x12C..0x9C0 | | | not decoded |
| 0x9C4..0xA2C | | | name indices (−1 = none), settings (f32 0x9F4..0xA10) |
| 0xA30, 0xA34, 0xA38 | | | names of the skeleton, the mesh and the cloth model |
| 0xA44 / 0xA48 | collision shapes | 11 × 24 at 0xAA8 | f32[3], f32[2], u32 bone \| part << 16 |
| 0xA4C / 0xA50 | state nodes | 186 × 0x84 at 0xBB0 | +0x78 clip name, +0x7C state name, the rest settings |
| 0xA60 / 0xA68 | coded item offsets | 1,325 × u16 at 0x6B98 | into the block at +0xA64 |
| 0xA64 | coded items | at 0x75F4 | u32 type first (21, 6, 7, 23, 15, 0, 12, 8, 9, 16); types 6 and 21 carry names |
| 0xA6C / 0xA70 / 0xA74 | body parts | 6 at 0xF678, pointers at 0xF748 | `Arm L`, `Arm R`, `Body`, `Head`, `Leg L`, `Leg R` with their bones |
| 0xA58 / 0xA5C | start lists | 29 × 12 at 0xF760 | u32 ?, offset of a state, u32 ? |
| 0xA94 / 0xA98 | modes | 6 × 0x128 at 0xF8BC | 74 slots each overriding the defaults (−1 = keep) |
| 0xA78 / 0xA7C / 0xA80 | | 2 at 0xFFAC, pointers at 0x1022C | |
| 0xA84 / 0xA88 | attachment nodes | 15 × 0x38 at 0x10234 | f32[12] matrix, name (`fxNode_Belch`, `Helper L Hand`, `Tracking_Node`, ...), bone \| index << 16 |
| 0xA8C / 0xA90 | effect names | 12 at 0x1057C | `Monster_Belch.edf`, `Debris_mega_punch.edf`, ... |
| 0xA54 | the move graph | header 0x34 bytes at 0x105AC | +0x2C state count, +0x30 offset of the first state |
| | states | 176 from 0x105E0 | see below |
| | | states² bytes at the end | a state × state byte matrix (30,976 = 176² for George; 177² for Marco, 179² for Rojo) |
| 0xAA4 | | | name of the monster's property book (`George_prop.prb`: hand and foot bones, foot IK, effects, weapons) |

A state: 0x2C bytes, then 0x74 bytes per transition.

| State | | Transition | |
|---|---|---|---|
| +0x00 | index | +0x04 | offset of the target state |
| +0x04 | offset of its state node | +0x08 | flags (0x18, 0x1018, 0x218, ...; 0x100 and 0x200 set at run time) |
| +0x08 | f32 clip length (s) | +0x0C, +0x10 | f32 window A |
| +0x0E | i16 transition count | +0x14, +0x18 | f32 window B |
| +0x14 | offset | +0x1E | i16 condition count |
| +0x18 | u8[16] | +0x20 | 4 × 16-byte conditions {u32 kind, u32 id, u32 value, u32 ?} |
| +0x28 | offset of the transitions | +0x60 | callback name (−1 = none) |

Conditions are of kind 4 (1,280 in George: ids 10, 14, 15, 16, 17, 31, 48, 49, 50, 53, 56, 57, values 0 or 1),
kind 0 (164: ids 0, 31, 32, 40, 57, values 0 or a float, 0.8) and kind 3 (67, id 31). The last word looks like a
negation. From the graph, kind 4 id 14 starts a jump, 15 a grab, 16 a punch (with 48 / 49 up and down), 17 a kick,
10 a roar; kind 0 id 31 moves from idle to walk. These meanings are read off the graph, not yet from the evaluator.

All 30 monsters share the graph's shape (176 states and 1,085 transitions; Marco 177, Rojo 179) and differ in
clips, settings and coded items.

## At run time
0x2EBFE0 runs a monster's current state each frame: the state's time advances by dt × the monster's speed
(+0xA8C of the monster object), each transition gets flag 0x100 while the time is inside window A and 0x200 inside
window B, then 0x2EC3D0 tests the conditions (not decoded yet) and a transition that holds moves the monster to its
target state.
