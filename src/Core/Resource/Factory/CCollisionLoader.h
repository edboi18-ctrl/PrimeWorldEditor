#ifndef CCOLLISIONLOADER_H
#define CCOLLISIONLOADER_H

#include <Common/EGame.h>
#include <Core/Resource/TResPtr.h>

#include <cstdint>
#include <memory>

class CCollisionMaterial;
class CCollisionMesh;
class CCollisionMeshGroup;
class CResourceEntry;
class IInputStream;
struct SCollisionIndexData;

class CCollisionLoader
{
    TResPtr<CCollisionMeshGroup> mpGroup;
    CCollisionMesh *mpMesh = nullptr;
    EGame mVersion{};

    CCollisionLoader();

    void LoadCollisionMaterial(IInputStream& Src, CCollisionMaterial& OutMaterial);
    void LoadCollisionIndices(IInputStream& File, SCollisionIndexData& OutData);

public:
    static std::unique_ptr<CCollisionMeshGroup> LoadAreaCollision(IInputStream& rMREA);
    static std::unique_ptr<CCollisionMeshGroup> LoadDCLN(IInputStream& rDCLN, CResourceEntry *pEntry);
    static EGame GetFormatVersion(uint32_t Version);
    /** Sets a material from a game's raw collision flags (RawFlags() keeps them as they were). */
    static void DecodeMaterial(uint64_t RawFlags, EGame Game, CCollisionMaterial& OutMaterial);
};

#endif // CCOLLISIONLOADER_H
