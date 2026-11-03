#ifndef CAREACOOKER_H
#define CAREACOOKER_H


#include <Common/EGame.h>
#include <Common/FileIO/CVectorOutStream.h>
#include <Core/Resource/TResPtr.h>

#include <vector>

class CGameArea;
class IOutputStream;

class CAreaCooker
{
    TResPtr<CGameArea> mpArea;
    EGame mVersion{};

    std::vector<uint32_t> mSectionSizes;

    uint32_t mGeometrySecNum = UINT32_MAX;
    uint32_t mSCLYSecNum = UINT32_MAX;
    uint32_t mSCGNSecNum = UINT32_MAX;
    uint32_t mCollisionSecNum = UINT32_MAX;
    uint32_t mUnknownSecNum = UINT32_MAX;
    uint32_t mLightsSecNum = UINT32_MAX;
    uint32_t mVISISecNum = UINT32_MAX;
    uint32_t mPATHSecNum = UINT32_MAX;
    uint32_t mAROTSecNum = UINT32_MAX;
    uint32_t mFFFFSecNum = UINT32_MAX;
    uint32_t mPTLASecNum = UINT32_MAX;
    uint32_t mEGMCSecNum = UINT32_MAX;
    uint32_t mDepsSecNum = UINT32_MAX;
    uint32_t mModulesSecNum = UINT32_MAX;

    struct SCompressedBlock
    {
        uint32_t CompressedSize = 0;
        uint32_t DecompressedSize = 0;
        uint32_t NumSections = 0;
    };

    SCompressedBlock mCurBlock;
    CVectorOutStream mSectionData;
    CVectorOutStream mCompressedData;
    CVectorOutStream mAreaData;

    std::vector<SCompressedBlock> mCompressedBlocks;

    // The sections written: the area's original buffers, or a copy with replacements applied
    // (ApplyReplacements). The header's model count comes with them.
    const std::vector<std::vector<uint8_t>>* mpBuffers = nullptr;
    std::vector<std::vector<uint8_t>> mReplacedBuffers;
    uint32_t mModelCount = 0;

    CAreaCooker();
    void DetermineSectionNumbersPrime();
    bool ApplyReplacements();
    void DetermineSectionNumbersCorruption();

    // Header
    void WritePrimeHeader(IOutputStream& rOut);
    void WriteCorruptionHeader(IOutputStream& rOut);
    void WriteCompressionHeader(IOutputStream& rOut);
    void WriteAreaData(IOutputStream& rOut);

    // SCLY
    void WritePrimeSCLY(IOutputStream& rOut);
    void WriteEchoesSCLY(IOutputStream& rOut);

    // Other Sections
    void WriteDependencies(IOutputStream& rOut);
    void WriteModules(IOutputStream& rOut);

    // Section Management
    void AddSectionToBlock();
    void FinishSection(bool ForceFinishBlock);
    void FinishBlock();

public:
    static bool CookMREA(CGameArea *pArea, IOutputStream& rOut);
    /** The same for a given game (an area with no resource entry, as in tests/, reports none). */
    static bool CookMREA(CGameArea *pArea, EGame Game, IOutputStream& rOut);
    static uint32_t GetMREAVersion(EGame Version);
};

#endif // CAREACOOKER_H
