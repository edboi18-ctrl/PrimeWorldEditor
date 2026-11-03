"""Read an Echoes room's world geometry back into triangles: for importing a retail room into Blender
as a reference, and for checking what geometry.py writes.

Walks the sections as CGameArea::FillInStaticGeometry does and each surface's display list as
CCubeModel does (primitive & 0xF8, the vertex format in the low bits, a u16 count, then per vertex the
index of every attribute its material's vertex description has, after one byte per matrix index).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

from . import materials as mats
from . import mrea

GX_QUADS, GX_TRIANGLES, GX_TRIANGLESTRIP, GX_TRIANGLEFAN = 0x80, 0x90, 0x98, 0xA0


@dataclass
class ReadSurface:
    material: int
    triangles: list = field(default_factory=list)   # [(corner, corner, corner)], corner = tuple of 11 indices or None
    bounds: tuple = None


@dataclass
class ReadModel:
    flags: int
    positions: list
    normals: list
    colors: list
    texcoords: list
    lightmap_texcoords: list
    surfaces: list
    records: list = field(default_factory=list)   # per surface: (first halfword, area surface index)
    groups: list = field(default_factory=list)    # per group: surface indices


@dataclass
class ReadGeometry:
    material_set: mats.MaterialSet
    models: list
    area_surfaces: list            # (bounds, model, group), entry 0 included
    octree: bytes


def _floats(b: bytes, n: int):
    count = len(b) // (4 * n)
    return [struct.unpack_from(">%df" % n, b, 4 * n * i) for i in range(count)]


def read_sections(sections: list, model_count: int) -> ReadGeometry:
    """The geometry from its sections (materials through the ambient light table)."""
    mset = mats.parse_set(sections[0])
    infos = [mats.parse_material(m) for m in mset.materials]
    s, models = 1, []
    for _ in range(model_count):
        flags = struct.unpack_from(">I", sections[s], 0)[0]
        short = bool(flags & 1)
        positions = _floats(sections[s + 1], 3)
        nb = sections[s + 2]
        if short:
            normals = [tuple(v / 16384.0 for v in struct.unpack_from(">3h", nb, 6 * i)) for i in range(len(nb) // 6)]
        else:
            normals = _floats(nb, 3)
        cb = sections[s + 3]
        colors = [struct.unpack_from(">I", cb, 4 * i)[0] for i in range(len(cb) // 4)]
        texcoords = _floats(sections[s + 4], 2)
        pb = sections[s + 5]
        lightmap = [tuple(v / 32768.0 for v in struct.unpack_from(">2H", pb, 4 * i)) for i in range(len(pb) // 4)]
        n = struct.unpack_from(">I", sections[s + 6], 0)[0]
        surfaces = []
        for i in range(n):
            sec = sections[s + 7 + i]
            material = struct.unpack_from(">I", sec, 0xC)[0]
            dl_size = struct.unpack_from(">I", sec, 0x10)[0] & 0x7FFFFFFF
            extra = struct.unpack_from(">I", sec, 0x1C)[0]
            bounds = struct.unpack_from(">6f", sec, 0x30) if extra else None
            surf = ReadSurface(material, [], bounds)
            desc = infos[material].vertex_desc if material < len(infos) else 0
            types = [desc >> (2 * a) & 3 for a in range(11)]
            matrix_bytes = bin(desc >> 24).count("1")
            p = (0x4F + extra) & ~31
            end = p + dl_size
            while p < end and p < len(sec):
                prim = sec[p] & 0xF8
                if prim == 0:
                    break
                count = struct.unpack_from(">H", sec, p + 1)[0]
                p += 3
                verts = []
                for _ in range(count):
                    p += matrix_bytes
                    corner = []
                    for t in types:
                        if t == 3:
                            corner.append(struct.unpack_from(">H", sec, p)[0])
                            p += 2
                        elif t == 2:
                            corner.append(sec[p])
                            p += 1
                        else:
                            corner.append(None)
                    verts.append(tuple(corner))
                if prim == GX_TRIANGLES:
                    surf.triangles += [tuple(verts[k:k + 3]) for k in range(0, count - 2, 3)]
                elif prim == GX_TRIANGLESTRIP:
                    surf.triangles += [(verts[k], verts[k + 1], verts[k + 2]) if k % 2 == 0 else
                                       (verts[k + 1], verts[k], verts[k + 2]) for k in range(count - 2)]
                elif prim == GX_TRIANGLEFAN:
                    surf.triangles += [(verts[0], verts[k + 1], verts[k + 2]) for k in range(count - 2)]
                elif prim == GX_QUADS:
                    for k in range(0, count - 3, 4):
                        surf.triangles += [(verts[k], verts[k + 1], verts[k + 2]), (verts[k], verts[k + 2], verts[k + 3])]
            surfaces.append(surf)
        rec = sections[s + 7 + n]
        records = [struct.unpack_from(">HH", rec, 2 + 4 * i) for i in range(n)]
        grp = sections[s + 8 + n]
        g_count = struct.unpack_from(">H", grp, 0)[0]
        g_ends = struct.unpack_from(">%dH" % g_count, grp, 2)
        groups, start = [], 0
        for e in g_ends:
            groups.append(list(struct.unpack_from(">%dH" % (e - start), grp, 2 + 2 * g_count + 2 * start)))
            start = e
        models.append(ReadModel(flags, positions, normals, colors, texcoords, lightmap, surfaces, records, groups))
        s += 9 + n
    octree = sections[s]
    ab = sections[s + 1]
    count = struct.unpack_from(">I", ab, 0)[0]
    areas = []
    for k in range(count):
        v = struct.unpack_from(">6f4h", ab, 4 + 32 * k)
        areas.append((v[:6], v[6], v[7]))
    return ReadGeometry(mset, models, areas, octree)


def read_room(data: bytes) -> ReadGeometry:
    """The world geometry of a whole MREA."""
    h, sections = mrea.split(data)
    return read_sections(sections[h[17]:h[18]], h[14])
