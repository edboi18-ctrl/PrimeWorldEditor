"""Split an Echoes room (MREA version 0x19) into sections, and splice new geometry and collision in.

The header is 0xDEADBEEF, the version, the area transform (12 floats), world model count (word 14),
script layer count (15), section count (16), section numbers (17 geometry, 18 first script layer,
19 generated objects, 20 collision, 21 BSP, 22 lights, 23 visibility, 24 PATH, 25 the word the game
reads as the render octree's (not -1), 26 PTLA, 27 EGMC) and the block count (28), padded to 0x80; then
the section sizes and the block table (buffer size, decompressed size, compressed size, section count),
each padded to 32; then the blocks. A compressed size of 0 is a stored block (CGameArea reads it as is).
Splicing follows Prime World Editor's CAreaCooker: later sections are renumbered, new geometry clears
the visibility data (64 bytes or less is ignored) and the static geometry map (ID -1), which index the
old surfaces.
"""
from __future__ import annotations

import struct

from . import echoes_disc

BLOCK_LIMIT = 0x20000   # CAreaCooker's block size threshold


class MreaError(Exception):
    pass


def split(mrea: bytes):
    """(header words [32], sections [bytes])."""
    if len(mrea) < 0x80:
        raise MreaError("not an MREA (too short)")
    h = list(struct.unpack_from(">32I", mrea, 0))
    if h[0] != 0xDEADBEEF or h[1] != 0x19:
        raise MreaError(f"not an Echoes MREA (magic 0x{h[0]:08X}, version 0x{h[1]:X})")
    count, blocks = h[16], h[28]
    pos = 0x80
    sizes = struct.unpack_from(">%dI" % count, mrea, pos)
    pos += (count * 4 + 31) & ~31
    table = [struct.unpack_from(">4I", mrea, pos + 16 * i) for i in range(blocks)]
    pos += (blocks * 16 + 31) & ~31
    sections, si = [], 0
    for buf, dl, cl, sc in table:
        rd = ((cl or buf) + 31) & ~31
        raw = mrea[pos:pos + rd]
        data = echoes_disc.inflate_chunks(raw[rd - cl:], dl) if cl else raw[:dl]
        off = 0
        for i in range(si, si + sc):
            sections.append(bytes(data[off:off + sizes[i]]))
            off += sizes[i]
        pos += rd
        si += sc
    if len(sections) != count:
        raise MreaError("the block table does not cover every section")
    return h, sections


def assemble(header: list, sections: list) -> bytes:
    """An MREA of stored blocks: each script layer and the generated objects in a block of its own,
    other sections grouped up to BLOCK_LIMIT bytes."""
    h = list(header)
    sections = list(sections)
    h[16] = len(sections)
    single = set(range(h[18], h[18] + h[15])) | {h[19]}
    blocks, cur = [], []

    def flush():
        if cur:
            blocks.append(list(cur))
            cur.clear()
    for i, s in enumerate(sections):
        if len(s) % 32:
            s = s + b"\0" * (-len(s) % 32)
            sections[i] = s
        if i in single:
            flush()
            blocks.append([i])
            continue
        if cur and sum(len(sections[j]) for j in cur) + len(s) > BLOCK_LIMIT:
            flush()
        cur.append(i)
    flush()
    h[28] = len(blocks)
    out = bytearray(struct.pack(">29I", *h[:29]))
    out += b"\0" * (0x80 - len(out))
    out += struct.pack(">%dI" % len(sections), *map(len, sections))
    out += b"\0" * (-len(out) % 32)
    for b in blocks:
        size = sum(len(sections[j]) for j in b)
        out += struct.pack(">4I", size, size, 0, len(b))
    out += b"\0" * (-len(out) % 32)
    for b in blocks:
        for j in b:
            out += sections[j]
    return bytes(out)


def _asset_id_section(value: int) -> bytes:
    return struct.pack(">I", value) + b"\0" * 28


def splice(mrea: bytes, geometry: list = None, model_count: int = None, collision: bytes = None,
           clear_path: bool = False) -> bytes:
    """The room with its geometry sections (materials through the ambient light table) and/or its
    collision section replaced."""
    h, s = split(mrea)
    first_geometry, first_layer = h[17], h[18]
    if first_layer < first_geometry or h[27] >= len(s):
        raise MreaError("the section numbers do not match the sections")
    if geometry is not None:
        if model_count is None:
            raise MreaError("new geometry needs its world model count")
        delta = len(geometry) - (first_layer - first_geometry)
        s = s[:first_geometry] + list(geometry) + s[first_layer:]
        for w in range(18, 28):
            if h[w] != 0xFFFFFFFF and w != 25 and h[w] >= first_layer:
                h[w] += delta
        h[25] = h[18] - 1
        h[14] = model_count
        s[h[23]] = b"\0" * 32                 # visibility: ignored at 64 bytes or less
        s[h[27]] = _asset_id_section(0xFFFFFFFF)   # static geometry map: not loaded
    if collision is not None:
        s[h[20]] = collision
    if clear_path:
        s[h[24]] = _asset_id_section(0xFFFFFFFF)
    return assemble(h, s)
