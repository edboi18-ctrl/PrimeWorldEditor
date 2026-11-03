#ifndef CGAMEAREA_H
#define CGAMEAREA_H

#include "Core/Resource/CResource.h"
#include "Core/Resource/CLight.h"
#include "Core/Resource/CMaterialSet.h"
#include "Core/Resource/CPoiToWorld.h"
#include "Core/Resource/Collision/CCollisionMeshGroup.h"
#include "Core/Resource/Model/CModel.h"
#include "Core/Resource/Model/CStaticModel.h"
#include "Core/Resource/Script/CInstanceID.h"
#include <Common/Math/CQuaternion.h>
#include <Common/Math/CTransform4f.h>

#include <memory>
#include <ranges>
#include <unordered_map>
#include <vector>

class CScriptLayer;
class CScriptObject;
class CScriptTemplate;

class CGameArea : public CResource
{
    DECLARE_RESOURCE_TYPE(Area)
    friend class CAreaLoader;
    friend class CAreaCooker;
    friend struct SGameAreaTestAccess;  // tests/: sets the original sections

    uint32_t mWorldIndex = UINT32_MAX;
    uint32_t mVertexCount = 0;
    uint32_t mTriangleCount = 0;
    bool mTerrainMerged = false;
    CTransform4f mTransform;
    CAABox mAABox;

    // Data saved from the original file to help on recook
    std::vector<std::vector<uint8_t>> mSectionDataBuffers;
    uint32_t mOriginalWorldMeshCount = 0;
    bool mUsesCompression = false;

    struct SSectionNumber
    {
        CFourCC SectionID;
        uint32_t Index{};
    };
    std::vector<SSectionNumber> mSectionNumbers;

    // Geometry
    std::unique_ptr<CMaterialSet> mpMaterialSet;
    std::vector<std::unique_ptr<CModel>> mWorldModels; // TerrainModels is the original version of each model; this is currently mainly used in the POI map editor
    std::vector<std::unique_ptr<CStaticModel>> mStaticWorldModels; // StaticTerrainModels is the merged terrain for faster rendering in the world editor
    // Script
    std::vector<std::unique_ptr<CScriptLayer>> mScriptLayers;
    std::unordered_map<CInstanceID, CScriptObject*> mObjectMap;
    // Collision
    std::unique_ptr<CCollisionMeshGroup> mpCollision;
    // Lights
    std::vector<std::vector<CLight>> mLightLayers;
    // Path Mesh
    CAssetID mPathID;
    // Portal Area
    CAssetID mPortalAreaID;
    // Object to Static Geometry Map
    TResPtr<CPoiToWorld> mpPoiToWorldMap;
    // Dependencies
    std::vector<CAssetID> mExtraAreaDeps;
    std::vector< std::vector<CAssetID> > mExtraLayerDeps;

    // Written by CAreaCooker in place of the original sections (Echoes only): world geometry from
    // CWorldGeometryCooker (materials through the ambient light table) and collision for
    // CCollisionCooker. Replacing the geometry also clears the room's visibility (PVS) and static
    // geometry map (EGMC), which index the old surfaces; mClearPathOnCook clears PATH too.
    std::vector<std::vector<uint8_t>> mReplacementGeometrySections;
    uint32_t mReplacementWorldModelCount = 0;
    std::unique_ptr<CCollisionMesh> mpReplacementCollision;
    bool mClearPathOnCook = false;

public:
    void ReplaceWorldGeometry(std::vector<std::vector<uint8_t>> Sections, uint32_t ModelCount)
    {
        mReplacementGeometrySections = std::move(Sections);
        mReplacementWorldModelCount = ModelCount;
    }
    void ReplaceCollision(std::unique_ptr<CCollisionMesh> pMesh) { mpReplacementCollision = std::move(pMesh); }
    void SetClearPathOnCook(bool Clear) { mClearPathOnCook = Clear; }
    bool HasReplacedWorldGeometry() const { return !mReplacementGeometrySections.empty(); }
    bool HasReplacedCollision() const { return mpReplacementCollision != nullptr; }

    explicit CGameArea(CResourceEntry *pEntry = nullptr);
    ~CGameArea() override;
    std::unique_ptr<CDependencyTree> BuildDependencyTree() override;

    void AddWorldModel(std::unique_ptr<CModel>&& pModel);
    void MergeTerrain();
    void ClearTerrain();
    void ClearScriptLayers();
    size_t TotalInstanceCount() const;
    CScriptObject* InstanceByID(CInstanceID ID);
    CInstanceID FindUnusedInstanceID() const;
    CScriptObject* SpawnInstance(CScriptTemplate* pTemplate, CScriptLayer* pLayer,
                                 const CVector3f& rkPosition = CVector3f::Zero(),
                                 const CQuaternion& rkRotation = CQuaternion::Identity(),
                                 const CVector3f& rkScale = CVector3f::One(),
                                 CInstanceID SuggestedID = {},
                                 uint32_t SuggestedLayerIndex = UINT32_MAX);
    void AddInstanceToArea(CScriptObject *pInstance);
    void DeleteInstance(CScriptObject *pInstance);
    void ClearExtraDependencies();

    // Accessors
    uint32_t WorldIndex() const                                  { return mWorldIndex; }
    const CTransform4f& Transform() const                        { return mTransform; }
    CMaterialSet* Materials() const                              { return mpMaterialSet.get(); }

    auto TerrainModels() const                                   { return std::views::transform(mWorldModels, [](const auto& entry) { return entry.get(); }); }
    auto StaticModels() const                                    { return std::views::transform(mStaticWorldModels, [](const auto& entry)  { return entry.get(); }); }
    auto ScriptLayers() const                                    { return std::views::transform(mScriptLayers, [](const auto& entry) { return entry.get(); }); }
    CScriptLayer* ScriptLayer(size_t Index) const                { return mScriptLayers[Index].get(); }
    size_t NumScriptLayers() const                               { return mScriptLayers.size(); }

    auto LightLayers()                                           { return std::views::all(mLightLayers); }
    auto Lights(size_t LayerIndex)                               { return std::views::all(mLightLayers[LayerIndex]); }

    size_t NumLightLayers() const                                { return mLightLayers.size(); }
    size_t NumLights(size_t LayerIndex) const                    { return (LayerIndex < mLightLayers.size() ? mLightLayers[LayerIndex].size() : 0); }

    CCollisionMeshGroup* Collision() const                       { return mpCollision.get(); }
    const CAssetID& PathID() const                               { return mPathID; }
    CPoiToWorld* PoiToWorldMap() const                           { return mpPoiToWorldMap; }
    const CAssetID& PortalAreaID() const                         { return mPortalAreaID; }
    const CAABox& AABox() const                                  { return mAABox; }

    void SetWorldIndex(uint32_t NewWorldIndex)                   { mWorldIndex = NewWorldIndex; }
};

#endif // CGAMEAREA_H
