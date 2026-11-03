#ifndef CCOLLISIONCOOKER_H
#define CCOLLISIONCOOKER_H

#include "Core/Resource/Collision/CCollisionMesh.h"
#include <Common/EGame.h>
#include <Common/Math/CVector3f.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IOutputStream;

/** Octree build limits for CCollisionCooker. A leaf holds at most 65535 triangles whatever these say. */
struct SCollisionOctreeSettings
{
    uint32_t MaxLeafTriangles = 128;
    uint32_t MaxDepth = 6;
};

/**
 * Writes area (MREA) collision for Metroid Prime 2: Echoes.
 *
 * The reader this has to satisfy is the game's own, as decompiled in the Echoes port
 * (MetroidPrime2Port): CGameArea::PostConstructArea (src/MetroidPrime/CGameArea.cpp) finds the
 * section, CAreaOctTree::MakeFromMemory (src/WorldFormat/CAreaOctTree.cpp) reads the octree and
 * the arrays after it, CCollisionPrimitiveData::GetTriangle (src/WorldFormat/
 * CCollisionPrimitiveData.cpp) rebuilds each triangle from two of its edges, and
 * CCollisionTracker (src/MetroidPrime/Enemies/CCollisionTracker.cpp) walks triangle neighbours.
 */
class CCollisionCooker
{
public:
    /** A triangle in counter-clockwise order as seen from its front, and its material. */
    struct STriangle
    {
        std::array<CVector3f, 3> Vertices;
        uint32_t MaterialIndex = 0;
    };

    using SOctreeSettings = SCollisionOctreeSettings;

    /** Raw Echoes flags (CMaterialList's EMaterialTypes bits) for common surfaces. */
    static constexpr uint64_t kEchoesFlippedTriangle = 0x01000000;   // kFlippedTriangle
    static constexpr uint64_t kEchoesStone           = 1ull << 1;    // kMT_Stone
    static constexpr uint64_t kEchoesMetal           = 1ull << 2;    // kMT_Metal
    static constexpr uint64_t kEchoesCeiling         = 1ull << 29;   // kMT_Ceiling
    static constexpr uint64_t kEchoesWall            = 1ull << 30;   // kMT_Wall
    static constexpr uint64_t kEchoesFloor           = 1ull << 31;   // kMT_Floor

    /**
     * Builds collision index data from triangles: shared vertices and edges, each triangle's
     * edge triple and neighbours, and flipped material variants where the shared edges need one.
     * MaterialFlags are raw game flags, one per material (at most 255 with their flipped variants).
     * Returns false (with a reason) if the mesh is over a format limit.
     */
    static bool BuildIndexData(const std::vector<STriangle>& Triangles, const std::vector<uint64_t>& MaterialFlags,
                               EGame Game, SCollisionIndexData& OutData, std::string* pOutError = nullptr);

    /** A mesh ready for WriteAreaCollisionSection or for display, from BuildIndexData's output. */
    static std::unique_ptr<CCollisionMesh> BuildMesh(SCollisionIndexData&& Data);

    /** Each triangle's neighbour across each of its edges (TriangleIndices order), 0xFFFF for none. */
    static std::vector<uint16_t> BuildTriangleNeighbors(const SCollisionIndexData& Data);

    /** The octree collision block CAreaOctTree::MakeFromMemory reads (magic 0xDEAFBABE onwards). */
    static bool WriteOctreeCollision(const SCollisionIndexData& Data, const CAABox& Bounds, IOutputStream& Out, EGame Game,
                                     const SOctreeSettings& Settings = {}, std::string* pOutError = nullptr);

    /** A whole MREA collision section: the word PostConstructArea skips, the block's size, the block. */
    static bool WriteAreaCollisionSection(const CCollisionMesh& Mesh, IOutputStream& Out, EGame Game,
                                          const SOctreeSettings& Settings = {}, std::string* pOutError = nullptr);
};

#endif // CCOLLISIONCOOKER_H
