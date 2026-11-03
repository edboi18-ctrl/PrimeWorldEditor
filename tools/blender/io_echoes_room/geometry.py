"""Echoes world geometry sections, a port of Prime World Editor's CWorldGeometryCooker
(src/Core/Resource/Cooker/CWorldGeometryCooker.cpp, which cites the game code each rule comes from).

Writes, in the order CGameArea::PostConstructArea and FillInStaticGeometry read them: the material set,
then per model its header, positions, normals, colours, float UVs, packed UVs, the surface count with
each surface's end offset, one section per surface, surface records and surface groups; then the render
octree, the area surface list and the ambient light table. Every model surface gets an area surface and a
surface group of its own. Sections are padded to 32 bytes.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

from . import materials as mats

IDENTITY = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0)
CHILD_COUNTS = (0, 2, 2, 4, 2, 4, 4, 8)
AXES = (-1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, -1, -1, -1, 0, 2, 1, 2, 0, 1)  # CAreaRenderOctTree skAxes


class GeometryError(Exception):
    pass


@dataclass
class Corner:
    position: int
    normal: int = 0
    color: int = 0
    tex: tuple = (0, 0, 0, 0, 0, 0, 0)   # TEX0-6; TEX0 indexes lightmap_texcoords for packed-UV materials


@dataclass
class Surface:
    material: int
    corners: list    # three per triangle, counter-clockwise from the front


@dataclass
class Model:
    positions: list = field(default_factory=list)          # (x, y, z)
    normals: list = field(default_factory=list)            # (x, y, z)
    colors: list = field(default_factory=list)             # 0xRRGGBBAA
    texcoords: list = field(default_factory=list)          # (u, v), GX_F32
    lightmap_texcoords: list = field(default_factory=list)  # (u, v), packed U16 with 15 fraction bits
    surfaces: list = field(default_factory=list)
    transform: tuple = IDENTITY
    extra_flags: int = 0


@dataclass
class Output:
    sections: list
    model_count: int
    area_surface_count: int
    bounds: tuple


def _pad(b: bytearray) -> bytes:
    b += b"\0" * (-len(b) % 32)
    return bytes(b)


def _box(bounds) -> bytes:
    return struct.pack(">6f", *bounds[0], *bounds[1])


def _empty_box():
    return [[math.inf] * 3, [-math.inf] * 3]


def _expand(box, p):
    for i in range(3):
        box[0][i] = min(box[0][i], p[i])
        box[1][i] = max(box[1][i], p[i])


def _expand_box(box, other):
    _expand(box, other[0])
    _expand(box, other[1])


def _overlap(a, b):
    return all(a[0][i] <= b[1][i] and a[1][i] >= b[0][i] for i in range(3))


def _child_bounds(box, flags, child):
    mn, mx = list(box[0]), list(box[1])
    ct = [(mn[i] + mx[i]) * 0.5 for i in range(3)]
    if flags in (1, 2, 4):
        a = {1: 0, 2: 1, 4: 2}[flags]
        if child == 0:
            mx[a] = ct[a]
        else:
            mn[a] = ct[a]
    elif flags in (3, 5, 6):
        a, b = AXES[flags * 3], AXES[flags * 3 + 1]
        (mn if child & 1 else mx)[a] = ct[a]
        (mn if child & 2 else mx)[b] = ct[b]
    elif flags == 7:
        for i in range(3):
            (mn if child & (1 << i) else mx)[i] = ct[i]
    return [mn, mx]


def _render_octree(bounds_list, box, max_leaf, max_depth) -> bytes:
    nodes = []  # [surfaces, flags, children]

    def build(nb, surfs, depth):
        index = len(nodes)
        nodes.append([surfs, 0, []])
        if len(surfs) <= max_leaf or depth >= max_depth:
            return index
        size = [nb[1][i] - nb[0][i] for i in range(3)]
        longest = max(size)
        flags = 0
        for i in range(3):
            if size[i] >= 0.5 * longest and size[i] > 0:
                flags |= 1 << i
        if flags == 6:   # the game's axis table gives this split Z and X
            flags = 7
        if flags == 0:
            return index
        child_sets, smaller = [], False
        for c in range(CHILD_COUNTS[flags]):
            cb = _child_bounds(nb, flags, c)
            s = [k for k in surfs if _overlap(bounds_list[k], cb)]
            child_sets.append(s)
            smaller |= len(s) < len(surfs)
        if not smaller:
            return index
        nodes[index][1] = flags
        for c in range(CHILD_COUNTS[flags]):
            nodes[index][2].append(build(_child_bounds(nb, flags, c), child_sets[c], depth + 1))
        return index

    build(box, list(range(len(bounds_list))), 0)
    mesh_count = len(bounds_list)
    words = (mesh_count + 31) // 32
    bitmap_index, bitmaps, node_bitmap = {}, [], []
    for surfs, _, _ in nodes:
        w = [0] * words
        for k in surfs:
            w[k >> 5] |= 1 << (k & 31)
        key = tuple(w)
        if key not in bitmap_index:
            bitmap_index[key] = len(bitmaps)
            bitmaps.append(key)
        node_bitmap.append(bitmap_index[key])
    if len(nodes) > 0xFFFF or len(bitmaps) > 0xFFFF:
        raise GeometryError("The render octree has more than 65535 nodes or bitmaps.")
    out = bytearray(struct.pack(">5I", 0x41524F54, 1, len(bitmaps), mesh_count, len(nodes)))
    out += _box(box)
    out += b"\0" * (64 - len(out))
    for key in bitmaps:
        out += struct.pack(">%dI" % words, *key)
    table_at = len(out)
    out += b"\0" * (4 * len(nodes))
    entries_at = len(out)
    for i, (_, flags, children) in enumerate(nodes):
        struct.pack_into(">I", out, table_at + 4 * i, len(out) - entries_at)
        out += struct.pack(">HBB", node_bitmap[i], 0, flags)
        out += struct.pack(">%dH" % len(children), *children)
    return _pad(out)


def lround(x: float) -> int:
    """C's lround: halves away from zero (Python's round() goes to even), as the C++ writer rounds."""
    return int(math.copysign(math.floor(abs(x) + 0.5), x))


def _attr_type(desc, attr):
    return desc >> (attr * 2) & 3


def write(material_set: bytes, models: list, max_leaf_surfaces: int = 16, max_depth: int = 8) -> Output:
    """The geometry sections for these models over a cooked material set."""
    infos = [mats.parse_material(m) for m in mats.parse_set(material_set).materials]
    for model in models:
        for s in model.surfaces:
            if s.material >= len(infos):
                raise GeometryError("A surface uses a material index past the material set.")
            desc = infos[s.material].vertex_desc
            if desc & 0xFF000000:
                raise GeometryError("A material asks for per-vertex matrix indices, which world geometry does not use here.")
            if _attr_type(desc, 0) < 2:
                raise GeometryError("A material has no indexed positions.")
            for attr in range(12):
                t = _attr_type(desc, attr)
                if t == 1 or (t != 0 and attr in (3, 11)):
                    raise GeometryError("A material uses direct vertex data, CLR1 or TEX7.")
            if not s.corners or len(s.corners) % 3:
                raise GeometryError("A surface's corner count is not a positive multiple of 3.")

    sections = [_pad(bytearray(material_set))]
    area_bounds, area_owner = [], []
    room = _empty_box()

    for model_index, model in enumerate(models):
        if not model.surfaces or len(model.surfaces) > 0xFFFF:
            raise GeometryError("A world model has no surfaces, or more than 65535.")
        if max(len(model.positions), len(model.normals), len(model.colors), len(model.texcoords),
               len(model.lightmap_texcoords)) > 0x10000:
            raise GeometryError("A world model has an array of more than 65536 entries.")
        # Packed lightmap UVs need GX_VTXFMT2, which reads normals as S16: the model then has 16-bit
        # normals throughout (flag bit 0) and its other surfaces use GX_VTXFMT1.
        short = any(infos[s.material].flags & mats.LIGHTMAP_UV_ARRAY for s in model.surfaces)
        mbox = _empty_box()
        for s in model.surfaces:
            for c in s.corners:
                if c.position < len(model.positions):
                    _expand(mbox, model.positions[c.position])

        sections.append(_pad(bytearray(struct.pack(">I12f", (model.extra_flags & ~1) | (1 if short else 0), *model.transform)
                                       + _box(mbox))))
        sections.append(_pad(bytearray(b"".join(struct.pack(">3f", *p) for p in model.positions))))
        if short:
            def s16(f):
                return max(-32768, min(32767, lround(f * 16384.0)))
            normals = b"".join(struct.pack(">3h", s16(n[0]), s16(n[1]), s16(n[2])) for n in model.normals)
        else:
            normals = b"".join(struct.pack(">3f", *n) for n in model.normals)
        sections.append(_pad(bytearray(normals)))
        sections.append(_pad(bytearray(b"".join(struct.pack(">I", c) for c in model.colors))))
        sections.append(_pad(bytearray(b"".join(struct.pack(">2f", *uv) for uv in model.texcoords))))

        def u16(f):
            return max(0, min(65535, lround(f * 32768.0)))
        sections.append(_pad(bytearray(b"".join(struct.pack(">2H", u16(uv[0]), u16(uv[1])) for uv in model.lightmap_texcoords))))

        surface_sections = []
        for surf_index, s in enumerate(model.surfaces):
            info = infos[s.material]
            desc = info.vertex_desc
            packed = bool(info.flags & mats.LIGHTMAP_UV_ARRAY)
            fmt = (2 if packed else 1) if short else 0
            arrays = [len(model.positions), len(model.normals), len(model.colors), 0]
            box = _empty_box()
            fx = fy = fz = 0.0
            for i, c in enumerate(s.corners):
                idx = [c.position, c.normal, c.color, 0, *c.tex]
                for attr in range(11):
                    t = _attr_type(desc, attr)
                    if t == 0:
                        continue
                    count = arrays[attr] if attr < 4 else (
                        len(model.lightmap_texcoords) if (attr == 4 and packed) else len(model.texcoords))
                    if not (idx[attr] < count and (t == 3 or idx[attr] < 0x100)):
                        raise GeometryError("A corner's index is past its array, or past 255 for an 8-bit index.")
                _expand(box, model.positions[c.position])
                if i % 3 == 2:
                    a = model.positions[s.corners[i - 2].position]
                    b = model.positions[s.corners[i - 1].position]
                    p = model.positions[c.position]
                    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
                    v = (p[0] - a[0], p[1] - a[1], p[2] - a[2])
                    fx += u[1] * v[2] - u[2] * v[1]
                    fy += u[2] * v[0] - u[0] * v[2]
                    fz += u[0] * v[1] - u[1] * v[0]
            length = math.sqrt(fx * fx + fy * fy + fz * fz)
            hint = (fx / length, fy / length, fz / length) if length > 1e-12 else (1.0, 0.0, 0.0)

            # GX_TRIANGLES | format, u16 count, then each attribute's index in GX order.
            dl = bytearray()
            per = 0xFFFF // 3 * 3
            for first in range(0, len(s.corners), per):
                chunk = s.corners[first:first + per]
                dl += struct.pack(">BH", 0x90 | fmt, len(chunk))
                for c in chunk:
                    idx = (c.position, c.normal, c.color, 0, *c.tex)
                    for attr in range(11):
                        t = _attr_type(desc, attr)
                        if t == 3:
                            dl += struct.pack(">H", idx[attr])
                        elif t == 2:
                            dl += struct.pack(">B", idx[attr])
            dl = _pad(dl)
            center = tuple((box[0][i] + box[1][i]) * 0.5 for i in range(3))
            header = bytearray(struct.pack(">3fIIIII3fhh", *center, s.material, 0x80000000 | len(dl), 0, 0, 0x18, *hint, 0, 0))
            header += _box(box)
            surface_sections.append(_pad(header) + dl)
            if len(area_bounds) >= 0xFFFF:
                raise GeometryError("More than 65535 surfaces in the room.")
            area_bounds.append(box)
            area_owner.append((model_index, surf_index))
            _expand_box(room, box)

        ends, end = [], 0
        for b in surface_sections:
            end += len(b)
            ends.append(end)
        sections.append(_pad(bytearray(struct.pack(">I%dI" % len(ends), len(ends), *ends))))
        sections += surface_sections
        first_area = len(area_bounds) - len(model.surfaces)
        n = len(model.surfaces)
        records = struct.pack(">H", n) + b"".join(struct.pack(">HH", 0, first_area + i) for i in range(n))
        sections.append(_pad(bytearray(records)))
        groups = struct.pack(">H", n) + struct.pack(">%dH" % n, *range(1, n + 1)) + struct.pack(">%dH" % n, *range(n))
        sections.append(_pad(bytearray(groups)))

    if not area_bounds:
        raise GeometryError("There are no world models.")
    sections.append(_render_octree(area_bounds, room, max_leaf_surfaces, max_depth))
    areas = bytearray(struct.pack(">I", len(area_bounds) + 1) + _box(room) + struct.pack(">4h", -1, -1, 0, 0))
    for box, (m, g) in zip(area_bounds, area_owner):
        areas += _box(box) + struct.pack(">4h", m, g, 0, 0)
    sections.append(_pad(areas))
    sections.append(_pad(bytearray(struct.pack(">II", 0, 0))))
    return Output(sections, len(models), len(area_bounds), (tuple(room[0]), tuple(room[1])))
