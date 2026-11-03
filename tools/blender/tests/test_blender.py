"""End-to-end test of Echoes Room Tools inside Blender (bpy as a module, or `blender -b --python`).

    python tools/blender/tests/test_blender.py [--check <pwe_world_geometry_cooker_tests>]

Builds a small disc image (one PAK: a world, a room with geometry and a retail-like material, its
textures), enables the add-on, imports the room as a reference, models a new room with template, disc
and lightmapped materials, builds it into a mods folder, and reads the result back. --check also runs
the C++ engine-reader check on the built room.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import bpy            # noqa: E402
import addon_utils    # noqa: E402

from io_echoes_room import collision as col          # noqa: E402
from io_echoes_room import geometry as geo           # noqa: E402
from io_echoes_room import materials as mats         # noqa: E402
from io_echoes_room import mrea, room_reader, txtr   # noqa: E402
import test_core                                     # noqa: E402

FAILURES = []


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print("FAIL:", what)


def be(*words):
    return struct.pack(">%dI" % len(words), *words)


def strg(text):
    return be(0x87654321, 1, 1, 1) + b"ENGL" + be(0, 0) + be(0, 0) + be(4) + text.encode("utf-16-be") + b"\0\0"


def mlvl(world_strg, area_strg, mrea_id):
    body = be(0xDEAFBABE, 0x17, world_strg, 0x12, 0x13, 0x14, 0x15) + be(1)
    body += be(area_strg) + b"\0" * 48 + struct.pack(">6f", -50, -50, -50, 50, 50, 50) + be(mrea_id, 0)
    body += be(0) + be(0) + be(0) + be(0) + be(0) + be(0) + be(0) + b"01_test_hall\0"
    return body


def pak(named, resources):
    head = be(0x30005, 0, len(named))
    for name, (rtype, rid) in named.items():
        head += rtype.encode() + be(rid, len(name)) + name.encode()
    offset = len(head) + 4 + 20 * len(resources)
    offset += -offset % 32
    table, data = be(len(resources)), b""
    for rid, (rtype, blob) in resources.items():
        table += be(0) + rtype.encode() + be(rid, len(blob), offset + len(data))
        data += blob + b"\0" * (-len(blob) % 32)
    out = head + table
    return out + b"\0" * (-len(out) % 32) + data


def gc_disc(files):
    names = b"".join(name.encode() + b"\0" for name in files)
    count = len(files) + 1
    fst_off = 0x2440
    data_off = (fst_off + count * 12 + len(names) + 0x7FFF) & ~0x7FFF
    entries = struct.pack(">III", 1 << 24, 0, count)
    name_off, pos, body = 0, data_off, b""
    for name, data in files.items():
        entries += struct.pack(">III", name_off, pos, len(data))
        name_off += len(name) + 1
        body += data + b"\0" * (-len(data) % 0x8000)
        pos += len(data) + (-len(data) % 0x8000)
    head = bytearray(fst_off)
    head[0:6] = b"G2ME01"
    struct.pack_into(">II", head, 0x424, fst_off, count * 12 + len(names))
    image = bytes(head) + entries + names
    return image + b"\0" * (data_off - len(image)) + body


def make_disc(path: pathlib.Path):
    """A disc whose room has one model drawn with a retail-like lightmapped material (textures 0x1000
    lightmap, 0x2000 diffuse, 0x3000 glow, all in the PAK)."""
    material_set = mats.MaterialSet([0x1000, 0x2000, 0x3000], [test_core.retail_like_material()]).to_bytes()
    m = geo.Model(positions=[(0, 0, 0), (4, 0, 0), (4, 4, 0), (0, 4, 0)], normals=[(0, 0, 1)],
                  texcoords=[(0, 0), (1, 0), (1, 1), (0, 1), (0.5, 0.5)])
    m.surfaces = [geo.Surface(0, [geo.Corner(i, 0, 0, (4, i, 0, 0, 0, 0, 0)) for i in (0, 1, 2, 0, 2, 3)])]
    g = geo.write(material_set, [m])
    room = mrea.splice(test_core.base_room(), g.sections, g.model_count)
    texture = txtr.encode_rgba8(test_core.image(8, 8, 7))
    resources = {0x900: ("MLVL", mlvl(0x11, 0x2000AA, 0x800)), 0x800: ("MREA", room), 0x11: ("STRG", strg("Test World")),
                 0x2000AA: ("STRG", strg("Test Hall")), 0x1000: ("TXTR", texture), 0x2000: ("TXTR", texture),
                 0x3000: ("TXTR", texture)}
    path.write_bytes(gc_disc({"Metroid1.pak": pak({"test_world": ("MLVL", 0x900)}, resources)}))


def new_image(name, w, h, seed):
    rgba = test_core.image(w, h, seed)
    image = bpy.data.images.new(name, w, h, alpha=True)
    image.pixels.foreach_set((rgba.astype(np.float32) / 255.0).ravel())
    return image, rgba


def floor_object(name, x0, mats_):
    """A 4 x 4 plane at x0 with two faces (one material each), UVMap and Lightmap."""
    mesh = bpy.data.meshes.new(name)
    verts = [(x0, 0, 0), (x0 + 2, 0, 0), (x0 + 4, 0, 0), (x0, 4, 0), (x0 + 2, 4, 0), (x0 + 4, 4, 0)]
    mesh.from_pydata(verts, [], [(0, 1, 4, 3), (1, 2, 5, 4)])
    for m in mats_:
        mesh.materials.append(m)
    mesh.polygons[1].material_index = len(mats_) - 1
    for layer_name in ("UVMap", "Lightmap"):
        layer = mesh.uv_layers.new(name=layer_name)
        for loop in mesh.loops:
            v = mesh.vertices[loop.vertex_index].co
            layer.data[loop.index].uv = ((v.x - x0) / 4, v.y / 4)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def main():
    check_binary = sys.argv[sys.argv.index("--check") + 1] if "--check" in sys.argv else None
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        make_disc(tmp / "echoes.iso")
        (tmp / "mods").mkdir()
        for obj in list(bpy.data.objects):      # the default scene's cube, camera and light
            bpy.data.objects.remove(obj)
        mod = addon_utils.enable("io_echoes_room", default_set=True)
        check(mod is not None, "the add-on enables")
        prefs = bpy.context.preferences.addons["io_echoes_room"].preferences
        prefs.disc_path, prefs.mods_path = str(tmp / "echoes.iso"), str(tmp / "mods")
        scene = bpy.context.scene
        from io_echoes_room import ui
        worlds = ui.world_items(None, bpy.context)
        check(worlds[0][1] == "Test World", f"worlds listed ({worlds})")
        scene.echoes_room.world = worlds[0][0]
        areas = ui.area_items(None, bpy.context)
        check(areas[0][1] == "0: Test Hall", f"rooms listed ({areas})")
        scene.echoes_room.area = areas[0][0]

        # Import the room as a reference.
        check(bpy.ops.echoes.import_room() == {"FINISHED"}, "import room")
        refs = [o for o in scene.objects if o.echoes_reference]
        check(len(refs) == 1 and len(refs[0].data.polygons) == 2, "reference model imported")
        disc_mat = bpy.data.materials.get("01_test_hall 0")
        check(disc_mat is not None and disc_mat.echoes.source == "DISC", "disc material imported")
        check(disc_mat.echoes.disc_textures == "00001000,00002000,00003000", "its texture IDs")
        check(bpy.data.images.get("TXTR_00002000") is not None, "its textures decoded")
        uv = refs[0].data.uv_layers["UVMap"].data
        check(any(abs(d.uv[0] - 1.0) < 1e-6 and abs(d.uv[1] - 1.0) < 1e-6 for d in uv), "diffuse UVs, v flipped")

        # Model the new room: a template material, a lightmapped one, and the imported disc material
        # with its lightmap replaced.
        diffuse, diffuse_rgba = new_image("stone.png", 8, 8, 11)
        lightmap, _ = new_image("baked.png", 16, 16, 12)
        bpy.ops.echoes.new_material(template="LIT")
        lit = bpy.data.materials["Echoes Lit"]
        lit.echoes.diffuse = diffuse
        lit.echoes.surface = "METAL"
        bpy.ops.echoes.new_material(template="LIGHTMAPPED")
        lm = bpy.data.materials["Echoes Lightmapped"]
        lm.echoes.diffuse, lm.echoes.lightmap = diffuse, lightmap
        o = disc_mat.echoes.overrides.add()
        o.slot, o.image = 0, lightmap
        floor_object("FloorA", 0.0, [lit, lm])
        b = floor_object("FloorB", 8.0, [disc_mat])
        b.scale = (-1.0, 1.0, 1.0)     # mirrored: the build must turn its faces back the right way
        wall = floor_object("Wall", 20.0, [lit])
        wall.echoes_role = "COLLISION"

        check(bpy.ops.echoes.build_room() == {"FINISHED"}, "build room")
        report = scene.echoes_room.last_report
        check("2 models" in report or "3 models" in report, f"build report ({report})")
        out = tmp / "mods" / "my_room" / "00000800.MREA"
        check(out.is_file(), "room written to the mods folder")
        textures = sorted((tmp / "mods" / "my_room" / "Metroid1.pak").glob("*.TXTR"))
        check(len(textures) == 2, f"two new textures in the PAK folder ({[t.name for t in textures]})")
        g = room_reader.read_room(out.read_bytes())
        check(len(g.models) == 2, f"two drawn models ({len(g.models)})")
        check(len(g.material_set.materials) == 3, "three materials")
        check(0x2000 in g.material_set.texture_ids and 0x1000 not in g.material_set.texture_ids,
              "disc material keeps its diffuse, loses the replaced lightmap")
        stone = next(t for t in textures if np.array_equal(txtr.decode(t.read_bytes()), diffuse_rgba[::-1]))
        check(stone is not None, "the texture is the image, top row first")
        # The mirrored floor still faces up: its triangles' normals point +z.
        mirrored = g.models[1]
        for surf in mirrored.surfaces:
            for tri in surf.triangles:
                a, b_, c = (mirrored.positions[k[0]] for k in tri)
                nz = (b_[0] - a[0]) * (c[1] - a[1]) - (b_[1] - a[1]) * (c[0] - a[0])
                check(nz > 0, "mirrored object's faces turned back up")
        h, s = mrea.split(out.read_bytes())
        tris, _, _, _ = test_core.collision_triangles(s[h[20]])
        # Each plane is two quads, four triangles: both floors and the wall collide.
        check(len(tris) == 4 + 4 + 4, f"collision: both floors and the wall ({len(tris)})")
        check(sum(1 for _, f in tris if f & col.METAL) == 2 + 4, "the Lit material's metal (FloorA's first face, the wall)")
        check(all(f & col.FLOOR for _, f in tris), "every plane faces up: all floor")

        # The panels draw without errors (background mode never draws them): a stand-in layout that
        # records what is asked of it, and contexts with a material and an object.
        class Layout:
            def __init__(self):
                self.calls = []

            def __getattr__(self, name):

                def call(*args, **kwargs):
                    self.calls.append(name)
                    return self
                return call

        class Panel:
            def __init__(self):
                self.layout = Layout()

        class Ctx:
            def __init__(self, **kw):
                self.__dict__.update(scene=scene, preferences=bpy.context.preferences, **kw)
        for panel_cls, ctx in ((ui.VIEW3D_PT_echoes_room, Ctx()), (ui.MATERIAL_PT_echoes, Ctx(material=lm)),
                               (ui.MATERIAL_PT_echoes, Ctx(material=disc_mat)), (ui.OBJECT_PT_echoes, Ctx(object=refs[0]))):
            p = Panel()
            try:
                panel_cls.draw(p, ctx)
                check(len(p.layout.calls) >= 2, f"{panel_cls.__name__} draws")
            except Exception as e:
                check(False, f"{panel_cls.__name__} draw raised {e!r}")
        if check_binary:
            r = subprocess.run([check_binary, "--check-mrea", str(out)], capture_output=True, text=True)
            check(r.returncode == 0, f"C++ engine-reader check: {r.stderr.strip().splitlines()[-1:]}")
        addon_utils.disable("io_echoes_room")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        sys.exit(1)
    print("echoes room tools in Blender: ok")


if __name__ == "__main__":
    main()
