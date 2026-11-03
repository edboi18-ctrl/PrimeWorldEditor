# Vendored from MetroidPrime2Port tools/echoes_disc.py (the port's disc reader) so the add-on works
# without the port checked out. Keep it in step with that file.
"""Minimal read-only access to the Echoes (G2ME01) disc: ISO FST, PAK, MLVL, MREA script layers.

Used by tools/rel_manifest.py. Everything is parsed from the user's own disc image; nothing is
written. Layouts follow the engine's own readers (src/Kyoto/CPakFile.cpp, src/MetroidPrime/
CWorld.cpp, CGameArea.cpp).
"""
import os
import struct

DEFAULT_ISO = "/run/media/odran/Leo/Portable/roms/gc/Metroid Prime 2 - Echoes.iso"


def find_iso():
    p = os.environ.get("MP2_LIST_DISC") or os.environ.get("MP2_DISC")  # LIST: a plain ISO when MP2_DISC is an rvz
    if p and os.path.exists(p):
        return p
    return DEFAULT_ISO if os.path.exists(DEFAULT_ISO) else None


# ---------------------------------------------------------------------------- ISO
class CisoFile:
    """A CISO image read as the plain disc it holds, through seek() and read() (Dolphin's
    DiscIO/CISOBlob: a 0x8000-byte header of "CISO", a little-endian block size and one byte per
    block, 1 = stored; the stored blocks follow in order, so block b is at 0x8000 + its index among
    the stored ones times the block size, and a block that isn't stored reads as zeros)."""

    HEADER_SIZE = 0x8000

    def __init__(self, f):
        head = f.read(self.HEADER_SIZE)
        if len(head) < self.HEADER_SIZE or head[:4] != b"CISO":
            raise ValueError("not a CISO image")
        (self.block_size,) = struct.unpack_from("<I", head, 4)
        if self.block_size == 0:
            raise ValueError("CISO block size 0")
        self.blocks = []
        stored = 0
        for used in head[8:]:
            self.blocks.append(stored if used == 1 else None)
            stored += used == 1
        self.f, self.pos = f, 0

    def seek(self, pos, whence=0):
        self.pos = pos if whence == 0 else self.pos + pos
        return self.pos

    def read(self, n=-1):
        out = bytearray()
        while n != 0:
            block, off = divmod(self.pos, self.block_size)
            if block >= len(self.blocks):
                break
            take = self.block_size - off if n < 0 else min(n, self.block_size - off)
            index = self.blocks[block]
            if index is None:
                out += bytes(take)
            else:
                self.f.seek(self.HEADER_SIZE + index * self.block_size + off)
                data = self.f.read(take)
                out += data + bytes(take - len(data))
            self.pos += take
            n = n - take if n > 0 else n
        return bytes(out)

    def close(self):
        self.f.close()


def open_disc(path):
    """The disc at `path` as a file object: a plain ISO/GCM, or a CISO read through CisoFile."""
    f = open(path, "rb")
    if f.read(4) == b"CISO":
        f.seek(0)
        try:
            return CisoFile(f)
        except ValueError:
            f.close()
            raise
    f.seek(0)
    return f


class Iso:
    def __init__(self, path):
        self.f = open_disc(path)
        self.f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", self.f.read(8))
        self.f.seek(fst_off)
        fst = self.f.read(fst_size)
        count = struct.unpack_from(">I", fst, 8)[0]
        names = count * 12
        self.files = {}
        stack = [("", count)]
        for i in range(1, count):
            while stack and i >= stack[-1][1] and len(stack) > 1:
                stack.pop()
            flag_name, off, size = struct.unpack_from(">III", fst, i * 12)
            name_off = flag_name & 0xFFFFFF
            end = fst.index(b"\0", names + name_off)
            name = fst[names + name_off:end].decode("latin1")
            if flag_name >> 24:
                stack.append((stack[-1][0] + name + "/", size))
            else:
                self.files[stack[-1][0] + name] = (off, size)

    def read(self, path, off=0, n=None):
        o, s = self.files[path]
        n = s - off if n is None else n
        self.f.seek(o + off)
        return self.f.read(n)


# ---------------------------------------------------------------------------- LZO1X
def lzo1x_decompress(src):
    """Plain LZO1X decoder (kernel-style state machine); returns the decoded bytes."""
    out = bytearray()
    ip = 0

    def ext(t, base):
        nonlocal ip
        if t == 0:
            while src[ip] == 0:
                t += 255
                ip += 1
            t += base + src[ip]
            ip += 1
        return t

    def match(dist, length):
        start = len(out) - dist
        if start < 0:
            raise ValueError("lzo: bad distance")
        if dist >= length:
            out.extend(out[start:start + length])
        else:
            for k in range(length):
                out.append(out[start + k])

    state = 0
    if src[0] > 17:
        n = src[0] - 17
        ip = 1
        out += src[ip:ip + n]
        ip += n
        state = 4 if n >= 4 else n
    while True:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:
                t = ext(t, 15)
                out += src[ip:ip + t + 3]
                ip += t + 3
                state = 4
                continue
            if state == 4:
                dist, length = (1 + 0x800) + (t >> 2) + (src[ip] << 2), 3
            else:
                dist, length = 1 + (t >> 2) + (src[ip] << 2), 2
            ip += 1
            state = t & 3
            match(dist, length)
        else:
            if t >= 64:
                dist, length = 1 + ((t >> 2) & 7) + (src[ip] << 3), (t >> 5) + 1
                ip += 1
                state = t & 3
            elif t >= 32:
                length = ext(t & 31, 31) + 2
                w = src[ip] | (src[ip + 1] << 8)
                ip += 2
                dist, state = 1 + (w >> 2), w & 3
            else:
                length = ext(t & 7, 7) + 2
                w = src[ip] | (src[ip + 1] << 8)
                ip += 2
                dist = ((t & 8) << 11) + (w >> 2)
                if dist == 0:
                    return bytes(out)
                dist += 0x4000
                state = w & 3
            match(dist, length)
        if state:
            out += src[ip:ip + state]
            ip += state


def inflate_chunks(buf, out_len, pos=0):
    """Retro's chunked LZO: s16 chunk size (negative = stored), repeated until out_len bytes."""
    out = bytearray()
    while len(out) < out_len:
        (cs,) = struct.unpack_from(">h", buf, pos)
        pos += 2
        if cs < 0:
            out += buf[pos:pos - cs]
            pos += -cs
        else:
            out += lzo1x_decompress(buf[pos:pos + cs])
            pos += cs
    return bytes(out[:out_len])


# ---------------------------------------------------------------------------- PAK
def fcc(v):
    return struct.pack(">I", v).decode("latin1")


class Pak:
    def __init__(self, iso, path):
        self.iso, self.path = iso, path
        _, size = iso.files[path]
        head = iso.read(path, 0, min(size, 1 << 20))
        ver, _, nn = struct.unpack_from(">III", head, 0)
        assert ver == 0x30005, (path, hex(ver))
        pos = 12
        self.names = {}
        for _ in range(nn):
            t, i, ln = struct.unpack_from(">III", head, pos)
            pos += 12
            self.names[head[pos:pos + ln].rstrip(b"\0").decode("latin1")] = (fcc(t), i)
            pos += ln
        (rc,) = struct.unpack_from(">I", head, pos)
        pos += 4
        self.res = {}
        for _ in range(rc):
            fl, t, i, sz, off = struct.unpack_from(">IIIII", head, pos)
            pos += 20
            self.res[i] = (fcc(t), fl, sz, off)

    def get(self, rid):
        t, fl, sz, off = self.res[rid]
        raw = self.iso.read(self.path, off, sz)
        if fl:
            (dl,) = struct.unpack_from(">I", raw, 0)
            return inflate_chunks(raw, dl, 4)
        return raw


def load_paks(iso):
    paks = []
    for p in sorted(iso.files):
        if p.lower().endswith(".pak"):
            paks.append(Pak(iso, p))
    return paks


# ---------------------------------------------------------------------------- MLVL
class Rd:
    def __init__(self, b, pos=0):
        self.b, self.p = b, pos

    def u32(self):
        v = struct.unpack_from(">I", self.b, self.p)[0]
        self.p += 4
        return v

    def skip(self, n):
        self.p += n

    def cstr(self):
        e = self.b.index(b"\0", self.p)
        s = self.b[self.p:e].decode("latin1")
        self.p = e + 1
        return s


def parse_mlvl(b):
    r = Rd(b)
    r.u32()
    ver = r.u32()
    strg = r.u32()
    dark = r.u32() if ver >= 22 else None
    if ver >= 23:
        r.u32()
    r.u32()  # savw
    skybox = r.u32()  # the world's default sky CMDL (CWorld::Update reads it here)
    n = r.u32()
    areas = []
    for i in range(n):
        a = {"index": i}
        a["strg"] = r.u32()
        r.skip(48 + 24)
        a["mrea"] = r.u32()
        r.u32()  # save id
        r.skip(2 * r.u32())
        r.skip(8 * r.u32())
        r.skip(8 * r.u32())
        r.skip(4 * r.u32())
        a["docks"] = []  # per dock: [(connected area index, connected dock index)]
        for _ in range(r.u32()):
            a["docks"].append([(r.u32(), r.u32()) for _ in range(r.u32())])
            r.skip(12 * r.u32())  # the dock's portal corners
        a["mods"] = [r.cstr() for _ in range(r.u32())]
        a["offs"] = [r.u32() for _ in range(r.u32())]
        a["iname"] = r.cstr()
        areas.append(a)
    # end: where the area list stops (the MapWorld ID, layer flags and layer names follow; see
    # tools/mcp/workshop.py parse_layer_table).
    return {"version": ver, "strg": strg, "dark": dark, "skybox": skybox, "areas": areas, "end": r.p}


# ---------------------------------------------------------------------------- MREA
def mrea_layers(b):
    """Return the raw script-layer sections of a version-0x19 MREA, in layer order.

    Only the blocks that hold script sections are inflated (the rest is geometry and is slow in
    pure Python). A compressed block's data is right-aligned in its 32-byte-padded read.
    """
    h = struct.unpack_from(">32I", b, 0)
    assert h[0] == 0xDEADBEEF and h[1] >= 24, hex(h[1])
    nsec, nlay, first, nblk = h[16], h[15], h[18], h[28]
    pos = 0x80
    sizes = struct.unpack_from(">%dI" % nsec, b, pos)
    pos += (nsec * 4 + 31) & ~31
    blocks = [struct.unpack_from(">4I", b, pos + 16 * i) for i in range(nblk)]
    pos += (nblk * 16 + 31) & ~31
    layers, si = [], 0
    for buf, dl, cl, sc in blocks:
        rd = ((cl or buf) + 31) & ~31
        if sc == 1 and first <= si < first + nlay:
            raw = b[pos:pos + rd]
            data = inflate_chunks(raw[rd - cl:], dl) if cl else raw[:dl]
            layers.append(data[:sizes[si]])
        pos += rd
        si += sc
    return layers


def mrea_generated(b):
    """Return the generated-object (SCGN) section of a version-0x19 MREA, or None.

    Its section index is header word 19 (CGameArea's generated-script section); only the block
    holding it is inflated. Objects in it are built on demand (boss drops, for one) through the
    same loaders as layer objects, keeping their editor id.
    """
    h = struct.unpack_from(">32I", b, 0)
    assert h[0] == 0xDEADBEEF and h[1] >= 24, hex(h[1])
    nsec, want, nblk = h[16], h[19], h[28]
    pos = 0x80
    sizes = struct.unpack_from(">%dI" % nsec, b, pos)
    pos += (nsec * 4 + 31) & ~31
    blocks = [struct.unpack_from(">4I", b, pos + 16 * i) for i in range(nblk)]
    pos += (nblk * 16 + 31) & ~31
    si = 0
    for buf, dl, cl, sc in blocks:
        rd = ((cl or buf) + 31) & ~31
        if si <= want < si + sc:
            raw = b[pos:pos + rd]
            data = inflate_chunks(raw[rd - cl:], dl) if cl else raw[:dl]
            off = sum(sizes[si:want])
            sec = data[off:off + sizes[want]]
            return sec if sec[:4] == b"SCGN" else None
        pos += rd
        si += sc
    return None


def scgn_objects(sec):
    """Return [(fourcc, editor_id, property_bytes)] from an SCGN section.

    Layout from CGameArea and CScriptObjectLoaderHelper::LoadGeneratedScriptObjects: u32 'SCGN',
    u8, u8, u32 count, then objects laid out as in a script layer.
    """
    assert sec[:4] == b"SCGN", sec[:8]
    return scly_objects(b"SCLY" + sec[4:6] + b"\0\0\0\0" + sec[6:])


def strg_strings(b):
    """All strings of the first language of a v1 STRG (name table skipped), as str."""
    assert struct.unpack_from(">II", b, 0) == (0x87654321, 1), b[:8].hex()
    langs, cnt = struct.unpack_from(">II", b, 8)
    p = 16 + 12 * langs
    nc, nsz = struct.unpack_from(">II", b, p)
    p += 8 + nsz
    offs = struct.unpack_from(">%dI" % cnt, b, p)
    out = []
    for o in offs:
        e = p + o
        while b[e:e + 2] != b"\0\0":
            e += 2
        out.append(b[p + o:e].decode("utf-16-be", "replace"))
    return out


def scly_objects(sec):
    """Return [(fourcc, editor_id, property_bytes)] from one script-layer section.

    Layout from CScriptObjectLoaderHelper: u32 'SCLY', u8, u32, u8, u32 count, then per object
    FourCC, u16 length, u32 editor id, u16 connection count, 12 bytes per connection, properties.
    """
    assert sec[:4] == b"SCLY", sec[:8]
    count = struct.unpack_from(">I", sec, 10)[0]
    p = 14
    out = []
    for _ in range(count):
        t = sec[p:p + 4].decode("latin1")
        (ln,) = struct.unpack_from(">H", sec, p + 4)
        body = sec[p + 6:p + 6 + ln]
        eid, nc = struct.unpack_from(">IH", body, 0)
        out.append((t, eid, body[6 + 12 * nc:]))
        p += 6 + ln
    return out
