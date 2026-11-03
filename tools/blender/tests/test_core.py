"""Tests for the Echoes Room Tools add-on's Blender-free core (tools/blender/io_echoes_room).

    python3 tools/blender/tests/test_core.py [--dump <dir>]

Synthetic data only: the textures, materials, rooms and collision are built here to the game's layouts
(each module's docstring names the game code). --dump writes a built room for the C++ engine-reader
check (pwe_world_geometry_cooker_tests --check-mrea).
"""
import math
import pathlib
import struct
import sys
import unittest

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
DUMP = None
if "--dump" in sys.argv:
    DUMP = pathlib.Path(sys.argv[sys.argv.index("--dump") + 1])
    del sys.argv[sys.argv.index("--dump"):sys.argv.index("--dump") + 2]

from io_echoes_room import build as bld          # noqa: E402
from io_echoes_room import collision as col      # noqa: E402
from io_echoes_room import geometry as geo       # noqa: E402
from io_echoes_room import materials as mats     # noqa: E402
from io_echoes_room import mrea                  # noqa: E402
from io_echoes_room import room_reader           # noqa: E402
from io_echoes_room import txtr                  # noqa: E402


# ---------------------------------------------------------------------------- shared builders
def retail_like_material(group=0):
    """A lightmapped room material with a glow layer, laid out like the disc's (stage 0 RASC + C1 x
    lightmap to C0, stage 1 C0 x diffuse, stage 2 the glow added onto CPREV)."""
    a = mats.args
    stages = [mats.Stage(a(15, 4, 8, 10), a(7, 7, 7, 7), mats.op(1), mats.op(1), 4, 0, 0),
              mats.Stage(a(15, 2, 8, 15), a(7, 7, 7, 4), mats.op(0), mats.op(0), 0xFF, 1, 1),
              mats.Stage(a(15, 8, 12, 0), a(7, 7, 7, 0), mats.op(0), mats.op(0), 0xFF, 2, 1)]
    m = mats.Material(0x4082 | mats.LIGHTMAP, [0, 1, 2], 3 | 3 << 2 | 3 << 8 | 3 << 10, group=group,
                      stages=stages, texgens=[mats.texgen(0), mats.texgen(1)])
    return m.to_bytes()


def base_room(layers=(b"SCLY" + b"\1" * 28, b"SCLY" + b"\2" * 60)):
    """An Echoes MREA laid out as retail: geometry 0-3 (materials, a model would be here, octree, area
    surfaces, ambient), the script layers, SCGN, collision, BSP, lights, VISI, PATH, PTLA, EGMC. Sections
    hold recognisable bytes."""
    geometry = [bytes([0xA0 + i]) * 32 for i in range(4)]
    rest = [b"SCGN" + b"\0" * 28, b"\xC0" * 64, b"\xB5" * 32, b"\xB1" * 32, b"VISI" + b"\x55" * 92,
            struct.pack(">I", 0x12345678) + b"\0" * 28, struct.pack(">I", 0x23456789) + b"\0" * 28,
            struct.pack(">I", 0x3456789A) + b"\0" * 28]
    sections = geometry + list(layers) + rest
    g = len(geometry)
    n = len(layers)
    h = [0] * 32
    h[0], h[1] = 0xDEADBEEF, 0x19
    h[2:14] = [struct.unpack(">I", struct.pack(">f", f))[0] for f in geo.IDENTITY]
    h[14], h[15] = 0, n
    h[17], h[18], h[19] = 0, g, g + n
    h[20:28] = [g + n + 1, g + n + 2, g + n + 3, g + n + 4, g + n + 5, g - 1, g + n + 6, g + n + 7]
    return mrea.assemble(h, sections)


def image(w, h, seed=1):
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, (h, w, 4), dtype=np.uint8)


def quad(x0, y0, x1, y1, z=0.0, uvscale=1.0):
    """Two counter-clockwise (from +z) triangles of build.Corner."""
    def c(x, y):
        return bld.Corner((x, y, z), (0.0, 0.0, 1.0), (x * uvscale, y * uvscale), ((x - x0) / (x1 - x0), (y - y0) / (y1 - y0)))
    return [(c(x0, y0), c(x1, y0), c(x1, y1)), (c(x0, y0), c(x1, y1), c(x0, y1))]


# ---------------------------------------------------------------------------- engine-rule checks
def octree_query(octree: bytes, test):
    """CAreaRenderOctTree::FindOverlappingModels."""
    words = (struct.unpack_from(">I", octree, 12)[0] + 31) // 32
    bitmaps, nodes = struct.unpack_from(">I", octree, 8)[0], struct.unpack_from(">I", octree, 16)[0]
    root_box = struct.unpack_from(">6f", octree, 20)
    table = 64 + 4 * bitmaps * words
    entries = table + 4 * nodes
    out = [0] * words

    def overlap(a, b):
        return not any(b[i] > a[i + 3] or a[i] > b[i + 3] for i in range(3))

    def inside(a, b):
        return all(a[i] >= b[i] and a[i + 3] <= b[i + 3] for i in range(3))

    def rec(n, box):
        at = entries + struct.unpack_from(">I", octree, table + 4 * n)[0]
        bmp, _, flags = struct.unpack_from(">HBB", octree, at)
        if not overlap(test, box):
            return
        if flags == 0 or inside(box, test):
            for i in range(words):
                out[i] |= struct.unpack_from(">I", octree, 64 + 4 * (bmp * words + i))[0]
            return
        for c in range(geo.CHILD_COUNTS[flags]):
            child = struct.unpack_from(">H", octree, at + 4 + 2 * c)[0]
            cb = geo._child_bounds([list(box[:3]), list(box[3:])], flags, c)
            rec(child, tuple(cb[0]) + tuple(cb[1]))
    rec(0, root_box)
    return out


def collision_triangles(section: bytes):
    """Triangles as CAreaOctTree::MakeFromMemory + CCollisionPrimitiveData::GetTriangle give them,
    and every triangle index the tree's leaves hold."""
    size = struct.unpack_from(">I", section, 4)[0]
    b = section[8:8 + size]
    root, tree_size = struct.unpack_from(">II", b, 32)
    leaves = set()

    def walk(at, kind):
        if kind == 2:
            count = struct.unpack_from(">H", b, at + 24)[0]
            leaves.update(struct.unpack_from(">%dH" % count, b, at + 26))
        elif kind == 1:
            types = struct.unpack_from(">H", b, at)[0]
            offs = struct.unpack_from(">8I", b, at + 4)
            for i in range(8):
                if types >> (2 * i) & 3:
                    walk(at + 36 + offs[i], types >> (2 * i) & 3)
    walk(40, root)
    p = 40 + tree_size
    (mcount,) = struct.unpack_from(">I", b, p)
    mats_ = struct.unpack_from(">%dQ" % mcount, b, p + 4)
    p += 4 + 8 * mcount
    arrays = []
    for _ in range(3):
        (n,) = struct.unpack_from(">I", b, p)
        arrays.append(b[p + 4:p + 4 + n])
        p += 4 + n
    (ecount,) = struct.unpack_from(">I", b, p)
    edges = [struct.unpack_from(">HH", b, p + 4 + 4 * i) for i in range(ecount)]
    p += 4 + 4 * ecount
    (icount,) = struct.unpack_from(">I", b, p)
    surf = struct.unpack_from(">%dH" % icount, b, p + 4)
    p += 4 + 2 * icount
    (ncount,) = struct.unpack_from(">I", b, p)
    nb = struct.unpack_from(">%dH" % ncount, b, p + 4)
    p += 4 + 2 * ncount
    (vcount,) = struct.unpack_from(">I", b, p)
    verts = [struct.unpack_from(">3f", b, p + 4 + 12 * i) for i in range(vcount)]
    p += 4 + 12 * vcount
    assert p == len(b), "collision arrays do not end at the block's end"
    tris = []
    for t in range(icount // 3):
        e0, e1 = edges[surf[3 * t]], edges[surf[3 * t + 1]]
        flags = mats_[arrays[2][t]]
        idx = (e0[1], e0[0], e1[0]) if flags & col.FLIPPED else (e0[0], e0[1], e1[1])
        tris.append((tuple(verts[i] for i in idx), flags))
    return tris, leaves, nb, surf


def same_winding(a, b):
    return any(a[0] == b[r] and a[1] == b[(r + 1) % 3] and a[2] == b[(r + 2) % 3] for r in range(3))


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


# ---------------------------------------------------------------------------- tests
class TextureTests(unittest.TestCase):
    def test_rgba8_round_trip_and_mips(self):
        img = image(16, 8)
        data = txtr.encode_rgba8(img)
        self.assertEqual(txtr.read_header(data), (txtr.RGBA8, 16, 8, 2))   # 16x8, 8x4 (4 is odd for another level)
        self.assertEqual(len(data), 12 + 16 * 8 * 4 + 8 * 4 * 4)
        self.assertTrue(np.array_equal(txtr.decode(data), img))
        with self.assertRaises(txtr.TextureError):
            txtr.encode_rgba8(image(6, 4))

    def test_other_formats(self):
        # RGB565, one 4x4 tile of pure red; RGB5A3 with a translucent texel; IA8; I4 (two texels a byte).
        red = struct.pack(">IHHI", txtr.RGB565, 4, 4, 1) + struct.pack(">H", 0xF800) * 16
        self.assertTrue((txtr.decode(red) == [255, 0, 0, 255]).all())
        rgb5a3 = struct.pack(">IHHI", txtr.RGB5A3, 4, 4, 1) + struct.pack(">H", 0x7F00) + struct.pack(">H", 0x83E0) * 15
        out = txtr.decode(rgb5a3)
        self.assertEqual(out[0, 0].tolist(), [255, 0, 0, 255 * 0 + (7 << 5 | 7 << 2 | 3)])   # A=7 of 3 bits, R=15
        self.assertEqual(out[0, 1].tolist(), [0, 255, 0, 255])
        ia8 = struct.pack(">IHHI", txtr.IA8, 4, 4, 1) + struct.pack(">H", 0x80FF) * 16
        self.assertEqual(txtr.decode(ia8)[3, 3].tolist(), [255, 255, 255, 0x80])
        i4 = struct.pack(">IHHI", txtr.I4, 8, 8, 1) + bytes([0xF0]) * 32
        out = txtr.decode(i4)
        self.assertEqual(out[0, 0].tolist(), [255] * 4)
        self.assertEqual(out[0, 1].tolist(), [0] * 4)

    def test_cmpr(self):
        # One 8x8 block: sub-blocks red/blue (c0 > c1: four colours), each texel picking colour 0 or 1.
        sub_red = struct.pack(">HH", 0xF800, 0x001F) + bytes([0b00000000] * 4)
        sub_blue = struct.pack(">HH", 0xF800, 0x001F) + bytes([0b01010101] * 4)
        sub_mix = struct.pack(">HH", 0xF800, 0x001F) + bytes([0b00011011] * 4)   # texels 0,1,2,3 left to right
        data = struct.pack(">IHHI", txtr.CMPR, 8, 8, 1) + sub_red + sub_blue + sub_mix + sub_red
        out = txtr.decode(data)
        self.assertEqual(out[0, 0].tolist(), [255, 0, 0, 255])
        self.assertEqual(out[0, 4].tolist(), [0, 0, 255, 255])
        self.assertEqual(out[4, 0].tolist(), [255, 0, 0, 255])
        self.assertEqual(out[4, 1].tolist(), [0, 0, 255, 255])
        self.assertEqual(out[4, 2].tolist(), [170, 0, 85, 255])   # (2 red + blue) / 3
        self.assertEqual(out[7, 7].tolist(), [255, 0, 0, 255])


class MaterialTests(unittest.TestCase):
    def test_templates_round_trip(self):
        for kind in mats.TEMPLATES:
            m, roles = mats.template(kind, group=5)
            b = m.to_bytes()
            self.assertEqual(mats.parse_material(b).to_bytes(), b, kind)
            self.assertEqual(mats.parse_material(b).group, 5)
        lm, roles = mats.template("LIGHTMAPPED")
        self.assertEqual(roles, ["lightmap", "diffuse"])
        self.assertEqual(lm.roles(), {"lightmap": 0, "diffuse": 1})
        self.assertEqual(lm.lightmap_attr(), 0)
        with self.assertRaises(mats.MaterialError):
            mats.template("SHINY")

    def test_retail_like_roles(self):
        m = mats.parse_material(retail_like_material())
        self.assertEqual(m.roles(), {"lightmap": 0, "diffuse": 1, "glow": 2})
        self.assertEqual(m.lightmap_attr(), 0)
        self.assertEqual(m.tex_attrs(), [0, 1])

    def test_set_and_retexture(self):
        set_ids = [0xAAAA]
        body = mats.retexture(retail_like_material(), [0x11, 0xAAAA, 0x22], set_ids, group=9)
        self.assertEqual(set_ids, [0xAAAA, 0x11, 0x22])
        m = mats.parse_material(body)
        self.assertEqual((m.textures, m.group), ([1, 0, 2], 9))
        s = mats.MaterialSet(set_ids, [body, mats.template("LIT")[0].to_bytes()])
        back = mats.parse_set(s.to_bytes())
        self.assertEqual((back.texture_ids, back.materials), (s.texture_ids, s.materials))
        with self.assertRaises(mats.MaterialError):
            mats.parse_material(body + b"\0\0\0\0")


class GeometryTests(unittest.TestCase):
    def setUp(self):
        lit = mats.template("LIT")[0].to_bytes()
        packed, _ = mats.template("LIGHTMAPPED")
        packed.flags |= mats.LIGHTMAP_UV_ARRAY
        self.set = mats.MaterialSet([1, 2], [lit, packed.to_bytes()]).to_bytes()
        # Model 0: a floor of two triangles (LIT). Model 1: a ramp with packed lightmap UVs.
        floor = geo.Model(positions=[(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)], normals=[(0, 0, 1)],
                          texcoords=[(0, 0), (1, 0), (1, 1), (0, 1)])
        floor.surfaces = [geo.Surface(0, [geo.Corner(i, 0, 0, (i, 0, 0, 0, 0, 0, 0)) for i in (0, 1, 2, 0, 2, 3)])]
        n = 1 / math.sqrt(2)
        ramp = geo.Model(positions=[(3, 0, 0), (5, 0, 1), (5, 2, 1)], normals=[(-n, 0, n)],
                         texcoords=[(0, 0), (4, 0), (4, 4)], lightmap_texcoords=[(0.1, 0.2), (0.9, 0.2), (0.9, 0.8)])
        ramp.surfaces = [geo.Surface(1, [geo.Corner(i, 0, 0, (i, i, 0, 0, 0, 0, 0)) for i in (0, 1, 2)])]
        tiles = geo.Model(normals=[(0, 0, 1)], texcoords=[(0, 0)])
        for t in range(40):
            x, y = (t % 8) * 2 + 10, (t // 8) * 2
            b = len(tiles.positions)
            tiles.positions += [(x, y, 0), (x + 1, y, 0), (x + 1, y + 1, 0)]
            tiles.surfaces.append(geo.Surface(0, [geo.Corner(b + i) for i in range(3)]))
        self.models = [floor, ramp, tiles]

    def test_read_back(self):
        out = geo.write(self.set, self.models)
        self.assertTrue(all(len(s) % 32 == 0 for s in out.sections))
        g = room_reader.read_sections(out.sections, out.model_count)
        self.assertEqual(len(g.models), 3)
        self.assertEqual([m.flags & 1 for m in g.models], [0, 1, 0])   # packed lightmap UVs -> short normals
        self.assertEqual(len(g.area_surfaces), 1 + 1 + 1 + 40)
        self.assertEqual(g.area_surfaces[0][1:], (-1, -1))
        area = 0
        for mi, (inp, read) in enumerate(zip(self.models, g.models)):
            for si, (s_in, s_rd) in enumerate(zip(inp.surfaces, read.surfaces)):
                desc = mats.parse_material(mats.parse_set(self.set).materials[s_in.material]).vertex_desc
                tris = s_rd.triangles
                self.assertEqual(len(tris) * 3, len(s_in.corners))
                for k, tri in enumerate(tris):
                    for j, corner in enumerate(tri):
                        want = s_in.corners[3 * k + j]
                        self.assertEqual(read.positions[corner[0]], tuple(map(float, inp.positions[want.position])))
                        self.assertTrue(close(read.normals[corner[1]], inp.normals[want.normal], 2 / 16384))
                        if desc >> 8 & 3:
                            src = read.lightmap_texcoords if (mi == 1) else read.texcoords
                            want_uv = (inp.lightmap_texcoords if mi == 1 else inp.texcoords)[want.tex[0]]
                            self.assertTrue(close(src[corner[4]], want_uv, 1 / 32768))
                # Records name the area surface, which names this model and a group listing the surface.
                first, area_index = read.records[si]
                self.assertEqual(area_index, area)
                bounds, model, group = g.area_surfaces[area_index + 1]
                self.assertEqual(model, mi)
                self.assertIn(si, read.groups[group])
                bits = octree_query(g.octree, bounds)
                self.assertTrue(bits[area_index // 32] >> (area_index % 32) & 1)
                area += 1
        # No box across the room misses a surface it overlaps.
        for x in range(0, 26, 3):
            test = (x, 0.5, -1, x + 1.5, 2.5, 2)
            bits = octree_query(g.octree, test)
            for k, (b, _, _) in enumerate(g.area_surfaces[1:]):
                if not any(test[i] > b[i + 3] or b[i] > test[i + 3] for i in range(3)):
                    self.assertTrue(bits[k // 32] >> (k % 32) & 1, (x, k))

    def test_refusals(self):
        bad = mats.Material(0x4082, [0], 3 | 3 << 2 | 0x01000000, stages=[], texgens=[]).to_bytes()
        s = mats.MaterialSet([], [bad]).to_bytes()
        m = geo.Model(positions=[(0, 0, 0)] * 3, normals=[(0, 0, 1)])
        m.surfaces = [geo.Surface(0, [geo.Corner(0)] * 3)]
        with self.assertRaisesRegex(geo.GeometryError, "matrix"):
            geo.write(s, [m])
        m.surfaces = [geo.Surface(0, [geo.Corner(5)] * 3)]
        with self.assertRaisesRegex(geo.GeometryError, "past its array"):
            geo.write(self.set, [m])


class CollisionTests(unittest.TestCase):
    def test_box_room(self):
        X, Y, Z = 10.0, 10.0, 4.0
        quads = [((0, 0, 0), (X, 0, 0), (X, Y, 0), (0, Y, 0)), ((0, 0, Z), (0, Y, Z), (X, Y, Z), (X, 0, Z)),
                 ((0, 0, 0), (0, 0, Z), (X, 0, Z), (X, 0, 0)), ((X, Y, 0), (X, Y, Z), (0, Y, Z), (0, Y, 0)),
                 ((0, Y, 0), (0, Y, Z), (0, 0, Z), (0, 0, 0)), ((X, 0, 0), (X, 0, Z), (X, Y, Z), (X, Y, 0))]
        tris, flags = [], []
        for a, b, c, d in quads:
            for t in ((a, b, c), (a, c, d)):
                n = bld._face_normal([bld.Corner(p) for p in t])
                f = col.material_flags("STONE", n)
                if f not in flags:
                    flags.append(f)
                tris.append((*t, flags.index(f)))
        section = col.write_section(col.build(tris, flags))
        read, leaves, nb, surf = collision_triangles(section)
        self.assertEqual(leaves, set(range(12)))
        for (want, _), (got, f) in zip([(t[:3], None) for t in tris], read):
            self.assertTrue(same_winding(tuple(map(lambda p: tuple(map(float, p)), want)), got))
        floors = sum(1 for _, f in read if f & col.FLOOR)
        self.assertEqual((floors, sum(1 for _, f in read if f & col.CEILING), sum(1 for _, f in read if f & col.WALL)), (2, 2, 8))
        self.assertTrue(all(n != 0xFFFF for n in nb))   # closed box: every edge shared
        for t in range(12):
            for i in range(3):
                self.assertIn(surf[3 * t + i], surf[3 * nb[3 * t + i]:3 * nb[3 * t + i] + 3])


class MreaTests(unittest.TestCase):
    def test_splice(self):
        room = base_room()
        h0, s0 = mrea.split(room)
        new_geo = [b"\x11" * 32] * 7
        coll = b"\x22" * 64
        out = mrea.splice(room, new_geo, 2, coll, clear_path=True)
        h, s = mrea.split(out)
        g = 7
        self.assertEqual((h[14], h[15], h[16]), (2, 2, g + 2 + 8))
        self.assertEqual((h[17], h[18], h[19], h[20]), (0, g, g + 2, g + 3))
        self.assertEqual(h[25], g - 1)
        self.assertEqual(s[:g], new_geo)
        self.assertEqual(s[g:g + 2], s0[4:6])                  # layers kept
        self.assertEqual(s[h[19]][:4], b"SCGN")
        self.assertEqual(s[h[20]], coll)
        self.assertEqual(s[h[21]], s0[h0[21]])                 # BSP kept
        self.assertEqual(s[h[22]], s0[h0[22]])                 # lights kept
        self.assertEqual(s[h[23]], b"\0" * 32)                 # VISI cleared
        self.assertEqual(s[h[24]][:4], b"\xff" * 4)            # PATH cleared
        self.assertEqual(s[h[26]], s0[h0[26]])                 # PTLA kept
        self.assertEqual(s[h[27]][:4], b"\xff" * 4)            # EGMC cleared
        # Layers and SCGN each in a block of their own.
        blocks_at = 0x80 + ((h[16] * 4 + 31) & ~31)
        table = [struct.unpack_from(">4I", out, blocks_at + 16 * i) for i in range(h[28])]
        counts, si = {}, 0
        for _, _, cl, sc in table:
            self.assertEqual(cl, 0)
            for i in range(si, si + sc):
                counts[i] = sc
            si += sc
        for i in (h[18], h[18] + 1, h[19]):
            self.assertEqual(counts[i], 1)


class BuildTests(unittest.TestCase):
    def test_room(self):
        diffuse, lightmap, override = image(8, 8, 2), image(16, 16, 3), image(4, 4, 4)
        floor = bld.MeshInput("floor", quad(0, 0, 4, 4), [0, 0])
        ramp = bld.MeshInput("ramp", quad(4, 0, 8, 4, z=0.5), [1, 2])
        wall = bld.MeshInput("wall", [tuple(bld.Corner(p) for p in ((0, 0, 0), (0, 0, 3), (0, 4, 3)))], [0], role="COLLISION")
        deco = bld.MeshInput("deco", quad(0, 0, 1, 1, z=2), [3, 3], role="VISUAL")
        materials = [
            bld.MaterialInput("Floor", template="LIT", images={"diffuse": bld.ImageInput("floor.png", diffuse)}),
            bld.MaterialInput("Ramp", template="LIGHTMAPPED", images={"diffuse": bld.ImageInput("floor.png", diffuse),
                                                                      "lightmap": bld.ImageInput("lm.png", lightmap)}),
            bld.MaterialInput("Retail", kind="DISC", disc_material=retail_like_material(),
                              disc_texture_ids=[0x1000, 0x2000, 0x3000], overrides={0: bld.ImageInput("lm2.png", override)},
                              collision=bld.CollisionInput(surface="METAL")),
            bld.MaterialInput("Glow", template="ADDITIVE", images={"diffuse": bld.ImageInput("glow.png", override)},
                              collision=bld.CollisionInput(enabled=False)),
        ]
        inp = bld.BuildInput(base_room(), 0xABCD0001, "Metroid3.pak", [floor, ramp, wall, deco], materials,
                             disc_ids={0x1000, 0x2000, 0x3000}, area_bounds=((-1, -1, -1), (6, 6, 6)))
        out = bld.build(inp)
        self.assertIn("ABCD0001.MREA", out.files)
        textures = [k for k in out.files if k.endswith(".TXTR")]
        self.assertEqual(len(textures), 4)      # floor.png shared by two materials; lm.png, lm2.png, glow.png
        self.assertTrue(all(k.startswith("Metroid3.pak/") for k in textures))
        self.assertTrue(any("outside the room's bounds" in w for w in out.report["warnings"]))
        # Textures go out top row first.
        tid = next(t["id"] for t in out.report["new_textures"] if t["image"] == "floor.png")
        self.assertTrue(np.array_equal(txtr.decode(out.files[f"Metroid3.pak/{tid}.TXTR"]), diffuse[::-1]))

        g = room_reader.read_room(out.files["ABCD0001.MREA"])
        self.assertEqual(len(g.models), 3)       # floor, ramp, deco; the wall only collides
        self.assertEqual(len(g.material_set.materials), 4)
        self.assertIn(0x2000, g.material_set.texture_ids)   # the disc material's kept textures
        self.assertNotIn(0x1000, g.material_set.texture_ids)   # its overridden lightmap
        groups = [mats.parse_material(m).group for m in g.material_set.materials]
        self.assertEqual(len(set(groups)), 4)
        # The ramp's lightmapped surface takes the lightmap UV on TEX0 and the diffuse UV (v flipped) on TEX1.
        ramp_model = g.models[1]
        surf = next(s for s in ramp_model.surfaces if s.material == 1)
        corner = surf.triangles[0][1]            # (8, 0): diffuse uv (8, 0), lightmap (1, 0)
        self.assertEqual(ramp_model.texcoords[corner[4]], (1.0, 1.0))
        self.assertEqual(ramp_model.texcoords[corner[5]], (8.0, 1.0))

        h, s = mrea.split(out.files["ABCD0001.MREA"])
        read, _, _, _ = collision_triangles(s[h[20]])
        self.assertEqual(len(read), 2 + 2 + 1)   # floor, ramp, wall; the glow does not collide
        self.assertEqual(sum(1 for _, f in read if f & col.METAL), 1)
        if DUMP:
            DUMP.mkdir(parents=True, exist_ok=True)
            (DUMP / "addon_room.MREA").write_bytes(out.files["ABCD0001.MREA"])

    def test_errors(self):
        inp = bld.BuildInput(base_room(), 1, "Metroid3.pak", [bld.MeshInput("m", quad(0, 0, 1, 1), [0, 0])],
                             [bld.MaterialInput("x", template="LIT")])
        with self.assertRaisesRegex(bld.BuildError, "needs a diffuse image"):
            bld.build(inp)
        inp.materials = [bld.MaterialInput("x", template="LIT", images={"diffuse": bld.ImageInput("odd", image(6, 6))})]
        with self.assertRaisesRegex(bld.BuildError, "multiples of 4"):
            bld.build(inp)
        inp.materials[0].images["diffuse"] = bld.ImageInput("ok", image(4, 4))
        inp.meshes[0].role = "COLLISION"
        with self.assertRaisesRegex(bld.BuildError, "No drawn geometry"):
            bld.build(inp)

    def test_area_bounds_from_mlvl(self):
        body = struct.pack(">7I", 0xDEAFBABE, 0x17, 0x11, 0x12, 0x13, 0x14, 0x15) + struct.pack(">I", 2)
        for i in range(2):
            body += struct.pack(">I", 0x20 + i) + b"\0" * 48 + struct.pack(">6f", i, i, i, i + 1, i + 1, i + 1)
            body += struct.pack(">II", 0x800 + i, i) + struct.pack(">7I", 0, 0, 0, 0, 0, 0, 0) + b"area\0"
        self.assertEqual(bld.area_bounds_from_mlvl(body, 1), ((1.0, 1.0, 1.0), (2.0, 2.0, 2.0)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
