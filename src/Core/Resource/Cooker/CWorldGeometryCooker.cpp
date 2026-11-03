#include "Core/Resource/Cooker/CWorldGeometryCooker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace
{
void SetError(std::string* pOut, std::string Text)
{
    if (pOut)
        *pOut = std::move(Text);
}

/** A big-endian byte buffer (MREA data is big-endian). */
struct SBigEndian
{
    std::vector<uint8_t> Bytes;

    void U8(uint8_t V) { Bytes.push_back(V); }
    void U16(uint16_t V) { U8(uint8_t(V >> 8)); U8(uint8_t(V)); }
    void S16(int16_t V) { U16(static_cast<uint16_t>(V)); }
    void U32(uint32_t V) { U16(uint16_t(V >> 16)); U16(uint16_t(V)); }
    void F32(float V) { uint32_t Bits; std::memcpy(&Bits, &V, 4); U32(Bits); }
    void Vec(const CVector3f& V) { F32(V.X); F32(V.Y); F32(V.Z); }
    void Box(const CAABox& B) { Vec(B.Min()); Vec(B.Max()); }
    void PutU32(size_t At, uint32_t V) { for (int i = 0; i < 4; i++) Bytes[At + i] = uint8_t(V >> (24 - 8 * i)); }
    /** Sections and display lists are padded to 32 bytes (GXCallDisplayList takes 32-byte multiples;
     *  0x00 is GX_NOP). */
    void Pad32() { while (Bytes.size() % 32) U8(0); }
};

uint32_t ReadU32(const std::vector<uint8_t>& B, size_t At)
{
    return uint32_t(B[At]) << 24 | uint32_t(B[At + 1]) << 16 | uint32_t(B[At + 2]) << 8 | B[At + 3];
}

// GX attribute slots in the compressed description (CGX::SetVtxDescv_Compressed: GX_VA_POS + i,
// two bits each, 0 none / 1 direct / 2 8-bit index / 3 16-bit index).
enum EAttr { kPos = 0, kNrm = 1, kClr0 = 2, kClr1 = 3, kTex0 = 4, kNumAttr = 11 };

uint32_t AttrType(uint32_t Desc, int Attr) { return (Desc >> (Attr * 2)) & 3; }

bool BoxesOverlap(const CAABox& A, const CAABox& B)
{
    return A.Min().X <= B.Max().X && A.Max().X >= B.Min().X &&
           A.Min().Y <= B.Max().Y && A.Max().Y >= B.Min().Y &&
           A.Min().Z <= B.Max().Z && A.Max().Z >= B.Min().Z;
}

float Axis(const CVector3f& V, int i) { return i == 0 ? V.X : (i == 1 ? V.Y : V.Z); }
void SetAxis(CVector3f& V, int i, float F) { (i == 0 ? V.X : (i == 1 ? V.Y : V.Z)) = F; }

// CAreaRenderOctTree::Node::GetChildCount / GetNodeBounds, including its axis table for the
// two-axis splits (skAxes[flags * 3], skAxes[flags * 3 + 1]).
constexpr int kChildCounts[8] = {0, 2, 2, 4, 2, 4, 4, 8};
constexpr int kAxes[21] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, -1, -1, -1, 0, 2, 1, 2, 0, 1};

CAABox RenderChildBounds(const CAABox& Box, uint32_t Flags, int Child)
{
    CVector3f Min = Box.Min(), Max = Box.Max();
    const CVector3f Center = (Min + Max) * 0.5f;
    switch (Flags)
    {
    case 1: case 2: case 4:
    {
        const int a = Flags == 1 ? 0 : (Flags == 2 ? 1 : 2);
        if (Child == 0) SetAxis(Max, a, Axis(Center, a)); else SetAxis(Min, a, Axis(Center, a));
        break;
    }
    case 3: case 5: case 6:
    {
        const int a = kAxes[Flags * 3], b = kAxes[Flags * 3 + 1];
        if (Child & 1) SetAxis(Min, a, Axis(Center, a)); else SetAxis(Max, a, Axis(Center, a));
        if (Child & 2) SetAxis(Min, b, Axis(Center, b)); else SetAxis(Max, b, Axis(Center, b));
        break;
    }
    case 7:
        for (int i = 0; i < 3; i++)
        {
            if (Child & (1 << i)) SetAxis(Min, i, Axis(Center, i)); else SetAxis(Max, i, Axis(Center, i));
        }
        break;
    default:
        break;
    }
    return CAABox(Min, Max);
}

/** The render octree over the area surfaces (bit k = area surface k, i.e. SAreaSurface entry k + 1). */
struct SRenderOctreeBuilder
{
    struct SNode
    {
        std::vector<uint16_t> Surfaces;   // overlapping this node: its bitmap
        uint32_t Flags = 0;               // 0 = leaf
        std::vector<uint32_t> Children;   // node indices
    };

    const std::vector<CAABox>& SurfaceBounds;
    const SRenderOctreeSettings& Settings;
    std::vector<SNode> Nodes;

    uint32_t Build(const CAABox& Box, std::vector<uint16_t> Surfaces, uint32_t Depth)
    {
        const uint32_t Index = static_cast<uint32_t>(Nodes.size());
        Nodes.emplace_back();
        Nodes[Index].Surfaces = Surfaces;
        if (Surfaces.size() <= Settings.MaxLeafSurfaces || Depth >= Settings.MaxDepth)
            return Index;

        // Split the axes at least half as long as the longest; the Y+Z pair (flags 6) is avoided,
        // since the game's axis table gives it Z and X.
        const CVector3f Size = Box.Max() - Box.Min();
        const float Longest = std::max({Size.X, Size.Y, Size.Z});
        uint32_t Flags = 0;
        for (int i = 0; i < 3; i++)
        {
            if (Axis(Size, i) >= 0.5f * Longest && Axis(Size, i) > 0.f)
                Flags |= 1u << i;
        }
        if (Flags == 6)
            Flags = 7;
        if (Flags == 0)
            return Index;

        std::vector<std::vector<uint16_t>> ChildSets(kChildCounts[Flags]);
        bool Smaller = false;
        for (int Child = 0; Child < kChildCounts[Flags]; Child++)
        {
            const CAABox ChildBox = RenderChildBounds(Box, Flags, Child);
            for (const uint16_t Surface : Surfaces)
            {
                if (BoxesOverlap(SurfaceBounds[Surface], ChildBox))
                    ChildSets[Child].push_back(Surface);
            }
            Smaller |= ChildSets[Child].size() < Surfaces.size();
        }
        if (!Smaller)
            return Index;

        Nodes[Index].Flags = Flags;
        for (int Child = 0; Child < kChildCounts[Flags]; Child++)
        {
            const uint32_t ChildIndex = Build(RenderChildBounds(Box, Flags, Child), std::move(ChildSets[Child]), Depth + 1);
            Nodes[Index].Children.push_back(ChildIndex);
        }
        return Index;
    }
};

bool WriteRenderOctree(const std::vector<CAABox>& SurfaceBounds, const CAABox& Bounds, const SRenderOctreeSettings& Settings,
                       SBigEndian& Out, std::string* pOutError)
{
    SRenderOctreeBuilder Builder{SurfaceBounds, Settings, {}};
    std::vector<uint16_t> All(SurfaceBounds.size());
    for (size_t i = 0; i < All.size(); i++)
        All[i] = static_cast<uint16_t>(i);
    Builder.Build(Bounds, std::move(All), 0);

    const uint32_t MeshCount = static_cast<uint32_t>(SurfaceBounds.size());
    const uint32_t WordCount = (MeshCount + 31) / 32;   // mBitmapWordCount

    // Bitmaps, shared between nodes that cover the same surfaces.
    std::map<std::vector<uint32_t>, uint16_t> BitmapIndex;
    std::vector<std::vector<uint32_t>> Bitmaps;
    std::vector<uint16_t> NodeBitmap;
    for (const auto& Node : Builder.Nodes)
    {
        std::vector<uint32_t> Words(WordCount, 0);
        for (const uint16_t Surface : Node.Surfaces)
            Words[Surface >> 5] |= 1u << (Surface & 31);   // CAreaRenderOctTree::TestBit
        const auto [It, Added] = BitmapIndex.try_emplace(Words, static_cast<uint16_t>(Bitmaps.size()));
        if (Added)
            Bitmaps.push_back(Words);
        NodeBitmap.push_back(It->second);
    }
    if (Builder.Nodes.size() > 0xFFFF || Bitmaps.size() > 0xFFFF)
    {
        SetError(pOutError, "The render octree has more than 65535 nodes or bitmaps.");
        return false;
    }

    // Header (the constructor reads words 2-4 and the bounds at byte 20; bitmaps start at 64).
    Out.U32(0x41524F54);  // 'AROT'; the game skips words 0 and 1
    Out.U32(1);
    Out.U32(static_cast<uint32_t>(Bitmaps.size()));
    Out.U32(MeshCount);
    Out.U32(static_cast<uint32_t>(Builder.Nodes.size()));
    Out.Box(Bounds);
    while (Out.Bytes.size() < 64)
        Out.U8(0);
    for (const auto& Words : Bitmaps)
        for (const uint32_t Word : Words)
            Out.U32(Word);

    // Indirection table (an offset per node into the entries), then the entries:
    // u16 bitmap index, u8 (unknown, 0), u8 subdivision flags, u16 child node indices.
    const size_t TableAt = Out.Bytes.size();
    for (size_t i = 0; i < Builder.Nodes.size(); i++)
        Out.U32(0);
    const size_t EntriesAt = Out.Bytes.size();
    for (size_t i = 0; i < Builder.Nodes.size(); i++)
    {
        const auto& Node = Builder.Nodes[i];
        Out.PutU32(TableAt + 4 * i, static_cast<uint32_t>(Out.Bytes.size() - EntriesAt));
        Out.U16(NodeBitmap[i]);
        Out.U8(0);
        Out.U8(static_cast<uint8_t>(Node.Flags));
        for (const uint32_t Child : Node.Children)
            Out.U16(static_cast<uint16_t>(Child));
    }
    Out.Pad32();
    return true;
}

int16_t ShortNormal(float F)
{
    // GX_S16 with 14 fraction bits (GX_VTXFMT1/2 GX_VA_NRM).
    return static_cast<int16_t>(std::clamp(std::lround(F * 16384.f), -32768l, 32767l));
}

uint16_t PackedUV(float F)
{
    // GX_U16 with 15 fraction bits (GX_VTXFMT2 GX_VA_TEX0).
    return static_cast<uint16_t>(std::clamp(std::lround(F * 32768.f), 0l, 65535l));
}
}

bool CWorldGeometryCooker::ParseMaterialSet(const std::vector<uint8_t>& Set, std::vector<SMaterialInfo>& OutInfo, std::string* pOutError)
{
    // CMetroidModelInstance::GetMaterialByIndex: texture count and IDs, material count and each
    // material's end offset, then the materials. A material starts with its flags, its texture
    // count and indices, then the vertex description (CCubeMaterial::SetCurrent).
    OutInfo.clear();
    auto Fail = [&](const char* Why) { SetError(pOutError, std::string("Material set: ") + Why); return false; };
    if (Set.size() < 8)
        return Fail("too short.");
    const uint64_t TexCount = ReadU32(Set, 0);
    const uint64_t CountAt = 4 + 4 * TexCount;
    if (CountAt + 4 > Set.size())
        return Fail("texture list runs past the end.");
    const uint64_t MatCount = ReadU32(Set, CountAt);
    const uint64_t MatsAt = CountAt + 4 + 4 * MatCount;
    if (MatsAt > Set.size())
        return Fail("material offsets run past the end.");

    uint64_t Start = MatsAt;
    for (uint64_t i = 0; i < MatCount; i++)
    {
        const uint64_t End = MatsAt + ReadU32(Set, CountAt + 4 + 4 * i);
        if (End > Set.size() || End < Start + 12)
            return Fail("a material is shorter than its header or runs past the end.");
        const uint64_t MatTexCount = ReadU32(Set, Start + 4);
        const uint64_t DescAt = Start + 8 + 4 * MatTexCount;
        if (DescAt + 4 > End)
            return Fail("a material's texture list runs past it.");
        OutInfo.push_back({ReadU32(Set, Start), ReadU32(Set, DescAt)});
        Start = End;
    }
    return true;
}

bool CWorldGeometryCooker::Write(const std::vector<uint8_t>& MaterialSet, const std::vector<SModel>& Models, SOutput& Out,
                                 std::string* pOutError, const SRenderOctreeSettings& Settings)
{
    Out = SOutput();
    std::vector<SMaterialInfo> Materials;
    if (!ParseMaterialSet(MaterialSet, Materials, pOutError))
        return false;

    // Every surface's material has to describe a vertex this writer can produce.
    for (size_t m = 0; m < Models.size(); m++)
    {
        for (const SSurface& Surface : Models[m].Surfaces)
        {
            if (Surface.MaterialIndex >= Materials.size())
            {
                SetError(pOutError, "A surface uses a material index past the material set.");
                return false;
            }
            const uint32_t Desc = Materials[Surface.MaterialIndex].VertexDescription;
            if (Desc & 0xFF000000)
            {
                SetError(pOutError, "A material asks for per-vertex matrix indices, which world geometry does not use here.");
                return false;
            }
            if (AttrType(Desc, kPos) < 2)
            {
                SetError(pOutError, "A material has no indexed positions.");
                return false;
            }
            for (int Attr = 0; Attr < 12; Attr++)
            {
                const uint32_t Type = AttrType(Desc, Attr);
                if (Type == 1 || (Type != 0 && (Attr == kClr1 || Attr == 11)))
                {
                    // Direct data, CLR1 (CCubeModel sets no CLR1 array) and TEX7 (SetVtxDescv_Compressed
                    // stops at TEX6) cannot be drawn from world model arrays.
                    SetError(pOutError, "A material uses direct vertex data, CLR1 or TEX7.");
                    return false;
                }
            }
            if (Surface.Corners.empty() || Surface.Corners.size() % 3 != 0)
            {
                SetError(pOutError, "A surface's corner count is not a positive multiple of 3.");
                return false;
            }
        }
    }

    SBigEndian MaterialSection;
    MaterialSection.Bytes = MaterialSet;
    MaterialSection.Pad32();
    Out.Sections.push_back(std::move(MaterialSection.Bytes));

    std::vector<CAABox> AreaSurfaceBounds;
    std::vector<std::pair<int16_t, int16_t>> AreaSurfaceOwner;   // (model, group)
    Out.Bounds = CAABox::Infinite();

    for (size_t ModelIdx = 0; ModelIdx < Models.size(); ModelIdx++)
    {
        const SModel& Model = Models[ModelIdx];
        if (Model.Surfaces.empty() || Model.Surfaces.size() > 0xFFFF)
        {
            SetError(pOutError, "A world model has no surfaces, or more than 65535.");
            return false;
        }
        // Every index in a display list is at most 16 bits.
        if (std::max({Model.Positions.size(), Model.Normals.size(), Model.Colors.size(), Model.TexCoords.size(),
                      Model.LightmapTexCoords.size()}) > 0x10000)
        {
            SetError(pOutError, "A world model has an array of more than 65536 entries.");
            return false;
        }

        // GX_VTXFMT2 (packed lightmap UVs) reads normals as S16, so a model with such a material gets
        // short normals throughout (flag bit 0, CCubeModel::SetArraysCurrent) and its other surfaces
        // use GX_VTXFMT1; otherwise everything is float and GX_VTXFMT0.
        bool ShortNormals = false;
        for (const SSurface& Surface : Model.Surfaces)
            ShortNormals |= Materials[Surface.MaterialIndex].PackedLightmapUVs();

        CAABox ModelBounds = CAABox::Infinite();
        for (const SSurface& Surface : Model.Surfaces)
            for (const SCorner& Corner : Surface.Corners)
                if (Corner.Position < Model.Positions.size())
                    ModelBounds.ExpandBounds(Model.Positions[Corner.Position]);

        // 0: header (flags, transform, bounds)
        SBigEndian Header;
        Header.U32((Model.ExtraFlags & ~1u) | (ShortNormals ? 1u : 0u));
        for (const float F : Model.Transform)
            Header.F32(F);
        Header.Box(ModelBounds);
        Header.Pad32();
        Out.Sections.push_back(std::move(Header.Bytes));

        // 1-5: positions, normals, colours, float UVs, packed UVs
        SBigEndian Section;
        for (const CVector3f& P : Model.Positions)
            Section.Vec(P);
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));

        Section = {};
        for (const CVector3f& N : Model.Normals)
        {
            if (ShortNormals) { Section.S16(ShortNormal(N.X)); Section.S16(ShortNormal(N.Y)); Section.S16(ShortNormal(N.Z)); }
            else Section.Vec(N);
        }
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));

        Section = {};
        for (const uint32_t C : Model.Colors)
            Section.U32(C);
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));

        Section = {};
        for (const CVector2f& UV : Model.TexCoords) { Section.F32(UV.X); Section.F32(UV.Y); }
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));

        Section = {};
        for (const CVector2f& UV : Model.LightmapTexCoords) { Section.U16(PackedUV(UV.X)); Section.U16(PackedUV(UV.Y)); }
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));

        // 6: the surface count (FillInStaticGeometry reads it) and each surface's end offset
        std::vector<std::vector<uint8_t>> SurfaceSections;
        for (size_t SurfIdx = 0; SurfIdx < Model.Surfaces.size(); SurfIdx++)
        {
            const SSurface& Surface = Model.Surfaces[SurfIdx];
            const SMaterialInfo& Material = Materials[Surface.MaterialIndex];
            const uint32_t Desc = Material.VertexDescription;
            const uint8_t Format = ShortNormals ? (Material.PackedLightmapUVs() ? 2 : 1) : 0;

            // Bounds and the normal hint (CCubeMaterial reflects about the plane through the
            // centre with this normal); the area-weighted face normal.
            CAABox Bounds = CAABox::Infinite();
            CVector3f FaceSum(0.f, 0.f, 0.f);
            for (size_t c = 0; c < Surface.Corners.size(); c++)
            {
                const SCorner& Corner = Surface.Corners[c];
                auto Check = [&](int Attr, uint32_t Index, size_t Count) {
                    const uint32_t Type = AttrType(Desc, Attr);
                    return Type == 0 || (Index < Count && (Type == 3 || Index < 0x100));
                };
                bool Ok = Check(kPos, Corner.Position, Model.Positions.size()) &&
                          Check(kNrm, Corner.Normal, Model.Normals.size()) &&
                          Check(kClr0, Corner.Color, Model.Colors.size());
                for (int t = 0; t < 7 && Ok; t++)
                {
                    const size_t Count = (t == 0 && Material.PackedLightmapUVs()) ? Model.LightmapTexCoords.size() : Model.TexCoords.size();
                    Ok = Check(kTex0 + t, Corner.Tex[t], Count);
                }
                if (!Ok)
                {
                    SetError(pOutError, "A corner's index is past its array, or past 255 for an 8-bit index.");
                    return false;
                }
                Bounds.ExpandBounds(Model.Positions[Corner.Position]);
                if (c % 3 == 2)
                {
                    const CVector3f& A = Model.Positions[Surface.Corners[c - 2].Position];
                    const CVector3f& B = Model.Positions[Surface.Corners[c - 1].Position];
                    const CVector3f& C = Model.Positions[Corner.Position];
                    FaceSum += (B - A).Cross(C - A);
                }
            }
            const float Length = FaceSum.Magnitude();
            const CVector3f NormalHint = Length > 1e-12f ? FaceSum / Length : CVector3f(1.f, 0.f, 0.f);  // skDefaultNormal

            // Display list: GX_TRIANGLES (0x90) | vertex format, a u16 vertex count, then per vertex the
            // index of each attribute the description has, in GX order (POS, NRM, CLR0, TEX0..6).
            SBigEndian List;
            const size_t MaxPerPrimitive = 0xFFFF / 3 * 3;
            for (size_t First = 0; First < Surface.Corners.size(); First += MaxPerPrimitive)
            {
                const size_t Count = std::min(MaxPerPrimitive, Surface.Corners.size() - First);
                List.U8(static_cast<uint8_t>(0x90 | Format));
                List.U16(static_cast<uint16_t>(Count));
                for (size_t c = First; c < First + Count; c++)
                {
                    const SCorner& Corner = Surface.Corners[c];
                    const uint16_t Indices[kNumAttr] = {Corner.Position, Corner.Normal, Corner.Color, 0, Corner.Tex[0], Corner.Tex[1],
                                                        Corner.Tex[2], Corner.Tex[3], Corner.Tex[4], Corner.Tex[5], Corner.Tex[6]};
                    for (int Attr = 0; Attr < kNumAttr; Attr++)
                    {
                        const uint32_t Type = AttrType(Desc, Attr);
                        if (Type == 3) List.U16(Indices[Attr]);
                        else if (Type == 2) List.U8(static_cast<uint8_t>(Indices[Attr]));
                    }
                }
            }
            List.Pad32();

            // Surface header (CCubeSurface, 0x30 bytes, then the 0x18-byte bounds as its "extra" data),
            // padded to 32: GetSurfaceHeaderSize = (0x4f + extra) & ~31 = 0x60.
            SBigEndian Surf;
            Surf.Vec((Bounds.Min() + Bounds.Max()) * 0.5f);
            Surf.U32(Surface.MaterialIndex);
            Surf.U32(0x80000000u | static_cast<uint32_t>(List.Bytes.size()));
            Surf.U32(0);      // parent (set at load)
            Surf.U32(0);      // next surface (set at load)
            Surf.U32(0x18);   // extra size: the bounds follow
            Surf.Vec(NormalHint);
            Surf.S16(0);      // matrix bank (skinned models only)
            Surf.S16(0);      // shadow bank (groups surfaces for the silhouette pass)
            Surf.Box(Bounds);
            Surf.Pad32();
            Surf.Bytes.insert(Surf.Bytes.end(), List.Bytes.begin(), List.Bytes.end());
            SurfaceSections.push_back(std::move(Surf.Bytes));

            if (AreaSurfaceBounds.size() >= 0xFFFF)
            {
                SetError(pOutError, "More than 65535 surfaces in the room.");
                return false;
            }
            AreaSurfaceBounds.push_back(Bounds);
            AreaSurfaceOwner.emplace_back(static_cast<int16_t>(ModelIdx), static_cast<int16_t>(SurfIdx));
            Out.Bounds.ExpandBounds(Bounds);
        }

        Section = {};
        Section.U32(static_cast<uint32_t>(SurfaceSections.size()));
        uint32_t EndOffset = 0;
        for (const auto& Surf : SurfaceSections)
        {
            EndOffset += static_cast<uint32_t>(Surf.size());
            Section.U32(EndOffset);
        }
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));
        for (auto& Surf : SurfaceSections)
            Out.Sections.push_back(std::move(Surf));

        // Surface records (GetSurfaceRecords): a u16 count, then per surface two u16s, the second its
        // area surface index (GetSurfaceAreaIndex). The first is read by nothing; it is written as 0.
        const size_t FirstArea = AreaSurfaceBounds.size() - Model.Surfaces.size();
        Section = {};
        Section.U16(static_cast<uint16_t>(Model.Surfaces.size()));
        for (size_t s = 0; s < Model.Surfaces.size(); s++)
        {
            Section.U16(0);
            Section.U16(static_cast<uint16_t>(FirstArea + s));
        }
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));

        // Surface groups (CSurfaceGroups): a u16 group count, each group's cumulative end, then the
        // surface indices. Group s holds surface s alone.
        Section = {};
        Section.U16(static_cast<uint16_t>(Model.Surfaces.size()));
        for (size_t s = 0; s < Model.Surfaces.size(); s++)
            Section.U16(static_cast<uint16_t>(s + 1));
        for (size_t s = 0; s < Model.Surfaces.size(); s++)
            Section.U16(static_cast<uint16_t>(s));
        Section.Pad32();
        Out.Sections.push_back(std::move(Section.Bytes));
    }

    if (AreaSurfaceBounds.empty())
    {
        SetError(pOutError, "There are no world models.");
        return false;
    }

    // Render octree. The renderer dereferences it without a check (CCubeRenderer's light setup), and
    // FillInStaticGeometry expects the area surface list right after it, so it is always written.
    SBigEndian Octree;
    if (!WriteRenderOctree(AreaSurfaceBounds, Out.Bounds, Settings, Octree, pOutError))
        return false;
    Out.Sections.push_back(std::move(Octree.Bytes));

    // Area surfaces (rstl::vector<SAreaSurface>): a count, then bounds, model, group and two unused
    // shorts each. Entry 0 is a placeholder (model and group -1, which every reader skips); entry
    // k + 1 is area surface k.
    SBigEndian AreaSurfaces;
    AreaSurfaces.U32(static_cast<uint32_t>(AreaSurfaceBounds.size() + 1));
    AreaSurfaces.Box(Out.Bounds);
    AreaSurfaces.S16(-1); AreaSurfaces.S16(-1); AreaSurfaces.S16(0); AreaSurfaces.S16(0);
    for (size_t k = 0; k < AreaSurfaceBounds.size(); k++)
    {
        AreaSurfaces.Box(AreaSurfaceBounds[k]);
        AreaSurfaces.S16(AreaSurfaceOwner[k].first);
        AreaSurfaces.S16(AreaSurfaceOwner[k].second);
        AreaSurfaces.S16(0); AreaSurfaces.S16(0);
    }
    AreaSurfaces.Pad32();
    Out.Sections.push_back(std::move(AreaSurfaces.Bytes));

    // Ambient light IDs and per-area-surface indices: both empty (the renderer then gives no
    // surface an ambient level from them).
    SBigEndian Ambient;
    Ambient.U32(0);
    Ambient.U32(0);
    Ambient.Pad32();
    Out.Sections.push_back(std::move(Ambient.Bytes));

    Out.ModelCount = static_cast<uint32_t>(Models.size());
    Out.AreaSurfaceCount = static_cast<uint32_t>(AreaSurfaceBounds.size());
    return true;
}
