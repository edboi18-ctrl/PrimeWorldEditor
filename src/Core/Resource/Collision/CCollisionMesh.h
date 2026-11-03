#ifndef CCOLLISIONMESH_H
#define CCOLLISIONMESH_H

#include "Core/Resource/Collision/CCollisionRenderData.h"
#include "Core/Resource/Collision/SCollisionIndexData.h"
#include <Common/Math/CAABox.h>

/** Base class of collision geometry */
class CCollisionMesh
{
    friend class CCollisionLoader;
    friend class CCollisionCooker;

protected:
    CAABox                  mAABox;
    SCollisionIndexData     mIndexData;
    CCollisionRenderData    mRenderData;

public:
    virtual ~CCollisionMesh() = default;
    virtual void BuildRenderData();

    /** Accessors */
    const CAABox& Bounds() const
    {
        return mAABox;
    }

    const SCollisionIndexData& GetIndexData() const
    {
        return mIndexData;
    }

    const CCollisionRenderData& GetRenderData() const
    {
        return mRenderData;
    }

    CCollisionRenderData& GetRenderData()
    {
        return mRenderData;
    }
};

#endif // CCOLLISIONMESH_H
