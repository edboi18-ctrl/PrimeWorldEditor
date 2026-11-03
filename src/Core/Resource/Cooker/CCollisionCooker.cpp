#include "Core/Resource/Cooker/CCollisionCooker.h"

#include "Core/Resource/Factory/CCollisionLoader.h"
#include <Common/FileIO/CVectorOutStream.h>
#include <Common/FileIO/IOutputStream.h>
#include <Common/Math/CAABox.h>

#include <algorithm>
#include <map>
#include <utility>

namespace
{
constexpr uint16_t kNoNeighbor = 0xFFFF;
constexpr size_t kMaxIndex = 0xFFFF;      // every index in the format is a u16
constexpr size_t kMaxMaterials = 0xFF;    // material indices are u8

// CAreaOctTree::Node::ETreeType
enum ETreeType : uint32_t { kTT_Invalid = 0, kTT_Branch = 1, kTT_Leaf = 2 };

void SetError(std::string* pOut, std::string Text)
{
    if (pOut)
        *pOut = std::move(Text);
}

/** BoxFromIndex (CAreaOctTree.cpp): child i takes the upper half on X if bit 0, Y if bit 1, Z if bit 2. */
CAABox ChildBox(const CAABox& Box, uint32_t Index)
{
    const CVector3f Center = (Box.Min() + Box.Max()) * 0.5f;
    CVector3f Min = Box.Min(), Max = Center;
    if (Index & 1) { Min.X = Center.X; Max.X = Box.Max().X; }
    if (Index & 2) { Min.Y = Center.Y; Max.Y = Box.Max().Y; }
    if (Index & 4) { Min.Z = Center.Z; Max.Z = Box.Max().Z; }
    return CAABox(Min, Max);
}

bool Overlaps(const CAABox& A, const CAABox& B)
{
    return A.Min().X <= B.Max().X && A.Max().X >= B.Min().X &&
           A.Min().Y <= B.Max().Y && A.Max().Y >= B.Min().Y &&
           A.Min().Z <= B.Max().Z && A.Max().Z >= B.Min().Z;
}

void WriteBox(IOutputStream& Out, const CAABox& Box)
{
    Out.WriteF32(Box.Min().X); Out.WriteF32(Box.Min().Y); Out.WriteF32(Box.Min().Z);
    Out.WriteF32(Box.Max().X); Out.WriteF32(Box.Max().Y); Out.WriteF32(Box.Max().Z);
}

/** The three vertex indices of a triangle, rebuilt the way CCollisionPrimitiveData::GetTriangle does. */
std::array<uint16_t, 3> TriangleVertices(const SCollisionIndexData& Data, size_t Tri)
{
    const uint16_t E0 = Data.TriangleIndices[Tri * 3], E1 = Data.TriangleIndices[Tri * 3 + 1];
    const uint16_t A1 = Data.EdgeIndices[E0 * 2], A2 = Data.EdgeIndices[E0 * 2 + 1];
    const uint16_t B1 = Data.EdgeIndices[E1 * 2], B2 = Data.EdgeIndices[E1 * 2 + 1];
    const auto& Material = Data.Materials[Data.TriangleMaterialIndices[Tri]];
    if (Material.RawFlags() & CCollisionCooker::kEchoesFlippedTriangle)
        return {A2, A1, B1};
    return {A1, A2, B2};
}

struct SOctreeBuilder
{
    const SCollisionIndexData& Data;
    const CCollisionCooker::SOctreeSettings& Settings;
    std::vector<CAABox> TriBoxes;

    CAABox TightBox(const std::vector<uint16_t>& Tris) const
    {
        CAABox Box = CAABox::Infinite();
        for (const uint16_t Tri : Tris)
            Box.ExpandBounds(TriBoxes[Tri]);
        return Box;
    }

    /** Leaf: its own bounds, a u16 count, the triangle indices; padded so the next node stays word-aligned. */
    void WriteLeaf(IOutputStream& Out, const std::vector<uint16_t>& Tris) const
    {
        WriteBox(Out, TightBox(Tris));
        Out.WriteU16(static_cast<uint16_t>(Tris.size()));
        for (const uint16_t Tri : Tris)
            Out.WriteU16(Tri);
        if (Tris.size() % 2 == 0)
            Out.WriteU16(0);
    }

    /** One node (leaf or branch) for Tris inside Box; returns its type. */
    ETreeType WriteNode(IOutputStream& Out, const CAABox& Box, const std::vector<uint16_t>& Tris, uint32_t Depth) const
    {
        const bool MakeLeaf = Tris.size() <= Settings.MaxLeafTriangles || Depth >= Settings.MaxDepth;
        std::array<std::vector<uint16_t>, 8> Children;
        bool Splits = false;

        if (!MakeLeaf)
        {
            for (uint32_t Child = 0; Child < 8; Child++)
            {
                const CAABox ChildBounds = ChildBox(Box, Child);
                for (const uint16_t Tri : Tris)
                {
                    if (Overlaps(TriBoxes[Tri], ChildBounds))
                        Children[Child].push_back(Tri);
                }
                Splits |= Children[Child].size() < Tris.size();
            }
        }

        // A split that gives every child every triangle (one huge triangle, say) never gets smaller.
        if (MakeLeaf || !Splits)
        {
            if (Tris.size() > kMaxIndex)
                return kTT_Invalid;
            WriteLeaf(Out, Tris);
            return kTT_Leaf;
        }

        // Branch (Node::GetChild): u16 child types, 2 bits each; then 8 u32 offsets, each from the
        // end of this 36-byte header to the child.
        CVectorOutStream Body(std::endian::big);
        std::array<uint32_t, 8> Offsets{};
        uint16_t Types = 0;

        for (uint32_t Child = 0; Child < 8; Child++)
        {
            if (Children[Child].empty())
                continue;
            Offsets[Child] = Body.Size();
            const ETreeType Type = WriteNode(Body, ChildBox(Box, Child), Children[Child], Depth + 1);
            if (Type == kTT_Invalid)
                return kTT_Invalid;
            Types |= static_cast<uint16_t>(Type << (Child * 2));
        }

        Out.WriteU16(Types);
        Out.WriteU16(0);
        for (const uint32_t Offset : Offsets)
            Out.WriteU32(Offset);
        Out.WriteBytes(Body.Data(), Body.Size());
        return kTT_Branch;
    }
};
}

bool CCollisionCooker::BuildIndexData(const std::vector<STriangle>& Triangles, const std::vector<uint64_t>& MaterialFlags,
                                      EGame Game, SCollisionIndexData& OutData, std::string* pOutError)
{
    if (Game != EGame::Echoes)
    {
        SetError(pOutError, "Only Echoes area collision can be cooked so far.");
        return false;
    }
    if (MaterialFlags.size() > kMaxMaterials)
    {
        SetError(pOutError, "More than 255 collision materials.");
        return false;
    }
    OutData = SCollisionIndexData();

    // Vertices, shared when exactly equal.
    std::map<std::array<float, 3>, uint16_t> VertexIndex;
    std::vector<std::array<uint16_t, 3>> TriVerts;
    TriVerts.reserve(Triangles.size());

    for (const STriangle& Tri : Triangles)
    {
        if (Tri.MaterialIndex >= MaterialFlags.size())
        {
            SetError(pOutError, "A triangle uses a material index past the material list.");
            return false;
        }
        std::array<uint16_t, 3> Indices{};
        for (size_t i = 0; i < 3; i++)
        {
            const CVector3f& V = Tri.Vertices[i];
            const auto [It, Added] = VertexIndex.try_emplace({V.X, V.Y, V.Z}, static_cast<uint16_t>(OutData.Vertices.size()));
            if (Added)
            {
                if (OutData.Vertices.size() >= kMaxIndex)
                {
                    SetError(pOutError, "More than 65535 collision vertices.");
                    return false;
                }
                OutData.Vertices.push_back(V);
            }
            Indices[i] = It->second;
        }
        TriVerts.push_back(Indices);
    }

    // Edges, stored in the direction of the first triangle that uses them.
    std::map<std::pair<uint16_t, uint16_t>, uint16_t> EdgeIndex;  // keyed by (lower, higher) vertex
    auto EdgeFor = [&](uint16_t From, uint16_t To, bool& OutForward) -> int
    {
        const auto Key = std::minmax(From, To);
        const auto [It, Added] = EdgeIndex.try_emplace({Key.first, Key.second}, static_cast<uint16_t>(OutData.EdgeIndices.size() / 2));
        if (Added)
        {
            if (OutData.EdgeIndices.size() / 2 >= kMaxIndex)
                return -1;
            OutData.EdgeIndices.push_back(From);
            OutData.EdgeIndices.push_back(To);
        }
        OutForward = OutData.EdgeIndices[It->second * 2] == From;
        return It->second;
    };

    // Materials: the given ones, then a flipped variant of each as needed.
    for (const uint64_t Flags : MaterialFlags)
    {
        CCollisionMaterial Material;
        CCollisionLoader::DecodeMaterial(Flags & ~kEchoesFlippedTriangle, Game, Material);
        OutData.Materials.push_back(Material);
    }
    std::map<uint32_t, uint8_t> FlippedVariant;

    OutData.VertexMaterialIndices.assign(OutData.Vertices.size(), 0xFF);
    std::vector<uint8_t> EdgeMaterials;

    for (size_t TriIdx = 0; TriIdx < Triangles.size(); TriIdx++)
    {
        const auto& V = TriVerts[TriIdx];
        if (V[0] == V[1] || V[1] == V[2] || V[2] == V[0])
        {
            SetError(pOutError, "A collision triangle has two identical corners.");
            return false;
        }

        // The three edges in winding order (v0-v1, v1-v2, v2-v0) and whether each is stored that way.
        std::array<int, 3> Edges{};
        std::array<bool, 3> Forward{};
        for (size_t i = 0; i < 3; i++)
        {
            Edges[i] = EdgeFor(V[i], V[(i + 1) % 3], Forward[i]);
            if (Edges[i] < 0)
            {
                SetError(pOutError, "More than 65535 collision edges.");
                return false;
            }
        }

        // GetTriangle rebuilds the corners from the first two edges: both stored forward give
        // (e0.v1, e0.v2, e1.v2), both backward with the flipped flag give (e0.v2, e0.v1, e1.v1).
        // CCollisionTracker numbers a triangle's edges v0-v1, v1-v2, v2-v0 (FindExitEdge) and looks
        // them up in this triple (FindSharedEdge), so the triple stays in that order: start the
        // triangle at an edge whose successor runs the same way. Three edges with two directions
        // always have such a pair.
        size_t Start = 0;
        while (Forward[Start] != Forward[(Start + 1) % 3])
            Start++;
        const bool Flipped = !Forward[Start];

        for (size_t i = 0; i < 3; i++)
            OutData.TriangleIndices.push_back(static_cast<uint16_t>(Edges[(Start + i) % 3]));

        uint32_t Material = Triangles[TriIdx].MaterialIndex;
        if (Flipped)
        {
            const auto [It, Added] = FlippedVariant.try_emplace(Material, static_cast<uint8_t>(OutData.Materials.size()));
            if (Added)
            {
                if (OutData.Materials.size() >= kMaxMaterials)
                {
                    SetError(pOutError, "More than 255 collision materials once flipped variants are added.");
                    return false;
                }
                CCollisionMaterial Variant;
                CCollisionLoader::DecodeMaterial((MaterialFlags[Material] & ~kEchoesFlippedTriangle) | kEchoesFlippedTriangle, Game, Variant);
                OutData.Materials.push_back(Variant);
            }
            Material = It->second;
        }
        if (Material > kMaxMaterials - 1)
        {
            SetError(pOutError, "More than 255 collision materials.");
            return false;
        }
        OutData.TriangleMaterialIndices.push_back(static_cast<uint8_t>(Material));

        // Vertices and edges take the first unflipped material that touches them: bit 24 is the
        // flipped flag on a triangle but kMT_NoEdgeCollision on an edge.
        const auto Base = static_cast<uint8_t>(Triangles[TriIdx].MaterialIndex);
        for (const uint16_t Vertex : V)
        {
            if (OutData.VertexMaterialIndices[Vertex] == 0xFF)
                OutData.VertexMaterialIndices[Vertex] = Base;
        }
        EdgeMaterials.resize(OutData.EdgeIndices.size() / 2, 0xFF);
        for (const int Edge : Edges)
        {
            if (EdgeMaterials[Edge] == 0xFF)
                EdgeMaterials[Edge] = Base;
        }
    }

    OutData.EdgeMaterialIndices = std::move(EdgeMaterials);
    OutData.TriangleNeighbors = BuildTriangleNeighbors(OutData);
    return true;
}

std::unique_ptr<CCollisionMesh> CCollisionCooker::BuildMesh(SCollisionIndexData&& Data)
{
    auto pMesh = std::make_unique<CCollisionMesh>();
    pMesh->mAABox = CAABox::Infinite();
    for (const CVector3f& Vertex : Data.Vertices)
        pMesh->mAABox.ExpandBounds(Vertex);
    pMesh->mIndexData = std::move(Data);
    return pMesh;
}

std::vector<uint16_t> CCollisionCooker::BuildTriangleNeighbors(const SCollisionIndexData& Data)
{
    const size_t NumTris = Data.TriangleIndices.size() / 3;
    std::vector<std::vector<uint16_t>> EdgeUsers(Data.EdgeIndices.size() / 2);
    for (size_t Tri = 0; Tri < NumTris; Tri++)
    {
        for (size_t i = 0; i < 3; i++)
            EdgeUsers[Data.TriangleIndices[Tri * 3 + i]].push_back(static_cast<uint16_t>(Tri));
    }

    std::vector<uint16_t> Neighbors(NumTris * 3, kNoNeighbor);
    for (size_t Tri = 0; Tri < NumTris; Tri++)
    {
        for (size_t i = 0; i < 3; i++)
        {
            for (const uint16_t Other : EdgeUsers[Data.TriangleIndices[Tri * 3 + i]])
            {
                if (Other != Tri)
                {
                    Neighbors[Tri * 3 + i] = Other;
                    break;
                }
            }
        }
    }
    return Neighbors;
}

bool CCollisionCooker::WriteOctreeCollision(const SCollisionIndexData& Data, const CAABox& Bounds, IOutputStream& Out, EGame Game,
                                            const SOctreeSettings& Settings, std::string* pOutError)
{
    if (Game != EGame::Echoes)
    {
        SetError(pOutError, "Only Echoes area collision can be cooked so far.");
        return false;
    }
    const size_t NumTris = Data.TriangleIndices.size() / 3;
    if (NumTris == 0 || Data.TriangleMaterialIndices.size() != NumTris ||
        Data.VertexMaterialIndices.size() != Data.Vertices.size() || Data.EdgeMaterialIndices.size() != Data.EdgeIndices.size() / 2)
    {
        SetError(pOutError, "The collision index data is empty or its arrays disagree in length.");
        return false;
    }
    if (NumTris > kMaxIndex)
    {
        SetError(pOutError, "More than 65535 collision triangles.");
        return false;
    }

    SOctreeBuilder Builder{Data, Settings, {}};
    Builder.TriBoxes.reserve(NumTris);
    std::vector<uint16_t> AllTris(NumTris);
    for (size_t Tri = 0; Tri < NumTris; Tri++)
    {
        CAABox Box = CAABox::Infinite();
        for (const uint16_t Vertex : TriangleVertices(Data, Tri))
            Box.ExpandBounds(Data.Vertices[Vertex]);
        Builder.TriBoxes.push_back(Box);
        AllTris[Tri] = static_cast<uint16_t>(Tri);
    }

    CVectorOutStream Tree(std::endian::big);
    const ETreeType RootType = Builder.WriteNode(Tree, Bounds, AllTris, 0);
    if (RootType == kTT_Invalid)
    {
        SetError(pOutError, "An octree leaf would hold more than 65535 triangles.");
        return false;
    }

    // CAreaOctTree::MakeFromMemory: two words it skips (magic, version), the bounds, the root's
    // type, the tree's size, the tree, then the arrays, packed with no padding between them.
    Out.WriteU32(0xDEAFBABE);
    Out.WriteU32(0x4);  // CCollisionLoader::GetFormatVersion: 4 is Echoes
    WriteBox(Out, Bounds);
    Out.WriteU32(RootType);
    Out.WriteU32(Tree.Size());
    Out.WriteBytes(Tree.Data(), Tree.Size());

    Out.WriteU32(static_cast<uint32_t>(Data.Materials.size()));
    for (const CCollisionMaterial& Material : Data.Materials)
        Out.WriteU64(Material.RawFlags());

    for (const auto* pArray : {&Data.VertexMaterialIndices, &Data.EdgeMaterialIndices, &Data.TriangleMaterialIndices})
    {
        Out.WriteU32(static_cast<uint32_t>(pArray->size()));
        Out.WriteBytes(pArray->data(), static_cast<uint32_t>(pArray->size()));
    }

    Out.WriteU32(static_cast<uint32_t>(Data.EdgeIndices.size() / 2));
    for (const uint16_t Index : Data.EdgeIndices)
        Out.WriteU16(Index);

    Out.WriteU32(static_cast<uint32_t>(Data.TriangleIndices.size()));
    for (const uint16_t Index : Data.TriangleIndices)
        Out.WriteU16(Index);

    // Echoes: three neighbours per triangle (the reader takes as many as there are triangle indices).
    const std::vector<uint16_t> Neighbors = Data.TriangleNeighbors.size() == Data.TriangleIndices.size()
                                                ? Data.TriangleNeighbors : BuildTriangleNeighbors(Data);
    Out.WriteU32(static_cast<uint32_t>(Neighbors.size()));
    for (const uint16_t Index : Neighbors)
        Out.WriteU16(Index);

    Out.WriteU32(static_cast<uint32_t>(Data.Vertices.size()));
    for (const CVector3f& Vertex : Data.Vertices)
    {
        Out.WriteF32(Vertex.X); Out.WriteF32(Vertex.Y); Out.WriteF32(Vertex.Z);
    }
    return true;
}

bool CCollisionCooker::WriteAreaCollisionSection(const CCollisionMesh& Mesh, IOutputStream& Out, EGame Game,
                                                 const SOctreeSettings& Settings, std::string* pOutError)
{
    CVectorOutStream Block(std::endian::big);
    if (!WriteOctreeCollision(Mesh.GetIndexData(), Mesh.Bounds(), Block, Game, Settings, pOutError))
        return false;

    // PostConstructArea steps over the section's first byte and aligns to a word; the next word is
    // the block's size (CCollisionLoader::LoadAreaCollision skips both words).
    Out.WriteU32(0);
    Out.WriteU32(Block.Size());
    Out.WriteBytes(Block.Data(), Block.Size());
    return true;
}
