"""Echoes textures (TXTR): decode for previews, encode RGBA8 for new textures. numpy only (Blender ships it).

A TXTR is a u32 texel format, u16 width, u16 height, u32 mip count, a palette for C4/C8 (u32 format,
two u16s whose product is the entry count, the entries), then each mip level in GX tiles. Formats and
layouts follow the game's loader (MetroidPrime2Port src/Kyoto/Graphics/DolphinCTexture.cpp,
include/Kyoto/Graphics/CTexture.hpp ETexelFormat, DolphinCPalette.cpp) and GX's texel tiling, as
Prime World Editor's CTextureDecoder reads it.
"""
from __future__ import annotations

import struct

import numpy as np

I4, I8, IA4, IA8, C4, C8, C14X2, RGB565, RGB5A3, RGBA8, CMPR = range(11)
NAMES = ["I4", "I8", "IA4", "IA8", "C4", "C8", "C14X2", "RGB565", "RGB5A3", "RGBA8", "CMPR"]
# Tile size in texels and bits per texel (GX).
BLOCK = {I4: (8, 8), I8: (8, 4), IA4: (8, 4), IA8: (4, 4), C4: (8, 8), C8: (8, 4), C14X2: (4, 4),
         RGB565: (4, 4), RGB5A3: (4, 4), RGBA8: (4, 4), CMPR: (8, 8)}
BPP = {I4: 4, I8: 8, IA4: 8, IA8: 16, C4: 4, C8: 8, C14X2: 16, RGB565: 16, RGB5A3: 16, RGBA8: 32, CMPR: 4}


class TextureError(Exception):
    pass


def _ext4(v):
    return (v << 4) | v


def _ext5(v):
    return (v << 3) | (v >> 2)


def _ext6(v):
    return (v << 2) | (v >> 4)


def _ext3(v):
    return (v << 5) | (v << 2) | (v >> 1)


def _rgb565(v):
    v = v.astype(np.uint32)
    return np.stack([_ext5((v >> 11) & 31), _ext6((v >> 5) & 63), _ext5(v & 31), np.full_like(v, 255)], -1)


def _rgb5a3(v):
    v = v.astype(np.uint32)
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, _ext5((v >> 10) & 31), _ext4((v >> 8) & 15))
    g = np.where(opaque, _ext5((v >> 5) & 31), _ext4((v >> 4) & 15))
    b = np.where(opaque, _ext5(v & 31), _ext4(v & 15))
    a = np.where(opaque, 255, _ext3((v >> 12) & 7))
    return np.stack([r, g, b, a], -1)


def _ia8(v):
    v = v.astype(np.uint32)
    lum, alpha = v & 0xFF, (v >> 8) & 0xFF
    return np.stack([lum, lum, lum, alpha], -1)


def _palette_colors(fmt, entries):
    return {0: _ia8, 1: _rgb565, 2: _rgb5a3}[fmt](entries)


def _untile(values, width, height, bw, bh):
    """Tile-ordered texel values (one per texel) to an (h, w, ...) array."""
    tw, th = -(-width // bw), -(-height // bh)
    arr = values[: tw * th * bw * bh].reshape((th, tw, bh, bw) + values.shape[1:])
    arr = arr.swapaxes(1, 2).reshape((th * bh, tw * bw) + values.shape[1:])
    return arr[:height, :width]


def _cmpr(data, width, height):
    tw, th = -(-width // 8), -(-height // 8)
    blocks = np.frombuffer(data[: tw * th * 32], dtype=np.uint8).reshape(th, tw, 2, 2, 8)
    c0 = blocks[..., 0].astype(np.uint32) << 8 | blocks[..., 1]
    c1 = blocks[..., 2].astype(np.uint32) << 8 | blocks[..., 3]
    p0, p1 = _rgb565(c0).astype(np.int32), _rgb565(c1).astype(np.int32)
    four = (c0 > c1)[..., None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], -2)  # th, tw, 2, 2, 4 colours, rgba
    rows = blocks[..., 4:8]                    # th, tw, 2, 2, 4 rows
    idx = np.stack([(rows >> s) & 3 for s in (6, 4, 2, 0)], -1)  # leftmost texel in the top bits
    out = np.take_along_axis(palette, idx.reshape(th, tw, 2, 2, 16, 1).repeat(4, -1), axis=-2)
    out = out.reshape(th, tw, 2, 2, 4, 4, 4)            # sub-block row, col; texel row, col; rgba
    out = out.transpose(0, 2, 4, 1, 3, 5, 6).reshape(th * 8, tw * 8, 4)
    return out[:height, :width]


def read_header(data: bytes):
    fmt, width, height, mips = struct.unpack_from(">IHHI", data, 0)
    return fmt, width, height, mips


def decode(data: bytes) -> np.ndarray:
    """The top mip level as an (height, width, 4) uint8 RGBA array, top row first."""
    if len(data) < 12:
        raise TextureError("not a TXTR (too short)")
    fmt, width, height, _ = read_header(data)
    if fmt not in BLOCK or width == 0 or height == 0:
        raise TextureError(f"not a TXTR (format {fmt}, {width}x{height})")
    pos, palette = 12, None
    if fmt in (C4, C8, C14X2):
        pfmt, a, b = struct.unpack_from(">IHH", data, pos)
        pos += 8
        entries = np.frombuffer(data[pos:pos + 2 * a * b], dtype=">u2")
        pos += 2 * a * b
        palette = _palette_colors(pfmt, entries)
    bw, bh = BLOCK[fmt]
    tiles = -(-width // bw) * -(-height // bh)
    size = tiles * bw * bh * BPP[fmt] // 8
    body = data[pos:pos + size]
    if len(body) < size:
        raise TextureError("TXTR data is cut short")
    if fmt == CMPR:
        rgba = _cmpr(body, width, height)
    elif fmt == RGBA8:
        t = np.frombuffer(body, dtype=np.uint8).reshape(tiles, 2, 16, 2)
        values = np.stack([t[:, 0, :, 1], t[:, 1, :, 0], t[:, 1, :, 1], t[:, 0, :, 0]], -1).reshape(-1, 4)
        rgba = _untile(values, width, height, bw, bh)
    elif fmt in (RGB565, RGB5A3, IA8, C14X2):
        v = np.frombuffer(body, dtype=">u2")
        if fmt == RGB565:
            values = _rgb565(v)
        elif fmt == RGB5A3:
            values = _rgb5a3(v)
        elif fmt == IA8:
            values = _ia8(v)
        else:
            values = palette[np.minimum(v & 0x3FFF, len(palette) - 1)]
        rgba = _untile(values, width, height, bw, bh)
    elif fmt in (I8, IA4, C8):
        v = np.frombuffer(body, dtype=np.uint8).astype(np.uint32)
        if fmt == I8:
            values = np.stack([v, v, v, v], -1)
        elif fmt == IA4:
            lum, alpha = _ext4(v & 15), _ext4(v >> 4)
            values = np.stack([lum, lum, lum, alpha], -1)
        else:
            values = palette[np.minimum(v, len(palette) - 1)]
        rgba = _untile(values, width, height, bw, bh)
    else:  # I4, C4: two texels a byte, the first in the high nibble
        b = np.frombuffer(body, dtype=np.uint8).astype(np.uint32)
        v = np.stack([b >> 4, b & 15], -1).reshape(-1)
        values = np.stack([_ext4(v)] * 4, -1) if fmt == I4 else palette[np.minimum(v, len(palette) - 1)]
        rgba = _untile(values, width, height, bw, bh)
    return np.ascontiguousarray(rgba.astype(np.uint8))


def _downsample(rgba: np.ndarray) -> np.ndarray:
    h, w = rgba.shape[:2]
    a = rgba.astype(np.uint32).reshape(h // 2, 2, w // 2, 2, 4)
    return ((a.sum(axis=(1, 3)) + 2) // 4).astype(np.uint8)


def encode_rgba8(rgba: np.ndarray, mips: bool = True) -> bytes:
    """An RGBA8 TXTR from an (h, w, 4) uint8 array, top row first; mips down to 4x4 when asked.

    Width and height must be multiples of 4 (one GX tile); mip levels need both to stay even."""
    rgba = np.asarray(rgba, dtype=np.uint8)
    if rgba.ndim != 3 or rgba.shape[2] != 4:
        raise TextureError("expected an (height, width, 4) RGBA array")
    h, w = rgba.shape[:2]
    if h % 4 or w % 4 or h > 0xFFFF or w > 0xFFFF:
        raise TextureError(f"texture is {w}x{h}: both sides must be multiples of 4 (up to 65535)")
    levels = [rgba]
    while mips and levels[-1].shape[0] % 8 == 0 and levels[-1].shape[1] % 8 == 0:
        levels.append(_downsample(levels[-1]))
    out = bytearray(struct.pack(">IHHI", RGBA8, w, h, len(levels)))
    for level in levels:
        lh, lw = level.shape[:2]
        tiles = level.reshape(lh // 4, 4, lw // 4, 4, 4).swapaxes(1, 2).reshape(-1, 16, 4)
        ar = np.stack([tiles[..., 3], tiles[..., 0]], -1).reshape(-1, 32)
        gb = np.stack([tiles[..., 1], tiles[..., 2]], -1).reshape(-1, 32)
        out += np.concatenate([ar, gb], axis=1).tobytes()
    return bytes(out)
