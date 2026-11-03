// Tests for CCollisionCooker (Echoes area collision).
//
// The cooked bytes are read back three ways:
//  - EngineReader below, a transcription of the game's reader in the Echoes port
//    (MetroidPrime2Port src/WorldFormat/CAreaOctTree.cpp MakeFromMemory / Node::GetChild /
//    GetTriangleArray, src/WorldFormat/CCollisionPrimitiveData.cpp GetTriangle, and the section
//    preamble in src/MetroidPrime/CGameArea.cpp PostConstructArea);
//  - PWE's own CCollisionLoader::LoadAreaCollision;
//  - the rules CCollisionTracker relies on (edge numbering and neighbours).

#include "TestLog.h"
#include <Core/Resource/Cooker/CCollisionCooker.h>
#include <Core/Resource/Factory/CCollisionLoader.h>
#include <Core/Resource/Collision/CCollisionMeshGroup.h>
#include <Common/FileIO/CMemoryInStream.h>
#include <Common/FileIO/CVectorOutStream.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace
{
int gFailures = 0;
const char* gDumpDir = nullptr;   // --dump <dir>: write each cooked section there, for other readers

#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++gFailures; } } while (0)

using STri = CCollisionCooker::STriangle;

const uint64_t kFloor = CCollisionCooker::kEchoesFloor | CCollisionCooker::kEchoesStone;
const uint64_t kWall = CCollisionCooker::kEchoesWall | CCollisionCooker::kEchoesStone;
const uint64_t kCeiling = CCollisionCooker::kEchoesCeiling | CCollisionCooker::kEchoesMetal;

/** Two triangles for the quad a-b-c-d (counter-clockwise from the front). */
void Quad(std::vector<STri>& Out, CVector3f a, CVector3f b, CVector3f c, CVector3f d, uint32_t Material)
{
    Out.push_back({{a, b, c}, Material});
    Out.push_back({{a, c, d}, Material});
}

/** A closed 10 x 10 x 4 room, every face looking inwards: floor, four walls, ceiling. */
std::vector<STri> BoxRoom()
{
    std::vector<STri> T;
    const float X = 10, Y = 10, Z = 4;
    Quad(T, {0, 0, 0}, {X, 0, 0}, {X, Y, 0}, {0, Y, 0}, 0);   // floor, up
    Quad(T, {0, 0, Z}, {0, Y, Z}, {X, Y, Z}, {X, 0, Z}, 2);   // ceiling, down
    Quad(T, {0, 0, 0}, {0, 0, Z}, {X, 0, Z}, {X, 0, 0}, 1);   // y = 0 wall, facing +y
    Quad(T, {X, Y, 0}, {X, Y, Z}, {0, Y, Z}, {0, Y, 0}, 1);   // y = Y wall, facing -y
    Quad(T, {0, Y, 0}, {0, Y, Z}, {0, 0, Z}, {0, 0, 0}, 1);   // x = 0 wall, facing +x
    Quad(T, {X, 0, 0}, {X, 0, Z}, {X, Y, Z}, {X, Y, 0}, 1);   // x = X wall, facing -x
    return T;
}

/** A bumpy N x N floor grid: enough triangles to make the octree branch. */
std::vector<STri> Terrain(int N)
{
    std::vector<STri> T;
    auto H = [](int i, int j) { return float((i * 7 + j * 3) % 5) * 0.25f; };
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            Quad(T, {float(i), float(j), H(i, j)}, {float(i + 1), float(j), H(i + 1, j)},
                 {float(i + 1), float(j + 1), H(i + 1, j + 1)}, {float(i), float(j + 1), H(i, j + 1)}, (i + j) % 2);
    return T;
}

/** Same triangle, same winding: equal up to a rotation of its corners. */
bool SameWinding(const std::array<CVector3f, 3>& A, const std::array<CVector3f, 3>& B)
{
    for (int r = 0; r < 3; r++)
        if (A[0] == B[r] && A[1] == B[(r + 1) % 3] && A[2] == B[(r + 2) % 3])
            return true;
    return false;
}

// ---------------------------------------------------------------------------- engine reader
struct Be
{
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;
    uint32_t U32() { if (pos + 4 > n) { ok = false; return 0; } uint32_t v = uint32_t(p[pos]) << 24 | uint32_t(p[pos + 1]) << 16 | uint32_t(p[pos + 2]) << 8 | p[pos + 3]; pos += 4; return v; }
    uint16_t U16() { if (pos + 2 > n) { ok = false; return 0; } uint16_t v = uint16_t(p[pos] << 8 | p[pos + 1]); pos += 2; return v; }
    uint8_t U8() { if (pos + 1 > n) { ok = false; return 0; } return p[pos++]; }
    uint64_t U64() { uint64_t hi = U32(); return hi << 32 | U32(); }
    float F32() { uint32_t v = U32(); float f; std::memcpy(&f, &v, 4); return f; }
    CVector3f Vec() { float x = F32(), y = F32(); return {x, y, F32()}; }
};

struct SEngineCollision
{
    CVector3f BoundsMin, BoundsMax;
    std::vector<uint64_t> Materials;
    std::vector<uint8_t> VertexMaterials, EdgeMaterials, SurfaceMaterials;
    std::vector<std::pair<uint16_t, uint16_t>> Edges;
    std::vector<uint16_t> SurfaceIndices, Neighbors;
    std::vector<CVector3f> Vertices;
    std::vector<std::vector<uint16_t>> Leaves;   // triangle lists, in traversal order
    std::vector<std::pair<CVector3f, CVector3f>> LeafBoxes;
    bool Ok = true;

    /** CCollisionPrimitiveData::GetTriangle: corners from the first two edges, by the flipped bit. */
    std::array<CVector3f, 3> Triangle(size_t t) const
    {
        const auto& e0 = Edges[SurfaceIndices[t * 3]];
        const auto& e1 = Edges[SurfaceIndices[t * 3 + 1]];
        if (Materials[SurfaceMaterials[t]] & 0x01000000)  // kFlippedTriangle
            return {Vertices[e0.second], Vertices[e0.first], Vertices[e1.first]};
        return {Vertices[e0.first], Vertices[e0.second], Vertices[e1.second]};
    }
};

/** CGameArea::PostConstructArea's collision preamble, then CAreaOctTree::MakeFromMemory. */
SEngineCollision EngineRead(const std::vector<char>& Section)
{
    SEngineCollision C;
    const auto* bytes = reinterpret_cast<const uint8_t*>(Section.data());
    // "++collisionData; while (addr & 3) ++collisionData;": the section is 32-aligned, so word 1.
    Be pre{bytes, Section.size(), 4};
    const uint32_t BlockSize = pre.U32();
    if (!pre.ok || 8 + BlockSize > Section.size()) { C.Ok = false; return C; }
    Be in{bytes + 8, BlockSize};

    in.U32(); in.U32();                      // in.ReadInt32() twice
    C.BoundsMin = in.Vec(); C.BoundsMax = in.Vec();
    const uint32_t RootType = in.U32();
    const uint32_t TreeSize = in.U32();
    const size_t TreeStart = in.pos;
    if (TreeStart + TreeSize > BlockSize) { C.Ok = false; return C; }

    // Node::GetChild: branch = u16 types (2 bits per child), pad, 8 u32 offsets from node + 36;
    // a leaf child carries its own box; a branch child's box is the parent's octant (BoxFromIndex).
    std::function<void(size_t, uint32_t, CVector3f, CVector3f)> Walk = [&](size_t At, uint32_t Type, CVector3f Mn, CVector3f Mx)
    {
        if (At >= TreeStart + TreeSize) { C.Ok = false; return; }
        Be node{bytes + 8, BlockSize, At};
        if (Type == 2)
        {
            const CVector3f LMin = node.Vec(), LMax = node.Vec();
            const uint16_t Count = node.U16();
            std::vector<uint16_t> Tris(Count);
            for (auto& t : Tris) t = node.U16();
            if (!node.ok || node.pos > TreeStart + TreeSize) C.Ok = false;
            C.Leaves.push_back(Tris);
            C.LeafBoxes.push_back({LMin, LMax});
        }
        else if (Type == 1)
        {
            const uint16_t Types = node.U16();
            node.U16();
            uint32_t Offsets[8];
            for (auto& o : Offsets) o = node.U32();
            const CVector3f Ct = (Mn + Mx) * 0.5f;
            for (uint32_t i = 0; i < 8; i++)
            {
                const uint32_t ChildType = (Types >> (2 * i)) & 3;
                if (ChildType == 0) continue;
                if (ChildType == 3) { C.Ok = false; continue; }
                CVector3f a = Mn, b = Ct;
                if (i & 1) { a.X = Ct.X; b.X = Mx.X; }
                if (i & 2) { a.Y = Ct.Y; b.Y = Mx.Y; }
                if (i & 4) { a.Z = Ct.Z; b.Z = Mx.Z; }
                Walk(At + 36 + Offsets[i], ChildType, a, b);
            }
        }
        else
        {
            C.Ok = false;
        }
    };
    Walk(TreeStart, RootType, C.BoundsMin, C.BoundsMax);

    // The arrays, packed right after the tree.
    in.pos = TreeStart + TreeSize;
    C.Materials.resize(in.U32());
    for (auto& m : C.Materials) m = in.U64();
    for (auto* arr : {&C.VertexMaterials, &C.EdgeMaterials, &C.SurfaceMaterials})
    {
        arr->resize(in.U32());
        for (auto& v : *arr) v = in.U8();
    }
    C.Edges.resize(in.U32());
    for (auto& e : C.Edges) { e.first = in.U16(); e.second = in.U16(); }
    const uint32_t IndexCount = in.U32() / 3 * 3;      // triangleCount = *surfaceHeader / 3
    C.SurfaceIndices.resize(IndexCount);
    for (auto& s : C.SurfaceIndices) s = in.U16();
    in.U32();                                          // extraIndexHeader: skipped, triangleCount * 3 follow
    C.Neighbors.resize(IndexCount);
    for (auto& s : C.Neighbors) s = in.U16();
    C.Vertices.resize(in.U32());
    for (auto& v : C.Vertices) v = in.Vec();
    C.Ok = C.Ok && in.ok && in.pos == BlockSize;       // nothing missing, nothing left over
    return C;
}

bool BoxContains(const std::pair<CVector3f, CVector3f>& Box, const CVector3f& P)
{
    return P.X >= Box.first.X && P.Y >= Box.first.Y && P.Z >= Box.first.Z &&
           P.X <= Box.second.X && P.Y <= Box.second.Y && P.Z <= Box.second.Z;
}

// ---------------------------------------------------------------------------- one mesh, every check
void CheckMesh(const char* Name, const std::vector<STri>& Tris, const std::vector<uint64_t>& Materials,
               bool Closed, size_t MinLeaves)
{
    std::fprintf(stderr, "== %s: %zu triangles\n", Name, Tris.size());
    SCollisionIndexData Data;
    std::string Error;
    CHECK(CCollisionCooker::BuildIndexData(Tris, Materials, EGame::Echoes, Data, &Error));
    if (!Error.empty()) std::fprintf(stderr, "   %s\n", Error.c_str());
    const size_t N = Tris.size();
    CHECK(Data.TriangleIndices.size() == N * 3);
    CHECK(Data.TriangleMaterialIndices.size() == N);
    CHECK(Data.TriangleNeighbors.size() == N * 3);

    auto pMesh = CCollisionCooker::BuildMesh(SCollisionIndexData(Data));
    CVectorOutStream Out(std::endian::big);
    CHECK(CCollisionCooker::WriteAreaCollisionSection(*pMesh, Out, EGame::Echoes, {}, &Error));
    std::vector<char> Section(static_cast<const char*>(Out.Data()), static_cast<const char*>(Out.Data()) + Out.Size());
    if (gDumpDir)
    {
        std::string Path = std::string(gDumpDir) + "/" + Name + ".collision";
        std::replace(Path.begin() + std::strlen(gDumpDir), Path.end(), ' ', '_');
        if (FILE* f = std::fopen(Path.c_str(), "wb"))
        {
            std::fwrite(Section.data(), 1, Section.size(), f);
            std::fclose(f);
        }
    }

    // 1. The game's reader.
    const SEngineCollision C = EngineRead(Section);
    CHECK(C.Ok);
    CHECK(C.SurfaceIndices.size() == N * 3);
    std::set<uint16_t> Reached;
    for (size_t l = 0; l < C.Leaves.size(); l++)
    {
        for (const uint16_t t : C.Leaves[l])
        {
            CHECK(t < N);
            if (t >= N) continue;
            Reached.insert(t);
            for (const CVector3f& v : C.Triangle(t))
                CHECK(BoxContains(C.LeafBoxes[l], v));
        }
    }
    CHECK(Reached.size() == N);   // every triangle is in some leaf
    CHECK(C.Leaves.size() >= MinLeaves);
    std::fprintf(stderr, "   %zu leaves, %zu bytes\n", C.Leaves.size(), Section.size());

    for (size_t t = 0; t < N && C.Ok; t++)
    {
        const auto Corners = C.Triangle(t);
        CHECK(SameWinding(Corners, Tris[t].Vertices));
        // Its material is the input's (maybe the flipped variant of it).
        CHECK((C.Materials[C.SurfaceMaterials[t]] & ~CCollisionCooker::kEchoesFlippedTriangle) == Materials[Tris[t].MaterialIndex]);

        // FindExitEdge numbers edges v0-v1, v1-v2, v2-v0 and FindSharedEdge looks them up in this
        // triple, so slot i has to join corner i and corner i + 1.
        for (size_t i = 0; i < 3; i++)
        {
            const auto& e = C.Edges[C.SurfaceIndices[t * 3 + i]];
            const CVector3f a = C.Vertices[e.first], b = C.Vertices[e.second];
            const CVector3f p = Corners[i], q = Corners[(i + 1) % 3];
            CHECK((a == p && b == q) || (a == q && b == p));

            const uint16_t nb = C.Neighbors[t * 3 + i];
            if (Closed) CHECK(nb != 0xFFFF);
            if (nb != 0xFFFF)
            {
                const uint16_t Shared = C.SurfaceIndices[t * 3 + i];
                CHECK(nb != t);
                CHECK(C.SurfaceIndices[nb * 3] == Shared || C.SurfaceIndices[nb * 3 + 1] == Shared || C.SurfaceIndices[nb * 3 + 2] == Shared);
            }
        }
    }
    // Vertex and edge materials never carry the flipped bit (it means no-edge-collision there).
    for (const uint8_t m : C.VertexMaterials) CHECK(!(C.Materials[m] & CCollisionCooker::kEchoesFlippedTriangle));
    for (const uint8_t m : C.EdgeMaterials) CHECK(!(C.Materials[m] & CCollisionCooker::kEchoesFlippedTriangle));

    // 2. PWE's loader gets the same index data back.
    CMemoryInStream In(Section.data(), static_cast<uint32_t>(Section.size()), std::endian::big);
    auto pGroup = CCollisionLoader::LoadAreaCollision(In);
    CHECK(pGroup != nullptr);
    if (pGroup)
    {
        const SCollisionIndexData& Back = (*pGroup->Meshes().begin())->GetIndexData();
        CHECK(Back.EdgeIndices == Data.EdgeIndices);
        CHECK(Back.TriangleIndices == Data.TriangleIndices);
        CHECK(Back.TriangleNeighbors == Data.TriangleNeighbors);
        CHECK(Back.VertexMaterialIndices == Data.VertexMaterialIndices);
        CHECK(Back.EdgeMaterialIndices == Data.EdgeMaterialIndices);
        CHECK(Back.TriangleMaterialIndices == Data.TriangleMaterialIndices);
        CHECK(Back.Vertices == Data.Vertices);
        CHECK(Back.Materials.size() == Data.Materials.size());
        for (size_t m = 0; m < std::min(Back.Materials.size(), Data.Materials.size()); m++)
        {
            CHECK(Back.Materials[m].RawFlags() == Data.Materials[m].RawFlags());
            CHECK(Back.Materials[m].IsFloor() == Data.Materials[m].IsFloor());
        }
    }

    // 3. Cooking what was loaded gives the same bytes.
    if (pGroup)
    {
        CVectorOutStream Again(std::endian::big);
        CHECK(CCollisionCooker::WriteAreaCollisionSection(**pGroup->Meshes().begin(), Again, EGame::Echoes));
        CHECK(Again.Size() == Out.Size() && std::memcmp(Again.Data(), Out.Data(), Out.Size()) == 0);
    }
}

void CheckLimits()
{
    SCollisionIndexData Data;
    std::string Error;
    CHECK(!CCollisionCooker::BuildIndexData({{{CVector3f(0, 0, 0), CVector3f(1, 0, 0), CVector3f(0, 1, 0)}, 3}}, {kFloor}, EGame::Echoes, Data, &Error));
    CHECK(Error.find("material index") != std::string::npos);
    CHECK(!CCollisionCooker::BuildIndexData({{{CVector3f(0, 0, 0), CVector3f(0, 0, 0), CVector3f(0, 1, 0)}, 0}}, {kFloor}, EGame::Echoes, Data, &Error));
    CHECK(Error.find("identical") != std::string::npos);
    CHECK(!CCollisionCooker::BuildIndexData({}, {kFloor}, EGame::Prime, Data, &Error));
    CHECK(!CCollisionCooker::BuildIndexData({}, std::vector<uint64_t>(300, kFloor), EGame::Echoes, Data, &Error));
}
}

int main(int argc, char** argv)
{
    InitTestLog();
    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0)
        gDumpDir = argv[2];
    CheckMesh("box room", BoxRoom(), {kFloor, kWall, kCeiling}, true, 1);
    CheckMesh("terrain", Terrain(40), {kFloor, kFloor | CCollisionCooker::kEchoesMetal}, false, 64);
    CheckLimits();
    if (gFailures)
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    else
        std::fprintf(stderr, "all collision cooker checks passed\n");
    return gFailures ? 1 : 0;
}
