"""Build a room from meshes and materials (no Blender needed): the cooked MREA, its new textures, and
where each goes in the port's mods folder.

Inputs are plain data so the Blender operator, tests and scripts share one path:
- MeshInput: triangles with per-corner position, normal, UV (the diffuse map), lightmap UV and colour,
  in game space (z up, units in metres, counter-clockwise front faces, as Blender), a material per
  triangle, and whether the mesh is drawn, collides, or both.
- MaterialInput: a built-in template (materials.TEMPLATES) with its images, or a material read off the
  disc (its bytes and texture IDs) with any texture slots replaced by images; plus its collision.
UVs follow Blender (v up from the bottom); textures and UVs are flipped to GX's v-down on the way out.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

from . import collision as col
from . import geometry as geo
from . import materials as mats
from . import mrea as mrea_mod
from . import txtr

ROLES = ("BOTH", "VISUAL", "COLLISION")


class BuildError(Exception):
    pass


@dataclass
class ImageInput:
    name: str
    rgba: np.ndarray        # (h, w, 4) uint8, bottom row first (as Blender stores pixels)


@dataclass
class CollisionInput:
    enabled: bool = True
    surface: str = "STONE"
    flags: tuple = ()


@dataclass
class MaterialInput:
    name: str
    kind: str = "TEMPLATE"                  # TEMPLATE or DISC
    template: str = "LIT"
    images: dict = field(default_factory=dict)          # template role -> ImageInput ('diffuse', 'lightmap')
    disc_material: bytes = b""
    disc_texture_ids: list = field(default_factory=list)
    overrides: dict = field(default_factory=dict)       # disc texture slot -> ImageInput
    collision: CollisionInput = field(default_factory=CollisionInput)


@dataclass
class Corner:
    position: tuple
    normal: tuple = (0.0, 0.0, 1.0)
    uv: tuple = (0.0, 0.0)
    lightmap_uv: tuple = (0.0, 0.0)
    color: int = 0xFFFFFFFF


@dataclass
class MeshInput:
    name: str
    triangles: list            # [(Corner, Corner, Corner)]
    materials: list            # per triangle: index into the build's materials
    role: str = "BOTH"


@dataclass
class BuildInput:
    base_mrea: bytes           # the room being replaced, from the disc
    mrea_id: int
    pak: str                   # the disc PAK new textures join (the room's own)
    meshes: list
    materials: list
    disc_ids: set = field(default_factory=set)   # every asset ID on the disc (new IDs avoid them)
    clear_path: bool = False
    area_bounds: tuple = None  # the room's bounds in the world (MLVL), to warn about geometry outside them


@dataclass
class BuildOutput:
    files: dict                # path inside the mod folder -> bytes
    report: dict


def _new_texture_id(image: ImageInput, taken: set) -> int:
    tid = zlib.crc32(image.name.encode("utf-8") + image.rgba.tobytes()) & 0xFFFFFFFF
    while tid in taken or tid in (0, 0xFFFFFFFF):
        tid = (tid + 1) & 0xFFFFFFFF
    taken.add(tid)
    return tid


def _gx_v(uv):
    return (float(uv[0]), 1.0 - float(uv[1]))


def _face_normal(tri):
    a, b, c = (t.position for t in tri)
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    return (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])


def build(inp: BuildInput) -> BuildOutput:
    report = {"warnings": [], "new_textures": [], "materials": []}
    files = {}
    taken = set(inp.disc_ids)
    image_ids = {}            # (name, shape, pixel CRC) -> texture ID: one texture per distinct image

    def texture_for(image: ImageInput) -> int:
        pixels = np.asarray(image.rgba, dtype=np.uint8)
        key = (image.name, pixels.shape, zlib.crc32(pixels.tobytes()))
        if key not in image_ids:
            rgba = np.ascontiguousarray(np.asarray(image.rgba, dtype=np.uint8)[::-1])   # top row first for GX
            try:
                data = txtr.encode_rgba8(rgba)
            except txtr.TextureError as e:
                raise BuildError(f"image {image.name!r}: {e}") from None
            tid = _new_texture_id(image, taken)
            image_ids[key] = tid
            files[f"{inp.pak}/{tid:08X}.TXTR"] = data
            report["new_textures"].append({"image": image.name, "id": f"{tid:08X}", "size": list(rgba.shape[1::-1])})
        return image_ids[key]

    # Materials: one set, every material its own group, texture indices into the shared list.
    set_ids, set_mats, routing = [], [], []
    for i, m in enumerate(inp.materials):
        if m.kind == "TEMPLATE":
            mat, roles = mats.template(m.template, group=i)
            ids = []
            for role in roles:
                image = m.images.get(role) or (m.images.get("diffuse") if role == "glow" else None)
                if image is None:
                    raise BuildError(f"material {m.name!r} ({m.template}) needs a {role} image: set it in Material "
                                     "Properties > Echoes Material (or hide the objects that use it)")
                ids.append(texture_for(image))
            body = mats.retexture(mat.to_bytes(), ids, set_ids, i)
        elif m.kind == "DISC":
            if not m.disc_material:
                raise BuildError(f"material {m.name!r} has no disc material data")
            ids = list(m.disc_texture_ids)
            for slot, image in m.overrides.items():
                if not 0 <= slot < len(ids):
                    raise BuildError(f"material {m.name!r}: texture slot {slot} does not exist")
                ids[slot] = texture_for(image)
            body = mats.retexture(m.disc_material, ids, set_ids, i)
        else:
            raise BuildError(f"material {m.name!r}: kind {m.kind!r} is neither TEMPLATE nor DISC")
        info = mats.parse_material(body)
        lightmap_attr = info.lightmap_attr()
        packed = bool(info.flags & mats.LIGHTMAP_UV_ARRAY)
        routing.append((lightmap_attr, packed, info.tex_attrs()))
        set_mats.append(body)
        report["materials"].append({"name": m.name, "kind": m.kind, "template": m.template if m.kind == "TEMPLATE" else None,
                                    "textures": [f"{t:08X}" for t in ids]})
    material_set = mats.MaterialSet(set_ids, set_mats).to_bytes()

    # World models: one per drawn mesh, split where an array would pass 65536 entries.
    models = []
    for mesh in inp.meshes:
        if mesh.role not in ROLES:
            raise BuildError(f"mesh {mesh.name!r}: role {mesh.role!r} is not one of {ROLES}")
        if mesh.role == "COLLISION" or not mesh.triangles:
            continue
        if len(mesh.materials) != len(mesh.triangles):
            raise BuildError(f"mesh {mesh.name!r}: one material per triangle")
        model, maps, surfaces = None, None, None

        def start():
            nonlocal model, maps, surfaces
            model = geo.Model()
            maps = ({}, {}, {}, {}, {})   # positions, normals, colours, uvs, lightmap uvs
            surfaces = {}

        def finish():
            model.surfaces = [geo.Surface(mi, corners) for mi, corners in sorted(surfaces.items())]
            if model.surfaces:
                models.append(model)

        def index(which, value, array):
            table = maps[which]
            if value not in table:
                table[value] = len(array)
                array.append(value)
            return table[value]

        start()
        for tri, mi in zip(mesh.triangles, mesh.materials):
            if not 0 <= mi < len(inp.materials):
                raise BuildError(f"mesh {mesh.name!r}: material index {mi} is past the material list")
            if max(len(model.positions), len(model.normals), len(model.texcoords), len(model.lightmap_texcoords)) > 0x10000 - 6:
                finish()
                start()
            lightmap_attr, packed, attrs = routing[mi]
            corners = surfaces.setdefault(mi, [])
            for c in tri:
                p = index(0, tuple(map(float, c.position)), model.positions)
                n = index(1, tuple(map(float, c.normal)), model.normals)
                k = index(2, int(c.color), model.colors)
                tex = [0] * 7
                for attr in attrs:
                    if attr == lightmap_attr:
                        uv = _gx_v(c.lightmap_uv)
                        if packed and attr == 0:
                            tex[attr] = index(4, uv, model.lightmap_texcoords)
                        else:
                            tex[attr] = index(3, uv, model.texcoords)
                    else:
                        tex[attr] = index(3, _gx_v(c.uv), model.texcoords)
                corners.append(geo.Corner(p, n, k, tuple(tex)))
        finish()
    if not models:
        raise BuildError("No drawn geometry: give at least one mesh the role BOTH or VISUAL.")
    try:
        geometry = geo.write(material_set, models)
    except geo.GeometryError as e:
        raise BuildError(str(e)) from None

    # Collision: every triangle of a colliding mesh whose material collides.
    col_tris, col_flags, flag_index = [], [], {}
    for mesh in inp.meshes:
        if mesh.role == "VISUAL":
            continue
        for tri, mi in zip(mesh.triangles, mesh.materials):
            c = inp.materials[mi].collision
            if not c.enabled:
                continue
            flags = col.material_flags(c.surface, _face_normal(tri), c.flags)
            if flags not in flag_index:
                flag_index[flags] = len(col_flags)
                col_flags.append(flags)
            col_tris.append((*(t.position for t in tri), flag_index[flags]))
    collision_section = None
    if col_tris:
        try:
            collision_section = col.write_section(col.build(col_tris, col_flags))
        except col.CollisionError as e:
            raise BuildError(str(e)) from None
    else:
        report["warnings"].append("No collision triangles: the room keeps its old collision.")

    try:
        room = mrea_mod.splice(inp.base_mrea, geometry.sections, geometry.model_count, collision_section, inp.clear_path)
    except mrea_mod.MreaError as e:
        raise BuildError(f"base room: {e}") from None
    files[f"{inp.mrea_id:08X}.MREA"] = room

    if inp.area_bounds is not None:
        lo, hi = geometry.bounds
        alo, ahi = inp.area_bounds
        if any(lo[i] < alo[i] - 0.01 or hi[i] > ahi[i] + 0.01 for i in range(3)):
            report["warnings"].append(
                f"The geometry ({[round(v, 2) for v in lo]} to {[round(v, 2) for v in hi]}) reaches outside the room's "
                f"bounds in its world ({[round(v, 2) for v in alo]} to {[round(v, 2) for v in ahi]}); the game uses those "
                "for culling, so parts outside may vanish.")
    report.update(world_models=geometry.model_count, area_surfaces=geometry.area_surface_count,
                  collision_triangles=len(col_tris), bounds=[list(geometry.bounds[0]), list(geometry.bounds[1])],
                  mrea=f"{inp.mrea_id:08X}", mrea_bytes=len(room))
    return BuildOutput(files, report)


def area_bounds_from_mlvl(mlvl: bytes, area_index: int):
    """An area's bounds in its world: the box after its 48-byte transform (CWorld's area header, the
    48 + 24 bytes echoes_disc.parse_mlvl skips)."""
    from . import echoes_disc
    r = echoes_disc.Rd(mlvl)
    r.u32()
    ver = r.u32()
    r.u32()
    if ver >= 22:
        r.u32()
    if ver >= 23:
        r.u32()
    r.u32()
    r.u32()
    n = r.u32()
    for i in range(n):
        r.u32()
        r.skip(48)
        box = struct.unpack_from(">6f", mlvl, r.p)
        if i == area_index:
            return (box[:3], box[3:])
        r.skip(24)
        r.u32()
        r.u32()
        r.skip(2 * r.u32())
        r.skip(8 * r.u32())
        r.skip(8 * r.u32())
        r.skip(4 * r.u32())
        for _ in range(r.u32()):
            r.skip(8 * r.u32())
            r.skip(12 * r.u32())
        for _ in range(r.u32()):
            r.cstr()
        r.skip(4 * r.u32())
        r.cstr()
    return None
