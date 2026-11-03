"""Echoes area collision, a port of Prime World Editor's CCollisionCooker
(src/Core/Resource/Cooker/CCollisionCooker.cpp, which cites the game code each rule comes from).

Triangles become shared vertices and edges; each triangle is its edge triple (GetTriangle rebuilds
corners from the first two edges, both stored forward, or both backward with the flipped flag), with
slot i joining corners i and i + 1 (CCollisionTracker's numbering) and its neighbours across them.
The octree's branches split into octants as the game's BoxFromIndex does; leaves carry their own bounds.
"""
from __future__ import annotations

import math
import struct

FLIPPED = 0x01000000          # kFlippedTriangle (on edges and vertices this bit is kMT_NoEdgeCollision)
STONE, METAL = 1 << 1, 1 << 2
CEILING, WALL, FLOOR = 1 << 29, 1 << 30, 1 << 31
# Surface types (CMaterialList EMaterialTypes bits) offered for new collision.
SURFACES = {"STONE": 1, "METAL": 2, "GRASS": 3, "ICE": 4, "METAL_GRATING": 6, "PHAZON": 7, "DIRT": 8, "LAVA": 9,
            "LAVA_STONE": 10, "SNOW": 11, "MUD_SLOW": 12, "MUD": 14, "GLASS": 15, "SHIELD": 16, "SAND": 17,
            "WOOD": 22, "ORGANIC": 23}
FLAGS = {"SHOOT_THROUGH": 20, "CAMERA_THROUGH": 21, "SEE_THROUGH": 26, "SCAN_THROUGH": 27, "AI_THROUGH": 28}
NO_NEIGHBOR = 0xFFFF


class CollisionError(Exception):
    pass


def material_flags(surface: str, normal, extra=(), floor_cos: float = 0.7) -> int:
    """Raw flags for a triangle: its surface type, extra pass-through flags, and floor, wall or ceiling
    from its normal (z up: floor when the normal's z is at least floor_cos, ceiling when at most -floor_cos)."""
    flags = 1 << SURFACES[surface]
    for f in extra:
        flags |= 1 << FLAGS[f]
    length = math.sqrt(sum(c * c for c in normal)) or 1.0
    z = normal[2] / length
    flags |= FLOOR if z >= floor_cos else CEILING if z <= -floor_cos else WALL
    return flags


class IndexData:
    def __init__(self):
        self.materials = []           # raw u64 flags
        self.vertex_materials = []
        self.edge_materials = []
        self.triangle_materials = []
        self.edges = []               # (v1, v2)
        self.triangles = []           # edge indices, three per triangle
        self.neighbors = []
        self.vertices = []            # (x, y, z)


def build(triangles: list, material_flags_list: list) -> IndexData:
    """triangles: [((x,y,z), (x,y,z), (x,y,z), material index)], counter-clockwise from the front."""
    if len(material_flags_list) > 255:
        raise CollisionError("More than 255 collision materials.")
    d = IndexData()
    vindex, tri_verts = {}, []
    for a, b, c, m in triangles:
        if m >= len(material_flags_list):
            raise CollisionError("A triangle uses a material index past the material list.")
        idx = []
        for p in (a, b, c):
            key = (float(p[0]), float(p[1]), float(p[2]))
            if key not in vindex:
                if len(d.vertices) >= 0xFFFF:
                    raise CollisionError("More than 65535 collision vertices.")
                vindex[key] = len(d.vertices)
                d.vertices.append(key)
            idx.append(vindex[key])
        tri_verts.append(idx)

    eindex = {}

    def edge(f, t):
        key = (min(f, t), max(f, t))
        if key not in eindex:
            if len(d.edges) >= 0xFFFF:
                raise CollisionError("More than 65535 collision edges.")
            eindex[key] = len(d.edges)
            d.edges.append((f, t))
        e = eindex[key]
        return e, d.edges[e][0] == f

    d.materials = [f & ~FLIPPED for f in material_flags_list]
    flipped_variant = {}
    d.vertex_materials = [0xFF] * len(d.vertices)
    edge_mats = []
    for (a, b, c, m), v in zip(triangles, tri_verts):
        if v[0] == v[1] or v[1] == v[2] or v[2] == v[0]:
            raise CollisionError("A collision triangle has two identical corners.")
        es, fw = [], []
        for i in range(3):
            e, f = edge(v[i], v[(i + 1) % 3])
            es.append(e)
            fw.append(f)
        start = 0
        while fw[start] != fw[(start + 1) % 3]:
            start += 1
        flipped = not fw[start]
        d.triangles += [es[(start + i) % 3] for i in range(3)]
        mat = m
        if flipped:
            if m not in flipped_variant:
                if len(d.materials) >= 255:
                    raise CollisionError("More than 255 collision materials once flipped variants are added.")
                flipped_variant[m] = len(d.materials)
                d.materials.append((material_flags_list[m] & ~FLIPPED) | FLIPPED)
            mat = flipped_variant[m]
        if mat > 254:
            raise CollisionError("More than 255 collision materials.")
        d.triangle_materials.append(mat)
        for vi in v:
            if d.vertex_materials[vi] == 0xFF:
                d.vertex_materials[vi] = m
        edge_mats += [0xFF] * (len(d.edges) - len(edge_mats))
        for e in es:
            if edge_mats[e] == 0xFF:
                edge_mats[e] = m
    d.edge_materials = edge_mats
    d.neighbors = neighbors(d)
    return d


def neighbors(d: IndexData) -> list:
    users = [[] for _ in d.edges]
    n = len(d.triangles) // 3
    for t in range(n):
        for i in range(3):
            users[d.triangles[3 * t + i]].append(t)
    out = [NO_NEIGHBOR] * (3 * n)
    for t in range(n):
        for i in range(3):
            for other in users[d.triangles[3 * t + i]]:
                if other != t:
                    out[3 * t + i] = other
                    break
    return out


def _corners(d: IndexData, t: int):
    e0, e1 = d.edges[d.triangles[3 * t]], d.edges[d.triangles[3 * t + 1]]
    if d.materials[d.triangle_materials[t]] & FLIPPED:
        return e0[1], e0[0], e1[0]
    return e0[0], e0[1], e1[1]


def _box6(lo, hi):
    return struct.pack(">6f", *lo, *hi)


def write_octree(d: IndexData, bounds, max_leaf: int = 128, max_depth: int = 6) -> bytes:
    """The collision block CAreaOctTree::MakeFromMemory reads, from magic 0xDEAFBABE on."""
    n = len(d.triangles) // 3
    if n == 0 or len(d.triangle_materials) != n:
        raise CollisionError("The collision index data is empty or its arrays disagree in length.")
    if n > 0xFFFF:
        raise CollisionError("More than 65535 collision triangles.")
    tri_boxes = []
    for t in range(n):
        pts = [d.vertices[v] for v in _corners(d, t)]
        tri_boxes.append(([min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]))

    def overlaps(a, b):
        return all(a[0][i] <= b[1][i] and a[1][i] >= b[0][i] for i in range(3))

    def child_box(box, i):
        lo, hi = box
        c = [(lo[k] + hi[k]) * 0.5 for k in range(3)]
        mn, mx = list(lo), list(c)
        for k in range(3):
            if i & (1 << k):
                mn[k], mx[k] = c[k], hi[k]
        return (mn, mx)

    def leaf(tris):
        lo = [min(tri_boxes[t][0][k] for t in tris) for k in range(3)]
        hi = [max(tri_boxes[t][1][k] for t in tris) for k in range(3)]
        out = _box6(lo, hi) + struct.pack(">H", len(tris)) + struct.pack(">%dH" % len(tris), *tris)
        if len(tris) % 2 == 0:
            out += b"\0\0"
        return out

    def node(box, tris, depth):
        children = []
        splits = False
        if len(tris) > max_leaf and depth < max_depth:
            for i in range(8):
                cb = child_box(box, i)
                s = [t for t in tris if overlaps(tri_boxes[t], cb)]
                children.append(s)
                splits |= len(s) < len(tris)
        if len(tris) <= max_leaf or depth >= max_depth or not splits:
            if len(tris) > 0xFFFF:
                raise CollisionError("An octree leaf would hold more than 65535 triangles.")
            return 2, leaf(tris)
        body, offsets, types = bytearray(), [0] * 8, 0
        for i in range(8):
            if not children[i]:
                continue
            offsets[i] = len(body)
            ctype, data = node(child_box(box, i), children[i], depth + 1)
            body += data
            types |= ctype << (2 * i)
        return 1, struct.pack(">HH8I", types, 0, *offsets) + bytes(body)

    root_type, tree = node(bounds, list(range(n)), 0)
    out = bytearray(struct.pack(">II", 0xDEAFBABE, 4) + _box6(*bounds) + struct.pack(">II", root_type, len(tree)) + tree)
    out += struct.pack(">I", len(d.materials)) + b"".join(struct.pack(">Q", m) for m in d.materials)
    for arr in (d.vertex_materials, d.edge_materials, d.triangle_materials):
        out += struct.pack(">I", len(arr)) + bytes(arr)
    out += struct.pack(">I", len(d.edges)) + b"".join(struct.pack(">HH", *e) for e in d.edges)
    out += struct.pack(">I", len(d.triangles)) + struct.pack(">%dH" % len(d.triangles), *d.triangles)
    nb = d.neighbors if len(d.neighbors) == len(d.triangles) else neighbors(d)
    out += struct.pack(">I", len(nb)) + struct.pack(">%dH" % len(nb), *nb)
    out += struct.pack(">I", len(d.vertices)) + b"".join(struct.pack(">3f", *v) for v in d.vertices)
    return bytes(out)


def write_section(d: IndexData, max_leaf: int = 128, max_depth: int = 6) -> bytes:
    """A whole MREA collision section (a word PostConstructArea steps over, the block's size, the
    block), padded to 32 bytes."""
    lo = [min(v[k] for v in d.vertices) for k in range(3)]
    hi = [max(v[k] for v in d.vertices) for k in range(3)]
    block = write_octree(d, (lo, hi), max_leaf, max_depth)
    out = bytearray(struct.pack(">II", 0, len(block)) + block)
    out += b"\0" * (-len(out) % 32)
    return bytes(out)
