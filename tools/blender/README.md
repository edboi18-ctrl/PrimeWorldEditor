# Echoes Room Tools (Blender add-on)

Build Metroid Prime 2: Echoes rooms in Blender for the native port
([MetroidPrime2Port](https://github.com/edboi18-ctrl/MetroidPrime2Port)): model the room, give each material
an Echoes look (a built-in template, or a material read off your own disc), mark what is solid, and
**Build**. The add-on cooks the room (its MREA, with new world geometry and collision) and any new textures
straight into the port's mods folder.

Nothing from the game is included: materials and textures from the disc are read from your own image when
you use them.

## Install

1. Make the zip: `py -3 tools\blender\make_addon_zip.py` (or zip the `io_echoes_room` folder yourself).
2. Blender (4.2 or newer; tested with 5.2.3): Edit > Preferences > Add-ons > the drop-down at the top right >
   **Install from Disk...** > pick `io_echoes_room.zip`, and tick **Echoes Room Tools**.
3. In the add-on's preferences set:
   - **Disc image**: your Echoes `.iso`, `.gcm` or `.ciso` (convert RVZ/WIA/GCZ to ISO in Dolphin: right-click
     the game > Convert File...).
   - **Port mods folder**: on Windows, the `user\mods` folder beside `metroid_prime2_port.exe`.

## Making a room

The add-on replaces an existing room: the new room keeps its asset ID, its doors and its script objects,
so it plugs into the world exactly where the old one was.

1. **3D Viewport > Sidebar (N) > Echoes**: pick the **World** and the **Room** to replace.
2. **Import Room as Reference** brings in that room's world geometry, UV maps and materials (with their
   textures decoded off the disc) into a collection of its own. Use it to line your room up with the doors.
   References are not built unless you tick **Include in build** on the object (so you can also start from
   the retail room and change it).
3. Model your room. Blender's units and axes are the game's: Z is up, one unit is a metre, and faces point
   the way Blender shows (counter-clockwise from the front). Mirrored objects are fine.
4. Give every material an Echoes look in **Material Properties > Echoes Material** (or **New Echoes
   Material** in the sidebar):
   - **Template**: a built-in material. Pick an image for its texture (sides multiples of 4; powers of two
     are safest).

     | Template | Looks like |
     |---|---|
     | Lit | the texture, lit by the room's ambient and lights |
     | Lightmapped | the texture times a baked lightmap plus the room's lights, as retail rooms are built |
     | Cutout | Lit, with holes where the texture's alpha is under 64 (fences, foliage) |
     | Unlit | the texture as is (screens, skies, signs) |
     | Additive | the texture added over what is behind it (glows, light shafts); does not write depth |

     Lightmapped needs a second UV map named **Lightmap** (UV > Lightmap Pack) and a lightmap image: bake one
     in Cycles (Render Properties > Bake, type Diffuse or Combined, into an image on the Lightmap UV map).
   - **From disc**: a retail material, made by Import Room as Reference: assign it to your faces like any
     material. It keeps Retro's look (layers, glows, reflections); its texture roles are listed. A retail
     lightmap was baked for the old room, so **Replace a Texture** on the lightmap slot with your own bake.
   - **Collision**: whether faces with this material are solid, what they are made of (stone, metal, grass,
     ...; footsteps and effects), and what passes through them. Floor, wall or ceiling comes from each
     face's slope.
5. **Object Properties > Echoes**: an object can be drawn and solid (default), drawn only (decoration), or
   collision only (invisible walls, simpler collision for detailed meshes). Hidden objects are left out.
6. Name the **Mod** and press **Build Room into Mods Folder**. It writes
   `<mods>\<mod>\<room ID>.MREA` and, for each new image, `<mods>\<mod>\<the room's PAK>\<new ID>.TXTR`
   (the port adds those to that PAK). The panel shows what was written and any warnings, such as geometry
   outside the room's bounds in its world (the game culls by them).
7. Start the port in that room (the MCP server's `echoes_game_launch`), or press Reload mods in F1 > Mods.

**Clear pathfinding** drops the room's old PATH: enemies that walk by path then stay where they are, which
is better than walking through your walls.

## What is checked, and what is not

- `tests/test_core.py` (no Blender): textures, materials, geometry read back with octree queries, collision
  against the game's triangle rules, splicing into a room, a whole build.
- `tests/test_blender.py` (inside Blender): enable, import a room, model with template, lightmapped and disc
  materials, build, read back. The built room is also read by Prime World Editor's C++ transcription of the
  game's readers and its own material and collision loaders (`pwe_world_geometry_cooker_tests --check-mrea`).
- `ctest` in a Prime World Editor build runs all of these when Python has numpy (and Blender's `bpy`, or
  `PWE_BLENDER_EXECUTABLE` is set).
- **Not yet checked in the running game, or against retail rooms.** The geometry and collision layouts and
  the Lightmapped template follow the game's code and the port's scan of retail materials, but the first real
  test is yours. The port's MCP tools `echoes_disc_room_geometry` and `echoes_disc_room_collision` read
  retail rooms and report whether they follow the same rules.
- **The World Editor:** the materials are standard Echoes materials and load through Prime World Editor's own
  material loader (tested), so the editor can show them. It shows rooms from its project, though, and the add-on
  writes to the port's mods folder; putting a built room into a PWE project is the next step.

## Limits for now

- It replaces a room's geometry and collision; it does not add new rooms to a world or new doors.
- New textures are written as RGBA8 (uncompressed: larger files, full quality).
- The room keeps its old lights; the templates have no UV animation.
- Up to 65,535 surfaces in a room, and 65,535 collision triangles, vertices and edges (the format's 16-bit
  indices). Big meshes are split into several world models by themselves.

## For developers

Everything except `ui.py` runs without Blender, so `build.build()` can be scripted: see `build.py` for the
inputs. The geometry and collision writers are ports of Prime World Editor's `CWorldGeometryCooker` and
`CCollisionCooker`; each module's docstring names the game code its layout comes from.
`echoes_disc.py` is vendored from the port's `tools/echoes_disc.py`.
