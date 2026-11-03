# Building new Echoes rooms in Prime World Editor

The goal: make new rooms for Metroid Prime 2: Echoes in this editor (geometry, collision, lighting and
scripting) that the native port ([MetroidPrime2Port](https://github.com/edboi18-ctrl/MetroidPrime2Port))
loads as it loads retail rooms. Today PWE edits a room's scripting, but when it saves a room
(`CAreaCooker::CookMREA`) it copies every other section unchanged from the original file, so a room's
shape cannot change.

The reference for every format below is the game's own reader, as decompiled in the port. When this
document and that code disagree, the code is right.

## What a room file (MREA, version 0x19) has to contain

From `CGameArea::PostConstructArea` (port `src/MetroidPrime/CGameArea.cpp`), which walks the sections in
order:

| Section | What the game does with it | Smallest valid content | PWE today |
|---|---|---|---|
| Header | transform, world model count, layer count, section numbers | required | writes it |
| World materials | the material set every world model uses | required | loads; `CMaterialCooker::WriteCookedMatSet` has an Echoes path |
| World models, per model: header (visor flags, transform, bounds), positions, normals, colours, UVs, packed UVs, surface offsets, the surfaces, two surface tables | `FillInStaticGeometry`, `CMetroidModelInstance` | at least one model | loads them and re-splits them for display; **no writer** |
| Render octree | built only if the header's index is not -1 | omitted (-1) | - |
| Area surface list (`SAreaSurface`: bounds, model, surface group) and ambient light IDs | `FillInStaticGeometry` | required | skipped on load; **no writer** |
| Script layers, generated objects (SCGN) | the room's script objects | required | writes them |
| Collision | `CAreaOctTree::MakeFromMemory` | required | **writer added** (`CCollisionCooker`, below) |
| BSP tree | `CAreaBspTree` reads 8 bytes and keeps nothing | 8 bytes | copies |
| Lights | `CWorldLight` list (`0xBABEDEAD` for two layers); zero lights is valid | a count of 0 | loads; **no writer** |
| Visibility (PVS) | used only if the section is over 64 bytes and starts `VISI` | empty | copies |
| PATH, PTLA, EGMC asset IDs | each is loaded only if it is not `0xFFFFFFFF` | `0xFFFFFFFF` | copies |
| Dependencies, modules | resources and REL modules the room needs | required | writes them |

A first custom room therefore needs: world materials, one or more world models with their surface
tables, the area surface list, collision, an 8-byte BSP section, an empty light list, and the script
layers PWE already writes. Pathfinding (PATH) can come later. Without it, enemies that walk or fly by
path have nowhere to go.

## Collision (done)

`src/Core/Resource/Cooker/CCollisionCooker.*` builds and writes Echoes area collision:

- `BuildIndexData` turns triangles into the format's shared vertices and edges, each triangle's edge
  triple, per-vertex/edge/triangle materials and triangle neighbours.
- `WriteOctreeCollision` / `WriteAreaCollisionSection` write the octree (leaves with their own bounds,
  branches split into octants exactly as the game's `BoxFromIndex` does) and the arrays after it.

What the game requires, and where each rule comes from (port source):

- **Triangles are two edges plus a flag.** `CCollisionPrimitiveData::GetTriangle` takes corners
  (e0.v1, e0.v2, e1.v2), or (e0.v2, e0.v1, e1.v1) when the material has `kFlippedTriangle`
  (`0x01000000`). Edges are shared, so the cooker starts each triangle at an edge whose successor is
  stored the same way, and gives it the flipped variant of its material when both run backwards.
- **Edge slots follow the corners.** `CCollisionTracker`'s `FindExitEdge` numbers a triangle's edges
  v0-v1, v1-v2, v2-v0 and `FindSharedEdge` looks them up in the edge triple, so slot *i* joins corners
  *i* and *i*+1.
- **The Echoes-only index array is triangle neighbours** (`GetTriangleNeighbors`, three per triangle,
  `0xFFFF` for none; read by `CCollisionTracker`, which the Ing use to crawl over surfaces). PWE used to
  skip it as unknown; it now loads it as `SCollisionIndexData::TriangleNeighbors`.
- **Vertex and edge materials never get the flipped bit**: bit 24 is `kFlippedTriangle` on a triangle
  but `kMT_NoEdgeCollision` on an edge (`CMaterialList.hpp`).
- **Room triangles need no `Solid` bit**: the collider tests octree leaves as `CMaterialList(kMT_Solid)`
  (`CGameCollision.cpp`). The flags a file needs are the surface type (stone, metal ...) and
  floor/wall/ceiling (bits 31/30/29).
- **Section preamble**: `PostConstructArea` skips the section's first byte, aligns to a word, and reads
  the block's size.

- **Octree size**: leaves of up to 128 triangles, at most 6 levels (`SCollisionOctreeSettings`). The
  format always halves all three axes, so on a wide, flat room small leaves multiply: 64 per leaf gave
  a 40 x 40 terrain 3,088 leaves (337 KB), 128 gives 512 (174 KB). These defaults are a guess at a
  balance, not Retro's numbers; compare them with retail rooms (below).

Tests: `tests/CollisionCookerTests.cpp` (`ctest`) cooks a closed box room and a 3,200-triangle terrain,
then reads the bytes back three ways: a transcription of the game's reader (tree walk, `GetTriangle`,
the arrays ending exactly at the block's end), PWE's own loader, and the tracker's edge/neighbour rules.
Every triangle must come back with its winding, in a leaf whose bounds contain it, and cooking what was
loaded must give the same bytes. `pwe_collision_cooker_tests --dump <dir>` writes the cooked sections out;
read with the port's independent Python reader (`tools/mcp/workshop.py` `parse_area_collision`), both
pass every check, and the terrain's 160 edges without a neighbour are exactly its outer border.
**Not yet checked against the running game or against retail data.**

To check the rules on retail data, the port's MCP server has `echoes_disc_room_collision`: it reads a
room off your disc and reports whether Retro's collision follows them (edge slots, neighbours, where the
flipped bit sits) along with its octree's shape and material flags (port `docs/MCP_WORKSHOP.md`).

## Plan

1. **Collision writer**: done, above.
2. **World geometry writer**: world materials, models and their surfaces (GX display lists, which
   `CModelCooker` already writes for Echoes CMDLs), the two per-model surface tables and the area
   surface list (both read by `FillInStaticGeometry` and `CMetroidModelInstance`; some fields are still
   unnamed in the decompilation, so the renderer that reads them is checked first), lights, and the
   stub sections.
3. **Replace a room's shape from a model file**: Assimp import (already in the Model Editor) gives world
   models and, from the same triangles, collision, with floor/wall/ceiling picked from each triangle's
   normal and a surface type the user chooses. Textures need a TXTR writer (`CTextureEncoder` only turns
   DXT1 into CMPR today). The room keeps its asset ID, so the port loads it as a loose mod file, the same
   way it loads any replaced resource.
4. **New rooms in a world**: add an area to an MLVL (transform, bounds, docks and their portal corners,
   modules, layers, map), and the doors that join it to its neighbours. On the port side, loose
   resources with new asset IDs (today the port only replaces IDs a PAK already holds).
5. **Check against real rooms** (with the user's disc): load every retail room's collision, cook it
   again and compare the triangles; then put recooked rooms in the port's mods folder and walk them
   with the port's area sweep.
6. **Less of the disc**: as rooms, models, textures and sounds become the project's own, the port can
   load them from a content folder; what is still Retro's keeps coming from the user's own disc at run
   time. A game that is passed around has to carry only its own assets.

## Building notes

- The editor uses C++23's explicit object parameters (`this auto& self`): GCC 14 or newer, Clang 18 or
  newer, or a current MSVC. GCC 13 fails in `IProperty.h`. Two `std::vector::append_range` calls (a
  C++23 library function GCC 14's library lacks) are now plain `insert`s.
- The GUI needs **Qt 6.7 or newer** (not 6.5): it calls the `QMetaObject::invokeMethod` overloads that
  take a member function and its arguments, which Qt 6.7 introduced. With Ubuntu 24.04's Qt 6.4 the core
  library and the tests build, the editor does not.
- `pwe_core` and the tests need no Qt at all: on Linux, OpenGL and GLU headers (`libgl-dev`,
  `libglu1-mesa-dev`).
- A clone without tags (a shallow clone, or a fork made without tags) used to stop CMake with
  `VERSION "<hash>" format invalid`; the editor's version now falls back to 0.0.0 there.
