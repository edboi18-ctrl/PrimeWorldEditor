// Tests for CAreaCooker's replaced geometry, collision and pathfinding (Echoes).
//
// A whole MREA is cooked and split back into sections the way the game's loader and the port's
// tools/echoes_disc.py do (header, section sizes, block table, stored blocks), then walked in
// CGameArea::PostConstructArea's order to check the game finds each section where it should.

#include "TestLog.h"
#include <Core/Resource/Area/CGameArea.h>
#include <Core/Resource/Cooker/CAreaCooker.h>
#include <Core/Resource/Cooker/CCollisionCooker.h>
#include <Core/Resource/Cooker/CWorldGeometryCooker.h>
#include <Core/Resource/Factory/CCollisionLoader.h>
#include <Common/FileIO/CMemoryInStream.h>
#include <Common/FileIO/CVectorOutStream.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/** CGameArea befriends this to set the "original file" an area was loaded from. */
struct SGameAreaTestAccess
{
    static void SetOriginal(CGameArea& Area, std::vector<std::vector<uint8_t>> Sections, uint32_t MeshCount)
    {
        Area.mSectionDataBuffers = std::move(Sections);
        Area.mOriginalWorldMeshCount = MeshCount;
        Area.mUsesCompression = false;
    }
};

namespace
{
int gFailures = 0;
#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++gFailures; } } while (0)

using Bytes = std::vector<uint8_t>;

uint32_t U32(const Bytes& B, size_t At)
{
    return uint32_t(B[At]) << 24 | uint32_t(B[At + 1]) << 16 | uint32_t(B[At + 2]) << 8 | B[At + 3];
}
size_t Align32(size_t N) { return (N + 31) & ~size_t(31); }

struct SMrea
{
    std::vector<uint32_t> Header;   // 29 words
    std::vector<Bytes> Sections;
    bool Ok = true;
};

/** The header, section sizes and block table, then each stored block cut into its sections. */
SMrea Split(const Bytes& File)
{
    SMrea M;
    if (File.size() < 0x80) { M.Ok = false; return M; }
    for (int i = 0; i < 29; i++) M.Header.push_back(U32(File, 4 * i));
    const uint32_t Count = M.Header[16], Blocks = M.Header[28];
    size_t Pos = 0x80;
    std::vector<uint32_t> Sizes;
    for (uint32_t i = 0; i < Count; i++) Sizes.push_back(U32(File, Pos + 4 * i));
    Pos += Align32(Count * 4);
    std::vector<std::array<uint32_t, 4>> Table;
    for (uint32_t i = 0; i < Blocks; i++)
        Table.push_back({U32(File, Pos + 16 * i), U32(File, Pos + 16 * i + 4), U32(File, Pos + 16 * i + 8), U32(File, Pos + 16 * i + 12)});
    Pos += Align32(Blocks * 16);
    size_t Section = 0;
    for (const auto& [Buf, Decompressed, Compressed, SecCount] : Table)
    {
        if (Compressed != 0 || Pos + Decompressed > File.size()) { M.Ok = false; return M; }
        size_t At = Pos;
        for (uint32_t s = 0; s < SecCount; s++, Section++)
        {
            M.Sections.emplace_back(File.begin() + At, File.begin() + At + Sizes[Section]);
            At += Sizes[Section];
        }
        Pos += Align32(Buf);
    }
    M.Ok = M.Sections.size() == Count;
    return M;
}

Bytes Filled(uint8_t V) { return Bytes(32, V); }
Bytes AssetIDSection(uint32_t ID) { Bytes B(32, 0); for (int i = 0; i < 4; i++) B[i] = uint8_t(ID >> (24 - 8 * i)); return B; }

/** An area with no world models and no layers, as DetermineSectionNumbersPrime numbers it for
 *  Echoes: geometry 0-2, FFFF 3, SCLY/SCGN 4, collision 5, BSP 6, lights 7, VISI 8, PATH 9,
 *  PTLA 10, EGMC 11. Section i holds the byte 0xA0 + i. */
std::vector<Bytes> OriginalSections()
{
    std::vector<Bytes> S;
    for (uint8_t i = 0; i < 12; i++) S.push_back(Filled(uint8_t(0xA0 + i)));
    return S;
}

std::unique_ptr<CCollisionMesh> FloorCollision()
{
    using T = CCollisionCooker::STriangle;
    const std::vector<T> Tris = {{{CVector3f(0, 0, 0), CVector3f(4, 0, 0), CVector3f(4, 4, 0)}, 0},
                                 {{CVector3f(0, 0, 0), CVector3f(4, 4, 0), CVector3f(0, 4, 0)}, 0}};
    SCollisionIndexData Data;
    CHECK(CCollisionCooker::BuildIndexData(Tris, {CCollisionCooker::kEchoesFloor | CCollisionCooker::kEchoesStone}, EGame::Echoes, Data));
    return CCollisionCooker::BuildMesh(std::move(Data));
}

CWorldGeometryCooker::SOutput FloorGeometry()
{
    // One material (POS and NRM, 16-bit indices), one model, one surface: two triangles.
    Bytes Mat;
    auto W = [&](uint32_t v) { for (int i = 3; i >= 0; i--) Mat.push_back(uint8_t(v >> (8 * i))); };
    for (uint32_t w : {0u, 1u, 52u}) W(w);   // no textures, one material ending at byte 52
    for (uint32_t w : {0x4082u, 0u, 0xFu, 0u, 0u, 0u, 0x00010000u, 1u, 0x3001u, 0u, 0u, 4u, 0u}) W(w);
    CWorldGeometryCooker::SModel M;
    M.Positions = {CVector3f(0, 0, 0), CVector3f(4, 0, 0), CVector3f(4, 4, 0), CVector3f(0, 4, 0)};
    M.Normals = {CVector3f(0, 0, 1)};
    CWorldGeometryCooker::SSurface S;
    for (uint16_t i : {0, 1, 2, 0, 2, 3}) { CWorldGeometryCooker::SCorner c; c.Position = i; S.Corners.push_back(c); }
    M.Surfaces.push_back(S);
    CWorldGeometryCooker::SOutput Out;
    std::string Error;
    CHECK(CWorldGeometryCooker::Write(Mat, {M}, Out, &Error));
    return Out;
}

Bytes Cook(CGameArea& Area, EGame Game, bool* pOk = nullptr)
{
    CVectorOutStream Out(std::endian::big);
    const bool Ok = CAreaCooker::CookMREA(&Area, Game, Out);
    if (pOk) *pOk = Ok;
    const auto* p = static_cast<const uint8_t*>(Out.Data());
    return Bytes(p, p + Out.Size());
}

void CheckGeometryCollisionAndPath(const char* DumpDir)
{
    std::fprintf(stderr, "== geometry, collision and PATH replaced\n");
    CGameArea Area;
    SGameAreaTestAccess::SetOriginal(Area, OriginalSections(), 0);
    auto Geometry = FloorGeometry();
    const auto GeometrySections = Geometry.Sections;
    Area.ReplaceWorldGeometry(std::move(Geometry.Sections), Geometry.ModelCount);
    Area.ReplaceCollision(FloorCollision());
    Area.SetClearPathOnCook(true);

    bool Ok = false;
    const SMrea M = Split(Cook(Area, EGame::Echoes, &Ok));
    CHECK(Ok && M.Ok);
    if (!M.Ok) return;
    const uint32_t G = uint32_t(GeometrySections.size());   // 14: materials, 1 + 9 model sections + 1 surface, AROT, area surfaces, ambient
    CHECK(M.Header[0] == 0xDEADBEEF && M.Header[1] == 0x19);
    CHECK(M.Header[14] == 1);                  // world model count
    CHECK(M.Header[15] == 0);                  // layers
    CHECK(M.Header[16] == G + 8);              // + SCGN, collision, BSP, lights, VISI, PATH, PTLA, EGMC
    CHECK(M.Header[17] == 0);                  // geometry
    CHECK(M.Header[18] == G && M.Header[19] == G);   // SCLY, SCGN
    CHECK(M.Header[20] == G + 1);              // collision
    CHECK(M.Header[21] == G + 2 && M.Header[22] == G + 3 && M.Header[23] == G + 4 && M.Header[24] == G + 5);
    CHECK(M.Header[25] == G - 1);              // read by the game as the render octree's section: not -1
    CHECK(M.Header[26] == G + 6 && M.Header[27] == G + 7);

    for (uint32_t i = 0; i < G; i++) CHECK(M.Sections[i] == GeometrySections[i]);

    // PostConstructArea's walk: materials, each model (7 + surfaces + 2), the render octree, area
    // surfaces, ambient lights, the layers, SCGN, then collision.
    size_t s = 1;
    for (uint32_t model = 0; model < M.Header[14]; model++)
        s += 7 + U32(M.Sections[s + 6], 0) + 2;
    s += 3 + M.Header[15];
    CHECK(s == G);
    CHECK(std::memcmp(M.Sections[s].data(), "SCGN", 4) == 0);
    const Bytes& Collision = M.Sections[s + 1];
    CHECK(U32(Collision, 8) == 0xDEAFBABE);
    CMemoryInStream In(Collision.data(), uint32_t(Collision.size()), std::endian::big);
    auto pGroup = CCollisionLoader::LoadAreaCollision(In);
    CHECK(pGroup != nullptr && (*pGroup->Meshes().begin())->GetIndexData().TriangleIndices.size() == 6);

    CHECK(M.Sections[s + 2] == Filled(0xA6));          // BSP: kept
    CHECK(M.Sections[s + 3] == Filled(0xA7));          // lights: kept
    CHECK(M.Sections[s + 4] == Bytes(32, 0));          // VISI: cleared (<= 64 bytes is ignored)
    CHECK(M.Sections[s + 5] == AssetIDSection(0xFFFFFFFF));   // PATH: cleared
    CHECK(M.Sections[s + 6] == Filled(0xAA));          // PTLA: kept
    CHECK(M.Sections[s + 7] == AssetIDSection(0xFFFFFFFF));   // EGMC: cleared

    if (FILE* f = DumpDir ? std::fopen((std::string(DumpDir) + "/replaced_room.MREA").c_str(), "wb") : nullptr)
    {
        // For the port's independent reader (tools/mcp/workshop.py).
        CGameArea Again;
        SGameAreaTestAccess::SetOriginal(Again, OriginalSections(), 0);
        auto Geo = FloorGeometry();
        Again.ReplaceWorldGeometry(std::move(Geo.Sections), Geo.ModelCount);
        Again.ReplaceCollision(FloorCollision());
        const Bytes File = Cook(Again, EGame::Echoes);
        std::fwrite(File.data(), 1, File.size(), f);
        std::fclose(f);
    }
}

void CheckCollisionOnly()
{
    std::fprintf(stderr, "== collision replaced\n");
    CGameArea Area;
    SGameAreaTestAccess::SetOriginal(Area, OriginalSections(), 0);
    Area.ReplaceCollision(FloorCollision());
    const SMrea M = Split(Cook(Area, EGame::Echoes));
    CHECK(M.Ok);
    if (!M.Ok) return;
    CHECK(M.Header[16] == 12 && M.Header[20] == 5 && M.Header[25] == 3);
    for (uint32_t i = 0; i < 4; i++) CHECK(M.Sections[i] == Filled(uint8_t(0xA0 + i)));
    CHECK(U32(M.Sections[5], 8) == 0xDEAFBABE);
    for (uint32_t i = 6; i < 12; i++) CHECK(M.Sections[i] == Filled(uint8_t(0xA0 + i)));   // VISI, PATH, EGMC kept
}

void CheckNothingReplaced()
{
    std::fprintf(stderr, "== nothing replaced\n");
    CGameArea Area;
    SGameAreaTestAccess::SetOriginal(Area, OriginalSections(), 0);
    const SMrea M = Split(Cook(Area, EGame::Echoes));
    CHECK(M.Ok);
    if (!M.Ok) return;
    CHECK(M.Header[14] == 0 && M.Header[16] == 12);
    for (uint32_t i = 0; i < 12; i++)
        if (i != 4) CHECK(M.Sections[i] == Filled(uint8_t(0xA0 + i)));   // 4 is SCGN, written fresh
}

void CheckOtherGamesRefused()
{
    CGameArea Area;
    SGameAreaTestAccess::SetOriginal(Area, OriginalSections(), 0);
    Area.ReplaceCollision(FloorCollision());
    bool Ok = true;
    Cook(Area, EGame::Prime, &Ok);
    CHECK(!Ok);
}
}

int main(int argc, char** argv)
{
    InitTestLog();
    // --dump <dir>: also write a cooked room there (geometry and collision replaced).
    CheckGeometryCollisionAndPath(argc == 3 && std::strcmp(argv[1], "--dump") == 0 ? argv[2] : nullptr);
    CheckCollisionOnly();
    CheckNothingReplaced();
    CheckOtherGamesRefused();
    if (gFailures)
        std::fprintf(stderr, "%d check(s) failed\n", gFailures);
    else
        std::fprintf(stderr, "all area cooker checks passed\n");
    return gFailures ? 1 : 0;
}
