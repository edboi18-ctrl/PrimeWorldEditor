#ifndef CWORLDGEOMETRYCOOKER_H
#define CWORLDGEOMETRYCOOKER_H

#include <Common/Math/CAABox.h>
#include <Common/Math/CVector2f.h>
#include <Common/Math/CVector3f.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

/** Build limits for the render octree (AROT) of CWorldGeometryCooker. */
struct SRenderOctreeSettings
{
    uint32_t MaxLeafSurfaces = 16;
    uint32_t MaxDepth = 8;
};

/**
 * Writes the world geometry sections of a Metroid Prime 2: Echoes room (MREA version 0x19): the
 * world material set, every world model, the render octree, the area surface list and the ambient
 * light table, in the order the game reads them.
 *
 * The reader this has to satisfy is the game's own, as decompiled in the Echoes port
 * (MetroidPrime2Port): CGameArea::PostConstructArea and FillInStaticGeometry
 * (src/MetroidPrime/CGameArea.cpp) walk the sections, CMetroidModelInstance
 * (src/WorldFormat/CMetroidModelInstance.*) and CCubeSurface (src/Kyoto/Graphics/CCubeSurface.*)
 * read the models and surfaces, CCubeModel::SetArraysCurrent (src/Kyoto/Graphics/CCubeModel.cpp)
 * and CGraphics::SetDefaultVtxAttrFmt (DolphinCGraphics.cpp) fix the vertex formats, CAreaRenderOctTree
 * (src/WorldFormat/CAreaRenderOctTree.cpp) reads the octree, and CCubeRenderer
 * (src/MetaRender/CCubeRenderer.cpp) uses the surface records, groups and area surfaces.
 */
class CWorldGeometryCooker
{
public:
    /** What the writer needs from each material of the cooked set. */
    struct SMaterialInfo
    {
        uint32_t Flags = 0;              // CCubeMaterial flags (word 0)
        uint32_t VertexDescription = 0;  // compressed GX vertex description, 2 bits per attribute
        bool PackedLightmapUVs() const { return (Flags & kLightmapUvArray) != 0; }
    };

    /** One corner of a triangle: an index into each array its material reads. */
    struct SCorner
    {
        uint16_t Position = 0;
        uint16_t Normal = 0;
        uint16_t Color = 0;
        /** TEX0..TEX6. For a material with packed lightmap UVs, TEX0 indexes LightmapTexCoords;
         *  every other slot indexes TexCoords (the game points TEX1-7 at one shared array). */
        std::array<uint16_t, 7> Tex{};
    };

    struct SSurface
    {
        uint32_t MaterialIndex = 0;
        std::vector<SCorner> Corners;  // three per triangle, counter-clockwise from the front
    };

    struct SModel
    {
        std::array<float, 12> Transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // 3x4, row-major
        uint32_t ExtraFlags = 0;  // flag bits other than bit 0 (short normals), which the writer sets
        std::vector<CVector3f> Positions;
        std::vector<CVector3f> Normals;
        std::vector<uint32_t> Colors;            // RGBA8, 0xRRGGBBAA
        std::vector<CVector2f> TexCoords;        // float UVs (GX_F32)
        std::vector<CVector2f> LightmapTexCoords; // packed to u16 / 32768 (GX_U16, 15 fraction bits)
        std::vector<SSurface> Surfaces;
    };

    struct SOutput
    {
        std::vector<std::vector<uint8_t>> Sections;  // materials through the ambient light table
        uint32_t ModelCount = 0;
        uint32_t AreaSurfaceCount = 0;
        CAABox Bounds;
    };

    static constexpr uint32_t kLightmapUvArray = 1u << 13;  // kStateFlag_LightmapUvArray

    /** Each material's flags and vertex description, from a cooked Echoes material set. */
    static bool ParseMaterialSet(const std::vector<uint8_t>& MaterialSet, std::vector<SMaterialInfo>& OutInfo,
                                 std::string* pOutError = nullptr);

    /**
     * The geometry sections for these models over a cooked material set (for example a room's own,
     * its first section). Each model surface becomes one area surface with a group of its own.
     */
    static bool Write(const std::vector<uint8_t>& MaterialSet, const std::vector<SModel>& Models, SOutput& Out,
                      std::string* pOutError = nullptr, const SRenderOctreeSettings& Settings = {});
};

#endif // CWORLDGEOMETRYCOOKER_H
