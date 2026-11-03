"""Echoes world materials: read a cooked material set, recombine materials from several rooms, and
write new ones from templates.

Layout, as the game reads it (MetroidPrime2Port src/Kyoto/Graphics/CCubeMaterial.cpp SetCurrent,
HandleNormalTevs, HandleTev; src/Kyoto/Graphics/CGX.cpp for the packed words): flags; texture count and
indices into the set's texture list; the compressed vertex description; two Echoes words; a group index
(materials with the same group skip re-setting state, so every material here gets its own); konst
colours if flags bit 3; blend factors (source in the low half, destination in the high); an indirect
texture slot if flags bit 10; colour channels (count, then that many words); TEV stages (count, five
words each: colour inputs, alpha inputs, colour op, alpha op, channel | kcolor << 8 | kalpha << 16), one
texmap << 8 | texcoord word per stage; texgens (count, words); UV animations (byte size, count, data).
A set is the texture count and IDs, the material count and each material's end offset, the materials
(CMetroidModelInstance::GetMaterialByIndex).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Optional

# CCubeMaterial flags (include/Kyoto/Graphics/CCubeMaterial.hpp)
KONST_VALUES = 1 << 3
DEPTH_SORTING = 1 << 4
ALPHA_TEST = 1 << 5
REFLECTION = 1 << 6
DEPTH_WRITE = 1 << 7
INDIRECT_TEXTURE = 1 << 10
LIGHTMAP = 1 << 11
LIGHTMAP_UV_ARRAY = 1 << 13
ECHOES_BASE_FLAGS = 0x4002  # what Prime World Editor's material cooker writes for every Echoes material

# GX TEV colour and alpha arguments, registers, channels, texgen sources
CC_CPREV, CC_C0, CC_C1, CC_C2, CC_TEXC, CC_TEXA, CC_RASC, CC_ONE, CC_KONST, CC_ZERO = 0, 2, 4, 6, 8, 9, 10, 12, 14, 15
CA_APREV, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO = 0, 4, 5, 6, 7
REG_PREV, REG_0, REG_1, REG_2 = 0, 1, 2, 3
COLOR0A0, COLOR_NULL = 4, 0xFF
TG_TEX0 = 4  # GX_TG_TEX0; TEXn is 4 + n
# Blend factors (GXBlendFactor)
BL_ZERO, BL_ONE, BL_SRCALPHA, BL_INVSRCALPHA = 0, 1, 4, 5

TEMPLATES = {
    "LIT": "Lit: texture x (ambient + lights)",
    "LIGHTMAPPED": "Lightmapped: texture x (lightmap + ambient + lights), as retail rooms",
    "CUTOUT": "Cutout: lit, alpha-tested (texture alpha below 64 is a hole)",
    "UNLIT": "Unlit: texture as is (fullbright)",
    "ADDITIVE": "Additive glow: texture added over what is behind it",
}


class MaterialError(Exception):
    pass


def _u32(b: bytes, at: int) -> int:
    return struct.unpack_from(">I", b, at)[0]


def args(a: int, b: int, c: int, d: int) -> int:
    """TEV inputs as CGX packs them: out = d + lerp(a, b, c)."""
    return a | b << 5 | c << 10 | d << 15


def op(out_reg: int) -> int:
    """GX_TEV_ADD, no bias, scale 1, clamp, to out_reg (CGX's fast path, 0x100 | reg << 9)."""
    return 0x100 | out_reg << 9


def texgen(source_attr: int) -> int:
    """GX_TG_MTX2x4 from vertex attribute TEXn, identity texture and post matrices
    (CGX::SetTexCoordGen_Compressed: type, source << 4, matrix << 9, normalise << 14, post << 15)."""
    return 1 | (TG_TEX0 + source_attr) << 4 | 30 << 9 | 61 << 15


@dataclass
class Stage:
    color_in: int
    alpha_in: int
    color_op: int
    alpha_op: int
    mat_word: int
    texmap: int
    texcoord: int

    def color_args(self):
        return [self.color_in >> s & 31 for s in (0, 5, 10, 15)]


@dataclass
class Material:
    flags: int
    textures: list            # indices into the set's texture list, in GX texmap order
    vertex_desc: int
    echoes_a: int = 0
    echoes_b: int = 0
    group: int = 0
    konst: list = field(default_factory=list)
    blend: int = BL_ONE
    indirect_slot: Optional[int] = None
    channels: list = field(default_factory=lambda: [0x3001])
    stages: list = field(default_factory=list)
    texgens: list = field(default_factory=list)
    anims: bytes = b"\0\0\0\0"   # the count word and animation data

    def to_bytes(self) -> bytes:
        w = [self.flags | (KONST_VALUES if self.konst else 0), len(self.textures), *self.textures, self.vertex_desc,
             self.echoes_a, self.echoes_b, self.group]
        if self.konst:
            w += [len(self.konst), *self.konst]
        w.append(self.blend)
        if self.flags & INDIRECT_TEXTURE:
            w.append(self.indirect_slot or 0)
        w += [len(self.channels), *self.channels, len(self.stages)]
        for s in self.stages:
            w += [s.color_in, s.alpha_in, s.color_op, s.alpha_op, s.mat_word]
        w += [s.texmap << 8 | s.texcoord for s in self.stages]
        w += [len(self.texgens), *self.texgens, len(self.anims)]
        return struct.pack(">%dI" % len(w), *w) + self.anims

    # -- what each texture does, for previews and for which UV map feeds which attribute
    def texcoord_source(self, texcoord: int) -> Optional[int]:
        """The vertex attribute (TEXn) a texcoord reads, or None (positions, normals: no UVs)."""
        if texcoord >= len(self.texgens):
            return None
        src = self.texgens[texcoord] >> 4 & 31
        return src - TG_TEX0 if TG_TEX0 <= src < TG_TEX0 + 8 else None

    def roles(self) -> dict:
        """{'lightmap': slot, 'diffuse': slot, 'glow': slot} where found, slot = texmap (index into
        self.textures). The lightmap stage writes RASC + register x TEXC to a register, the diffuse
        stage is 0 + lerp(0, L, TEXC) with L the lit colour or a register (the rule the port's
        PortPbrMaps::ChooseStage applies to retail materials), a glow stage adds TEXC onto CPREV."""
        out = {}
        for i, s in enumerate(self.stages):
            a, b, c, d = s.color_args()
            if s.texmap == 0xFF:
                continue
            reg = s.color_op >> 9 & 3
            if "lightmap" not in out and d == CC_RASC and a == CC_ZERO and c == CC_TEXC and reg != REG_PREV:
                out["lightmap"] = s.texmap
            elif "diffuse" not in out and a == CC_ZERO and d == CC_ZERO and CC_TEXC in (b, c):
                other = c if b == CC_TEXC else b
                if other in (CC_RASC, CC_C0, CC_C1, CC_C2, CC_ONE):
                    out["diffuse"] = s.texmap
            elif "diffuse" in out and "glow" not in out and d == CC_CPREV and CC_TEXC in (a, b, c):
                out["glow"] = s.texmap
        if "diffuse" not in out:
            for s in self.stages:
                if s.texmap != 0xFF and CC_TEXC in s.color_args() and s.texmap != out.get("lightmap"):
                    out["diffuse"] = s.texmap
                    break
        return out

    def lightmap_attr(self) -> Optional[int]:
        """The TEXn the lightmap stage samples through, if the material has one."""
        lm = self.roles().get("lightmap")
        for s in self.stages:
            if s.texmap == lm and lm is not None:
                return self.texcoord_source(s.texcoord)
        return None

    def tex_attrs(self) -> list:
        """Every TEXn (0-6) the vertex description has an index for."""
        return [n for n in range(7) if self.vertex_desc >> (8 + 2 * n) & 3]


def parse_material(b: bytes) -> Material:
    """One material from its bytes (all of them: the length must match what the game reads)."""
    try:
        p = 0

        def take(n=1):
            nonlocal p
            v = struct.unpack_from(">%dI" % n, b, p)
            p += 4 * n
            return list(v)
        flags, tex_count = take(2)
        textures = take(tex_count)
        vertex_desc, echoes_a, echoes_b, group = take(4)
        konst = take(take()[0]) if flags & KONST_VALUES else []
        blend = take()[0]
        indirect = take()[0] if flags & INDIRECT_TEXTURE else None
        channels = take(take()[0])
        stage_count = take()[0]
        raw = [take(5) for _ in range(stage_count)]
        orders = take(stage_count)
        texgens = take(take()[0])
        anim_size = take()[0]
        anims = b[p:p + anim_size]
        p += anim_size
    except struct.error:
        raise MaterialError("material data is cut short") from None
    if p != len(b):
        raise MaterialError(f"material is {len(b)} bytes but its layout reads {p}")
    stages = [Stage(r[0], r[1], r[2], r[3], r[4], o >> 8 & 0xFF, o & 0xFF) for r, o in zip(raw, orders)]
    return Material(flags & ~KONST_VALUES, textures, vertex_desc, echoes_a, echoes_b, group, konst, blend, indirect,
                    channels, stages, texgens, anims)


@dataclass
class MaterialSet:
    texture_ids: list
    materials: list   # raw bytes per material

    def parsed(self, i: int) -> Material:
        return parse_material(self.materials[i])

    def to_bytes(self) -> bytes:
        out = bytearray(struct.pack(">I", len(self.texture_ids)))
        out += struct.pack(">%dI" % len(self.texture_ids), *self.texture_ids)
        out += struct.pack(">I", len(self.materials))
        end = 0
        for m in self.materials:
            end += len(m)
            out += struct.pack(">I", end)
        for m in self.materials:
            out += m
        return bytes(out)


def parse_set(b: bytes) -> MaterialSet:
    tex = _u32(b, 0)
    ids = list(struct.unpack_from(">%dI" % tex, b, 4))
    at = 4 + 4 * tex
    count = _u32(b, at)
    ends = struct.unpack_from(">%dI" % count, b, at + 4)
    base = at + 4 + 4 * count
    mats, start = [], 0
    for e in ends:
        if e < start or base + e > len(b):
            raise MaterialError("material offsets run past the set")
        mats.append(bytes(b[base + start:base + e]))
        start = e
    return MaterialSet(ids, mats)


def retexture(material: bytes, texture_ids: list, set_ids: list, group: int) -> bytes:
    """A material's bytes with its texture indices pointing into another set's list (set_ids, extended
    in place with any ID it lacks) and a new group index. texture_ids are the IDs it used, in order."""
    m = parse_material(material)
    if len(texture_ids) != len(m.textures):
        raise MaterialError("texture ID list does not match the material's texture count")
    new = []
    for tid in texture_ids:
        if tid not in set_ids:
            set_ids.append(tid)
        new.append(set_ids.index(tid))
    m.textures = new
    m.group = group
    return m.to_bytes()


def template(kind: str, group: int = 0) -> tuple:
    """(Material, texture roles in texmap order) for a built-in template; the texture indices are
    placeholders (0, 1, ...) until the set is assembled.

    Every template draws with 16-bit indices for POS, NRM and its UVs, and one lit colour channel
    (0x3001: lighting on, register ambient and material colours, clamped diffuse, spot attenuation,
    the engine filling in the light mask) except the unlit ones. Alpha comes from the texture."""
    idx16 = 3
    pos_nrm = idx16 | idx16 << 2
    if kind == "LIGHTMAPPED":
        # Retail room materials: stage 0 writes RASC + C1 x lightmap to C0 (C1 is the renderer's world
        # light level), stage 1 is C0 x diffuse (the stage PortPbrMaps::ChooseStage relights).
        stages = [Stage(args(CC_ZERO, CC_C1, CC_TEXC, CC_RASC), args(CA_ZERO, CA_ZERO, CA_ZERO, CA_ZERO), op(REG_0),
                        op(REG_0), COLOR0A0, 0, 0),
                  Stage(args(CC_ZERO, CC_C0, CC_TEXC, CC_ZERO), args(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA), op(REG_PREV),
                        op(REG_PREV), COLOR_NULL, 1, 1)]
        return (Material(ECHOES_BASE_FLAGS | DEPTH_WRITE, [0, 1], pos_nrm | idx16 << 8 | idx16 << 10, group=group,
                         stages=stages, texgens=[texgen(0), texgen(1)]), ["lightmap", "diffuse"])
    if kind in ("LIT", "CUTOUT"):
        stages = [Stage(args(CC_ZERO, CC_RASC, CC_TEXC, CC_ZERO), args(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA), op(REG_PREV),
                        op(REG_PREV), COLOR0A0, 0, 0)]
        flags = ECHOES_BASE_FLAGS | DEPTH_WRITE | (ALPHA_TEST if kind == "CUTOUT" else 0)
        return (Material(flags, [0], pos_nrm | idx16 << 8, group=group, stages=stages, texgens=[texgen(0)]), ["diffuse"])
    if kind in ("UNLIT", "ADDITIVE"):
        stages = [Stage(args(CC_ZERO, CC_ZERO, CC_ZERO, CC_TEXC), args(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA), op(REG_PREV),
                        op(REG_PREV), COLOR_NULL, 0, 0)]
        if kind == "UNLIT":
            return (Material(ECHOES_BASE_FLAGS | DEPTH_WRITE, [0], pos_nrm | idx16 << 8, group=group, channels=[],
                             stages=stages, texgens=[texgen(0)]), ["diffuse"])
        return (Material(ECHOES_BASE_FLAGS | DEPTH_SORTING, [0], pos_nrm | idx16 << 8, group=group, channels=[],
                         blend=BL_ONE << 16 | BL_ONE, stages=stages, texgens=[texgen(0)]), ["glow"])
    raise MaterialError(f"unknown template {kind!r}; templates: {', '.join(TEMPLATES)}")
