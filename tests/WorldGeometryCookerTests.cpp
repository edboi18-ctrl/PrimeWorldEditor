// Tests for CWorldGeometryCooker (Echoes world geometry sections).
//
// The sections are read back by EngineRead below, a transcription of the game's readers in the
// Echoes port (MetroidPrime2Port): CGameArea::PostConstructArea and FillInStaticGeometry
// (section walk), CMetroidModelInstance (material set, surface records, surface groups),
// CCubeSurface::ConvertSurfaceHeader / GetSurfaceHeaderSize (surfaces), CCubeModel's display list
// walk and vertex arrays with CGraphics::SetDefaultVtxAttrFmt (vertex formats), SAreaSurface, and
// CAreaRenderOctTree (header, nodes, RecursiveBuildOverlaps with CAABox::DoBoundsOverlap / Inside).

#include "TestLog.h"
#include <Core/Resource/Cooker/CWorldGeometryCooker.h>
#include <Core/GameProject/CResourceStore.h>
#include <Core/Resource/CMaterial.h>
#include <Core/Resource/CMaterialSet.h>
#include <Core/Resource/Collision/CCollisionMeshGroup.h>
#include <Core/Resource/Factory/CCollisionLoader.h>
#include <Core/Resource/Factory/CMaterialLoader.h>
#include <Common/FileIO/CMemoryInStream.h>

#include <fstream>
#include <iterator>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace
{
int gFailures = 0;
#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++gFailures; } } while (0)

using Cooker = CWorldGeometryCooker;

// ---------------------------------------------------------------------------- inputs
struct SBe
{
    std::vector<uint8_t> B;
    void U32(uint32_t v) { for (int i = 3; i >= 0; i--) B.push_back(uint8_t(v >> (8 * i))); }
};

/** A material laid out as CCubeMaterial::SetCurrent reads it, with no textures, TEV stages or UV
 *  animations: flags, texture count, vertex description, two Echoes words, group, blend, one
 *  colour channel, TEV count, texgen count, animation block size and count. */
std::vector<uint8_t> Material(uint32_t Flags, uint32_t Desc)
{
    SBe m;
    for (uint32_t w : {Flags, 0u, Desc, 0u, 0u, 0u, 0x00010000u, 1u, 0x3001u, 0u, 0u, 4u, 0u})
        m.U32(w);
    return m.B;
}

std::vector<uint8_t> MaterialSet(const std::vector<std::vector<uint8_t>>& Mats, const std::vector<uint32_t>& TextureIDs = {})
{
    SBe s;
    s.U32(uint32_t(TextureIDs.size()));
    for (uint32_t id : TextureIDs) s.U32(id);
    s.U32(uint32_t(Mats.size()));
    uint32_t End = 0;
    for (const auto& m : Mats) { End += uint32_t(m.size()); s.U32(End); }
    for (const auto& m : Mats) s.B.insert(s.B.end(), m.begin(), m.end());
    return s.B;
}

constexpr uint32_t kIdx16 = 3, kIdx8 = 2;
constexpr uint32_t Attr(int i, uint32_t type) { return type << (2 * i); }
// POS, NRM, CLR0, CLR1, TEX0.. (CGX::SetVtxDescv_Compressed)
const uint32_t kDescPosNrmTex0 = Attr(0, kIdx16) | Attr(1, kIdx16) | Attr(4, kIdx16);
const uint32_t kDescPosNrm = Attr(0, kIdx16) | Attr(1, kIdx16);
const uint32_t kDescLightmapped = Attr(0, kIdx16) | Attr(1, kIdx16) | Attr(2, kIdx8) | Attr(4, kIdx16) | Attr(5, kIdx16);

/** Model A: a 2x2-quad floor (material 0) and a wall (material 2), float normals. */
Cooker::SModel FloorAndWall()
{
    Cooker::SModel M;
    for (int y = 0; y <= 2; y++)
        for (int x = 0; x <= 2; x++)
        {
            M.Positions.push_back(CVector3f(float(x), float(y), 0.f));
            M.TexCoords.push_back(CVector2f(x * 0.5f, y * 0.5f));
        }
    M.Normals.push_back(CVector3f(0, 0, 1));
    M.Normals.push_back(CVector3f(0, 1, 0));
    Cooker::SSurface Floor{0, {}};
    auto V = [](int x, int y) { return uint16_t(y * 3 + x); };
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            for (uint16_t p : {V(x, y), V(x + 1, y), V(x + 1, y + 1), V(x, y), V(x + 1, y + 1), V(x, y + 1)})
            {
                Cooker::SCorner c; c.Position = p; c.Normal = 0; c.Tex[0] = p; Floor.Corners.push_back(c);
            }
    M.Surfaces.push_back(Floor);

    const uint16_t Base = uint16_t(M.Positions.size());
    for (auto p : {CVector3f(0, 0, 0), CVector3f(2, 0, 0), CVector3f(2, 0, 2), CVector3f(0, 0, 2)})
        M.Positions.push_back(p);
    Cooker::SSurface Wall{2, {}};
    for (uint16_t i : {0, 1, 2, 0, 2, 3})
    {
        Cooker::SCorner c; c.Position = uint16_t(Base + i); c.Normal = 1; Wall.Corners.push_back(c);
    }
    M.Surfaces.push_back(Wall);
    return M;
}

/** Model B: a lightmapped ramp (material 1, packed lightmap UVs): short normals, colours. */
Cooker::SModel LightmappedRamp()
{
    Cooker::SModel M;
    M.Positions = {CVector3f(3, 0, 0), CVector3f(5, 0, 1), CVector3f(5, 2, 1), CVector3f(3, 2, 0)};
    const float n = 1.f / std::sqrt(2.f);
    M.Normals = {CVector3f(-n, 0, n)};
    M.Colors = {0xFF8040FFu, 0x204060FFu};
    M.TexCoords = {CVector2f(0, 0), CVector2f(4, 0), CVector2f(4, 4), CVector2f(0, 4)};
    M.LightmapTexCoords = {CVector2f(0.1f, 0.2f), CVector2f(0.9f, 0.2f), CVector2f(0.9f, 0.8f), CVector2f(0.1f, 0.8f)};
    Cooker::SSurface S{1, {}};
    for (uint16_t i : {0, 1, 2, 0, 2, 3})
    {
        Cooker::SCorner c; c.Position = i; c.Normal = 0; c.Color = uint16_t(i % 2); c.Tex[0] = i; c.Tex[1] = i; S.Corners.push_back(c);
    }
    M.Surfaces.push_back(S);
    return M;
}

/** Model C: 64 small tiles over a 16 x 16 area, one surface each: enough to make the octree branch. */
Cooker::SModel Tiles()
{
    Cooker::SModel M;
    M.Normals = {CVector3f(0, 0, 1)};
    for (int t = 0; t < 64; t++)
    {
        const float x = float(t % 8) * 2.f + 10.f, y = float(t / 8) * 2.f;
        const uint16_t b = uint16_t(M.Positions.size());
        for (auto p : {CVector3f(x, y, 0), CVector3f(x + 1, y, 0), CVector3f(x + 1, y + 1, 0)})
            M.Positions.push_back(p);
        Cooker::SSurface S{2, {}};
        for (uint16_t i : {0, 1, 2})
        {
            Cooker::SCorner c; c.Position = uint16_t(b + i); S.Corners.push_back(c);
        }
        M.Surfaces.push_back(S);
    }
    return M;
}

// ---------------------------------------------------------------------------- engine reader
struct SRd
{
    const std::vector<uint8_t>& B;
    size_t P = 0;
    bool Ok = true;
    uint8_t U8() { if (P + 1 > B.size()) { Ok = false; return 0; } return B[P++]; }
    uint16_t U16() { uint16_t hi = U8(); return uint16_t(hi << 8 | U8()); }
    int16_t S16() { return static_cast<int16_t>(U16()); }
    uint32_t U32() { uint32_t hi = U16(); return hi << 16 | U16(); }
    float F32() { uint32_t v = U32(); float f; std::memcpy(&f, &v, 4); return f; }
    CVector3f Vec() { float x = F32(), y = F32(); return CVector3f(x, y, F32()); }
    CAABox Box() { CVector3f a = Vec(); return CAABox(a, Vec()); }
};

bool Overlap(const CAABox& a, const CAABox& b)   // CAABox::DoBoundsOverlap
{
    return !(b.Min().X > a.Max().X || a.Min().X > b.Max().X || b.Min().Y > a.Max().Y || a.Min().Y > b.Max().Y ||
             b.Min().Z > a.Max().Z || a.Min().Z > b.Max().Z);
}
bool Inside(const CAABox& a, const CAABox& b)    // a.Inside(b)
{
    return a.Min().X >= b.Min().X && a.Max().X <= b.Max().X && a.Min().Y >= b.Min().Y && a.Max().Y <= b.Max().Y &&
           a.Min().Z >= b.Min().Z && a.Max().Z <= b.Max().Z;
}
bool Contains(const CAABox& b, const CVector3f& p)
{
    return p.X >= b.Min().X && p.Y >= b.Min().Y && p.Z >= b.Min().Z && p.X <= b.Max().X && p.Y <= b.Max().Y && p.Z <= b.Max().Z;
}

struct SReadCorner { CVector3f Position, Normal; uint32_t Color = 0; CVector2f Tex[7]; };
struct SReadSurface { uint32_t Material; CAABox Bounds; CVector3f Center, Normal; uint32_t ExtraSize; std::vector<SReadCorner> Corners; };
struct SReadModel { uint32_t Flags; CAABox Bounds; std::vector<SReadSurface> Surfaces; std::vector<uint16_t> Records; std::vector<uint16_t> Groups; };
struct SAreaSurf { CAABox Bounds; int16_t Model, Group; };

struct SEngine
{
    bool Ok = true;
    std::vector<uint32_t> MatFlags, MatDesc;
    std::vector<SReadModel> Models;
    std::vector<SAreaSurf> AreaSurfaces;
    // render octree
    uint32_t MeshCount = 0, WordCount = 0;
    CAABox OctreeBounds;
    std::vector<uint32_t> Bitmaps;
    std::vector<size_t> NodeAt;
    const std::vector<uint8_t>* Octree = nullptr;
    size_t EntriesAt = 0;
    uint32_t AmbientIds = 0, AmbientIndices = 0;

    /** CAreaRenderOctTree::FindOverlappingModels. */
    std::vector<uint32_t> Query(const CAABox& Test) const
    {
        std::vector<uint32_t> Out(WordCount, 0);
        std::function<void(uint32_t, const CAABox&)> Rec = [&](uint32_t Node, const CAABox& Bounds) {
            SRd r{*Octree, EntriesAt + NodeAt[Node]};
            const uint16_t Bitmap = r.U16(); r.U8(); const uint8_t Flags = r.U8();
            if (!Overlap(Test, Bounds))
                return;
            if (Flags == 0 || Inside(Bounds, Test))
            {
                for (uint32_t i = 0; i < WordCount; i++) Out[i] |= Bitmaps[Bitmap * WordCount + i];
                return;
            }
            static const int kCounts[8] = {0, 2, 2, 4, 2, 4, 4, 8};
            static const int kAxes[21] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, -1, -1, -1, 0, 2, 1, 2, 0, 1};
            for (int c = 0; c < kCounts[Flags]; c++)
            {
                const uint16_t Child = r.U16();
                // GetNodeBounds
                float mn[3] = {Bounds.Min().X, Bounds.Min().Y, Bounds.Min().Z}, mx[3] = {Bounds.Max().X, Bounds.Max().Y, Bounds.Max().Z};
                float ct[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
                if (Flags == 1 || Flags == 2 || Flags == 4)
                {
                    const int a = Flags == 1 ? 0 : (Flags == 2 ? 1 : 2);
                    (c == 0 ? mx[a] : mn[a]) = ct[a];
                }
                else if (Flags == 7)
                {
                    for (int i = 0; i < 3; i++) ((c & (1 << i)) ? mn[i] : mx[i]) = ct[i];
                }
                else
                {
                    const int a = kAxes[Flags * 3], b = kAxes[Flags * 3 + 1];
                    switch (c)
                    {
                    case 0: mx[a] = ct[a]; mx[b] = ct[b]; break;
                    case 1: mn[a] = ct[a]; mx[b] = ct[b]; break;
                    case 2: mn[b] = ct[b]; mx[a] = ct[a]; break;
                    case 3: mn[a] = ct[a]; mn[b] = ct[b]; break;
                    }
                }
                Rec(Child, CAABox(CVector3f(mn[0], mn[1], mn[2]), CVector3f(mx[0], mx[1], mx[2])));
            }
        };
        Rec(0, OctreeBounds);
        return Out;
    }
};

SEngine EngineRead(const std::vector<std::vector<uint8_t>>& S, uint32_t ModelCount)
{
    SEngine E;
    // Material set (CMetroidModelInstance::GetMaterialByIndex).
    {
        SRd r{S[0]};
        const uint32_t TexCount = r.U32();
        r.P += 4 * TexCount;
        const uint32_t MatCount = r.U32();
        std::vector<uint32_t> Ends(MatCount);
        for (auto& e : Ends) e = r.U32();
        const size_t Mats = r.P;
        for (uint32_t i = 0; i < MatCount; i++)
        {
            SRd m{S[0], Mats + (i ? Ends[i - 1] : 0)};
            E.MatFlags.push_back(m.U32());
            const uint32_t tc = m.U32();
            m.P += 4 * tc;
            E.MatDesc.push_back(m.U32());
        }
        E.Ok &= r.Ok;
    }

    size_t s = 1;
    for (uint32_t model = 0; model < ModelCount; model++)
    {
        if (s + 7 > S.size()) { E.Ok = false; return E; }
        SReadModel M;
        SRd h{S[s]};
        M.Flags = h.U32();
        for (int i = 0; i < 12; i++) h.F32();
        M.Bounds = h.Box();
        const auto& Pos = S[s + 1];
        const auto& Nrm = S[s + 2];
        const auto& Clr = S[s + 3];
        const auto& Tc = S[s + 4];
        const auto& Packed = S[s + 5];
        const uint32_t SurfaceCount = SRd{S[s + 6]}.U32();
        const bool ShortNormals = (M.Flags & 1) != 0;   // CCubeModel::SetArraysCurrent stride
        if (s + 9 + SurfaceCount > S.size()) { E.Ok = false; return E; }

        for (uint32_t surf = 0; surf < SurfaceCount; surf++)
        {
            const auto& Sec = S[s + 7 + surf];
            SRd r{Sec};
            SReadSurface Out;
            Out.Center = r.Vec();
            Out.Material = r.U32();
            const uint32_t ListSize = r.U32() & 0x7FFFFFFF;   // GetDisplayListSize
            r.U32(); r.U32();
            Out.ExtraSize = r.U32();
            Out.Normal = r.Vec();
            r.S16(); r.S16();
            Out.Bounds = Out.ExtraSize ? r.Box() : CAABox(Out.Center, Out.Center);   // GetBounds
            const size_t HeaderSize = (0x4f + Out.ExtraSize) & ~31u;                 // GetSurfaceHeaderSize (PC)
            E.Ok &= ListSize % 32 == 0 && HeaderSize + ListSize <= Sec.size() && Out.Material < E.MatDesc.size();
            if (!E.Ok) return E;
            const uint32_t Desc = E.MatDesc[Out.Material];
            const bool PackedUVs = (E.MatFlags[Out.Material] & (1u << 13)) != 0;

            // The display list, as CCubeModel walks it (primitive & 0xf8, a u16 count).
            SRd dl{Sec, HeaderSize};
            while (dl.P < HeaderSize + ListSize)
            {
                const uint8_t Op = dl.U8();
                if ((Op & 0xF8) == 0) break;
                E.Ok &= (Op & 0xF8) == 0x90;   // GX_TRIANGLES
                const uint8_t Format = Op & 7;
                // SetDefaultVtxAttrFmt: format 0 float normals and UVs, 1 short normals, 2 short
                // normals and TEX0 as U16/32768.
                E.Ok &= ShortNormals ? (Format == (PackedUVs ? 2 : 1)) : Format == 0;
                const uint16_t Count = dl.U16();
                E.Ok &= Count % 3 == 0;
                for (uint16_t v = 0; v < Count; v++)
                {
                    SReadCorner C;
                    uint32_t Idx[11];
                    for (int a = 0; a < 11; a++)
                    {
                        const uint32_t t = (Desc >> (2 * a)) & 3;
                        Idx[a] = t == 3 ? dl.U16() : (t == 2 ? dl.U8() : 0xFFFFFFFF);
                    }
                    auto At = [&](const std::vector<uint8_t>& Arr, size_t Offset, size_t Size) {
                        if (Offset + Size > Arr.size()) E.Ok = false;
                        return SRd{Arr, std::min(Offset, Arr.size())};
                    };
                    if (Idx[0] != 0xFFFFFFFF) { auto r2 = At(Pos, Idx[0] * 12, 12); C.Position = r2.Vec(); }
                    if (Idx[1] != 0xFFFFFFFF)
                    {
                        if (ShortNormals) { auto r2 = At(Nrm, Idx[1] * 6, 6); float x = r2.S16() / 16384.f, y = r2.S16() / 16384.f; C.Normal = CVector3f(x, y, r2.S16() / 16384.f); }
                        else { auto r2 = At(Nrm, Idx[1] * 12, 12); C.Normal = r2.Vec(); }
                    }
                    if (Idx[2] != 0xFFFFFFFF) { auto r2 = At(Clr, Idx[2] * 4, 4); C.Color = r2.U32(); }
                    for (int t = 0; t < 7; t++)
                    {
                        if (Idx[4 + t] == 0xFFFFFFFF) continue;
                        if (t == 0 && PackedUVs) { auto r2 = At(Packed, Idx[4] * 4, 4); float u = r2.U16() / 32768.f; C.Tex[0] = CVector2f(u, r2.U16() / 32768.f); }
                        else { auto r2 = At(Tc, Idx[4 + t] * 8, 8); float u = r2.F32(); C.Tex[t] = CVector2f(u, r2.F32()); }
                    }
                    Out.Corners.push_back(C);
                }
            }
            E.Ok &= dl.Ok;
            M.Surfaces.push_back(Out);
        }
        // Surface records and groups (CMetroidModelInstance).
        for (auto* pDst : {&M.Records, &M.Groups})
        {
            const auto& Sec = S[pDst == &M.Records ? s + 7 + SurfaceCount : s + 8 + SurfaceCount];
            for (size_t i = 0; i + 1 < Sec.size(); i += 2) pDst->push_back(uint16_t(Sec[i] << 8 | Sec[i + 1]));
        }
        E.Models.push_back(std::move(M));
        s += 9 + SurfaceCount;
    }

    // PostConstructArea: the render octree (header index not -1), then FillInStaticGeometry's
    // (section + 1) area surfaces and (section + 2) ambient lights.
    if (s + 3 != S.size()) { E.Ok = false; return E; }
    {
        E.Octree = &S[s];
        SRd r{S[s]};
        r.U32(); r.U32();
        const uint32_t BitmapCount = r.U32();
        E.MeshCount = r.U32();
        const uint32_t NodeCount = r.U32();
        E.OctreeBounds = r.Box();
        E.WordCount = (E.MeshCount + 31) / 32;
        r.P = 64;
        for (uint32_t i = 0; i < BitmapCount * E.WordCount; i++) E.Bitmaps.push_back(r.U32());
        for (uint32_t i = 0; i < NodeCount; i++) E.NodeAt.push_back(r.U32());
        E.EntriesAt = r.P;
        E.Ok &= r.Ok && NodeCount > 0;
        for (uint32_t n = 0; n < NodeCount && E.Ok; n++)
        {
            SRd node{S[s], E.EntriesAt + E.NodeAt[n]};
            const uint16_t Bitmap = node.U16(); node.U8(); const uint8_t Flags = node.U8();
            static const int kCounts[8] = {0, 2, 2, 4, 2, 4, 4, 8};
            E.Ok &= Bitmap < BitmapCount && Flags < 8;
            for (int c = 0; c < kCounts[Flags & 7]; c++) E.Ok &= node.U16() < NodeCount;
            E.Ok &= node.Ok;
        }
        // No bit past the mesh count (the renderer indexes area surfaces by bit).
        for (uint32_t b = 0; b < BitmapCount; b++)
            for (uint32_t bit = E.MeshCount; bit < E.WordCount * 32; bit++)
                E.Ok &= !(E.Bitmaps[b * E.WordCount + bit / 32] & (1u << (bit % 32)));
    }
    {
        SRd r{S[s + 1]};
        const uint32_t Count = r.U32();
        for (uint32_t i = 0; i < Count; i++)
        {
            SAreaSurf a;
            a.Bounds = r.Box(); a.Model = r.S16(); a.Group = r.S16(); r.S16(); r.S16();
            E.AreaSurfaces.push_back(a);
        }
        E.Ok &= r.Ok;
        SRd am{S[s + 2]};
        E.AmbientIds = am.U32();
        E.AmbientIndices = am.U32();
    }
    return E;
}

// ---------------------------------------------------------------------------- checks
void CheckRoom(const char* Name, const std::vector<uint8_t>& Set, const std::vector<Cooker::SModel>& Models)
{
    std::fprintf(stderr, "== %s\n", Name);
    Cooker::SOutput Out;
    std::string Error;
    CHECK(Cooker::Write(Set, Models, Out, &Error));
    if (!Error.empty()) { std::fprintf(stderr, "   %s\n", Error.c_str()); return; }
    for (const auto& Sec : Out.Sections) CHECK(Sec.size() % 32 == 0);

    const SEngine E = EngineRead(Out.Sections, Out.ModelCount);
    CHECK(E.Ok);
    if (!E.Ok) return;
    CHECK(E.Models.size() == Models.size());

    size_t AreaCount = 0;
    for (const auto& M : Models) AreaCount += M.Surfaces.size();
    CHECK(E.AreaSurfaces.size() == AreaCount + 1);           // entry 0 is the placeholder
    CHECK(E.AreaSurfaces[0].Model == -1 && E.AreaSurfaces[0].Group == -1);
    CHECK(E.MeshCount == AreaCount);
    CHECK(E.AmbientIds == 0 && E.AmbientIndices == 0);

    std::vector<int> AreaSeen(AreaCount, 0);
    for (size_t m = 0; m < Models.size(); m++)
    {
        const auto& In = Models[m];
        const auto& Read = E.Models[m];
        CHECK(Read.Surfaces.size() == In.Surfaces.size());
        const bool Short = (Read.Flags & 1) != 0;
        for (size_t s = 0; s < In.Surfaces.size() && s < Read.Surfaces.size(); s++)
        {
            const auto& IS = In.Surfaces[s];
            const auto& RS = Read.Surfaces[s];
            const bool Packed = (E.MatFlags[IS.MaterialIndex] & (1u << 13)) != 0;
            const uint32_t Desc = E.MatDesc[IS.MaterialIndex];
            CHECK(RS.Material == IS.MaterialIndex);
            CHECK(RS.ExtraSize == 0x18);
            CHECK(RS.Corners.size() == IS.Corners.size());
            for (size_t c = 0; c < IS.Corners.size() && c < RS.Corners.size(); c++)
            {
                const auto& ic = IS.Corners[c];
                const auto& rc = RS.Corners[c];
                CHECK(rc.Position == In.Positions[ic.Position]);
                CHECK(Contains(RS.Bounds, rc.Position));
                CHECK(Contains(Read.Bounds, rc.Position));
                if (Desc & (3u << 2))
                {
                    const CVector3f d = rc.Normal - In.Normals[ic.Normal];
                    CHECK(std::fabs(d.X) + std::fabs(d.Y) + std::fabs(d.Z) <= (Short ? 3.f / 16384.f : 0.f));
                }
                if (Desc & (3u << 4)) CHECK(rc.Color == In.Colors[ic.Color]);
                for (int t = 0; t < 7; t++)
                {
                    if (!(Desc & (3u << (8 + 2 * t)))) continue;
                    const CVector2f want = (t == 0 && Packed) ? In.LightmapTexCoords[ic.Tex[0]] : In.TexCoords[ic.Tex[t]];
                    const float tol = (t == 0 && Packed) ? 1.f / 32768.f : 0.f;
                    CHECK(std::fabs(rc.Tex[t].X - want.X) <= tol && std::fabs(rc.Tex[t].Y - want.Y) <= tol);
                }
            }
            CHECK(std::fabs(RS.Normal.Magnitude() - 1.f) < 1e-4f);

            // Records: this surface's area surface (DrawGeometry: records[s * 2 + 2]).
            const uint16_t Area = Read.Records[s * 2 + 2];
            CHECK(Read.Records[0] == In.Surfaces.size());
            CHECK(Area < AreaCount);
            if (Area >= AreaCount) continue;
            AreaSeen[Area]++;
            const SAreaSurf& A = E.AreaSurfaces[Area + 1];
            CHECK(A.Model == int16_t(m));
            CHECK(Inside(RS.Bounds, A.Bounds));
            // Its group (CSurfaceGroups::GetSurfaceCount / GetSurfaceIndices) lists this surface.
            const auto& G = Read.Groups;
            const uint16_t GroupCount = G[0];
            CHECK(A.Group >= 0 && A.Group < GroupCount);
            if (A.Group < 0 || A.Group >= GroupCount) continue;
            const uint16_t Count = A.Group == 0 ? G[1] : G[A.Group + 1] - G[A.Group];
            const uint16_t Start = A.Group == 0 ? 0 : G[A.Group];
            bool Listed = false;
            for (uint16_t i = 0; i < Count; i++) Listed |= G[GroupCount + 1 + Start + i] == s;
            CHECK(Listed);

            // The octree reports this area surface for its own bounds, and for its centre alone
            // (a light there), as CCubeRenderer's queries need.
            for (const CAABox& Test : {A.Bounds, CAABox(RS.Center, RS.Center)})
            {
                const auto Bits = E.Query(Test);
                CHECK(Bits[Area / 32] & (1u << (Area % 32)));
            }
        }
    }
    for (int Seen : AreaSeen) CHECK(Seen == 1);   // each area surface is exactly one model surface

    // No false negatives: for boxes across the room, every area surface that overlaps the box is reported.
    const CAABox& B = Out.Bounds;
    int Queries = 0;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
        {
            const CVector3f Lo(B.Min().X + (B.Max().X - B.Min().X) * i / 6.f, B.Min().Y + (B.Max().Y - B.Min().Y) * j / 6.f, B.Min().Z - 1.f);
            const CAABox Test(Lo, Lo + CVector3f(1.5f, 1.5f, 2.5f));
            const auto Bits = E.Query(Test);
            for (size_t k = 0; k < AreaCount; k++)
                if (Overlap(Test, E.AreaSurfaces[k + 1].Bounds))
                    CHECK(Bits[k / 32] & (1u << (k % 32)));
            Queries++;
        }
    std::fprintf(stderr, "   %zu sections, %zu area surfaces, %zu octree nodes, %d queries\n",
                 Out.Sections.size(), AreaCount, E.NodeAt.size(), Queries);
}

void CheckRefusals(const std::vector<uint8_t>& Set)
{
    Cooker::SOutput Out;
    std::string Error;
    auto Model = FloorAndWall();
    Model.Surfaces[0].Corners[0].Position = 999;
    CHECK(!Cooker::Write(Set, {Model}, Out, &Error) && Error.find("past its array") != std::string::npos);

    Model = FloorAndWall();
    Model.Surfaces[0].Corners.pop_back();
    CHECK(!Cooker::Write(Set, {Model}, Out, &Error) && Error.find("multiple of 3") != std::string::npos);

    Model = FloorAndWall();
    Model.Surfaces[0].MaterialIndex = 7;
    CHECK(!Cooker::Write(Set, {Model}, Out, &Error) && Error.find("past the material set") != std::string::npos);

    const auto MtxSet = MaterialSet({Material(0x80, kDescPosNrm | 0x01000000u)});
    Model = FloorAndWall();
    for (auto& S : Model.Surfaces) S.MaterialIndex = 0;
    CHECK(!Cooker::Write(MtxSet, {Model}, Out, &Error) && Error.find("matrix") != std::string::npos);

    const auto Clr1Set = MaterialSet({Material(0x80, kDescPosNrm | Attr(3, kIdx16))});
    CHECK(!Cooker::Write(Clr1Set, {Model}, Out, &Error) && Error.find("CLR1") != std::string::npos);

    // An 8-bit index past 255.
    auto Ramp = LightmappedRamp();
    Ramp.Colors.resize(300, 0);
    Ramp.Surfaces[0].Corners[0].Color = 299;
    CHECK(!Cooker::Write(Set, {Ramp}, Out, &Error) && Error.find("255") != std::string::npos);

    std::vector<Cooker::SMaterialInfo> Info;
    CHECK(!Cooker::ParseMaterialSet({0, 0, 0, 9}, Info, &Error));
}

// ---------------------------------------------------------------------------- --check-mrea
// A room cooked elsewhere (the Blender add-on's Python writers): its geometry through EngineRead, its
// materials through PWE's own loader (what the World Editor shows), its collision through PWE's loader.
uint32_t BE32(const std::vector<uint8_t>& B, size_t At)
{
    return uint32_t(B[At]) << 24 | uint32_t(B[At + 1]) << 16 | uint32_t(B[At + 2]) << 8 | B[At + 3];
}

/** The TEV stage count of a material, walking its header as CCubeMaterial::SetCurrent does. */
uint32_t TevCount(const std::vector<uint8_t>& M)
{
    size_t p = 0;
    const uint32_t Flags = BE32(M, p); p += 4;
    const uint32_t Tex = BE32(M, p); p += 4 + 4 * Tex;
    p += 16;                                     // vertex description, two Echoes words, group
    if (Flags & 0x8) p += 4 + 4 * BE32(M, p);    // konst colours
    p += 4;                                      // blend
    if (Flags & 0x400) p += 4;                   // indirect texture slot
    p += 4 + 4 * BE32(M, p);                     // colour channels
    return BE32(M, p);
}

int CheckMrea(const char* Path)
{
    std::ifstream In(Path, std::ios::binary);
    const std::vector<uint8_t> File((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    CHECK(File.size() >= 0x80 && BE32(File, 0) == 0xDEADBEEF && BE32(File, 4) == 0x19);
    if (gFailures) return 1;
    std::vector<uint32_t> H;
    for (int i = 0; i < 29; i++) H.push_back(BE32(File, 4 * i));
    const uint32_t Count = H[16], Blocks = H[28];
    size_t Pos = 0x80;
    std::vector<uint32_t> Sizes;
    for (uint32_t i = 0; i < Count; i++) Sizes.push_back(BE32(File, Pos + 4 * i));
    Pos += (Count * 4 + 31) & ~31u;
    std::vector<std::vector<uint8_t>> Sections;
    const size_t TableAt = Pos;
    Pos += (Blocks * 16 + 31) & ~31u;
    for (uint32_t b = 0; b < Blocks; b++)
    {
        const uint32_t Buf = BE32(File, TableAt + 16 * b), Compressed = BE32(File, TableAt + 16 * b + 8), SecCount = BE32(File, TableAt + 16 * b + 12);
        CHECK(Compressed == 0);   // the add-on writes stored blocks
        size_t At = Pos;
        for (uint32_t i = 0; i < SecCount; i++)
        {
            const uint32_t Size = Sizes[Sections.size()];
            Sections.emplace_back(File.begin() + At, File.begin() + At + Size);
            At += Size;
        }
        Pos += (Buf + 31) & ~31u;
    }
    CHECK(Sections.size() == Count);
    if (gFailures) return 1;

    const std::vector<std::vector<uint8_t>> Geometry(Sections.begin() + H[17], Sections.begin() + H[18]);
    const SEngine E = EngineRead(Geometry, H[14]);
    CHECK(E.Ok);
    if (!E.Ok) return 1;
    size_t AreaCount = 0;
    for (const auto& M : E.Models) AreaCount += M.Surfaces.size();
    CHECK(E.AreaSurfaces.size() == AreaCount + 1 && E.MeshCount == AreaCount);
    CHECK(E.AreaSurfaces[0].Model == -1 && E.AreaSurfaces[0].Group == -1);
    std::vector<int> Seen(AreaCount, 0);
    size_t Triangles = 0;
    for (size_t m = 0; m < E.Models.size(); m++)
    {
        const auto& Model = E.Models[m];
        for (size_t s = 0; s < Model.Surfaces.size(); s++)
        {
            const auto& S = Model.Surfaces[s];
            Triangles += S.Corners.size() / 3;
            for (const auto& C : S.Corners) { CHECK(Contains(S.Bounds, C.Position)); CHECK(Contains(Model.Bounds, C.Position)); }
            const uint16_t Area = Model.Records[s * 2 + 2];
            CHECK(Area < AreaCount);
            if (Area >= AreaCount) continue;
            Seen[Area]++;
            const SAreaSurf& A = E.AreaSurfaces[Area + 1];
            CHECK(A.Model == int16_t(m) && Inside(S.Bounds, A.Bounds));
            const auto& G = Model.Groups;
            const uint16_t GroupCount = G[0];
            CHECK(A.Group >= 0 && A.Group < GroupCount);
            if (A.Group < 0 || A.Group >= GroupCount) continue;
            const uint16_t N = A.Group == 0 ? G[1] : G[A.Group + 1] - G[A.Group];
            const uint16_t Start = A.Group == 0 ? 0 : G[A.Group];
            bool Listed = false;
            for (uint16_t i = 0; i < N; i++) Listed |= G[GroupCount + 1 + Start + i] == s;
            CHECK(Listed);
            for (const CAABox& Test : {A.Bounds, CAABox(S.Center, S.Center)})
                CHECK(E.Query(Test)[Area / 32] & (1u << (Area % 32)));
        }
    }
    for (int v : Seen) CHECK(v == 1);

    // Materials through PWE's loader, with an empty resource store (textures load as null).
    // Never destroyed: ~CResourceStore closes a project, which a store made from a database path has not.
    static CResourceStore* pStore = new CResourceStore("/nonexistent-pwe-check/store.rdb");
    CMemoryInStream MatIn(Geometry[0].data(), uint32_t(Geometry[0].size()), std::endian::big);
    std::unique_ptr<CMaterialSet> pSet(CMaterialLoader::LoadMaterialSet(MatIn, EGame::Echoes, pStore));
    const uint32_t TexCount = BE32(Geometry[0], 0);
    const uint32_t MatCount = BE32(Geometry[0], 4 + 4 * TexCount);
    CHECK(pSet && pSet->NumMaterials() == MatCount);
    if (pSet)
    {
        const size_t Base = 8 + 4 * TexCount + 4 * MatCount;
        uint32_t Start = 0;
        for (uint32_t i = 0; i < MatCount && i < pSet->NumMaterials(); i++)
        {
            const uint32_t End = BE32(Geometry[0], 8 + 4 * TexCount + 4 * i);
            const std::vector<uint8_t> M(Geometry[0].begin() + Base + Start, Geometry[0].begin() + Base + End);
            const CMaterial* pMat = pSet->MaterialByIndex(i, false);
            CHECK(pMat && std::ranges::size(pMat->Passes()) == TevCount(M));
            CHECK(pMat && pMat->VtxDesc().HasFlag(EVertexAttribute::Position));
            Start = End;
        }
    }

    // Collision through PWE's loader.
    const auto& Coll = Sections[H[20]];
    CMemoryInStream CollIn(Coll.data(), uint32_t(Coll.size()), std::endian::big);
    auto pGroup = CCollisionLoader::LoadAreaCollision(CollIn);
    CHECK(pGroup != nullptr);
    size_t CollTris = pGroup ? (*pGroup->Meshes().begin())->GetIndexData().TriangleIndices.size() / 3 : 0;

    std::fprintf(stderr, "%s: %zu models, %zu surfaces, %zu triangles, %u materials, %zu collision triangles: %s\n", Path,
                 E.Models.size(), AreaCount, Triangles, MatCount, CollTris, gFailures ? "FAILED" : "ok");
    return gFailures ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    InitTestLog();
    if (argc == 3 && std::strcmp(argv[1], "--check-mrea") == 0)
        return CheckMrea(argv[2]);
    const auto Set = MaterialSet({Material(0x4082, kDescPosNrmTex0),
                                  Material(0x4082 | (1u << 13) | (1u << 11), kDescLightmapped),
                                  Material(0x4082, kDescPosNrm)},
                                 {0x11111111, 0x22222222});
    std::vector<Cooker::SMaterialInfo> Info;
    CHECK(Cooker::ParseMaterialSet(Set, Info));
    CHECK(Info.size() == 3 && Info[1].PackedLightmapUVs() && !Info[0].PackedLightmapUVs() && Info[2].VertexDescription == kDescPosNrm);

    CheckRoom("floor and wall", Set, {FloorAndWall()});
    CheckRoom("three models, lightmapped and tiled", Set, {FloorAndWall(), LightmappedRamp(), Tiles()});
    CheckRefusals(Set);

    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0)
    {
        // The sections of the three-model room, for the port's independent reader.
        Cooker::SOutput Out;
        Cooker::Write(Set, {FloorAndWall(), LightmappedRamp(), Tiles()}, Out);
        for (size_t i = 0; i < Out.Sections.size(); i++)
        {
            const std::string Path = std::string(argv[2]) + "/section" + std::to_string(i) + ".bin";
            if (FILE* f = std::fopen(Path.c_str(), "wb"))
            {
                std::fwrite(Out.Sections[i].data(), 1, Out.Sections[i].size(), f);
                std::fclose(f);
            }
        }
    }

    if (gFailures)
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    else
        std::fprintf(stderr, "all world geometry cooker checks passed\n");
    return gFailures ? 1 : 0;
}
