#include "Core/Resource/Cooker/CAreaCooker.h"

#include "Core/CompressionUtil.h"
#include "Core/GameProject/DependencyListBuilders.h"
#include "Core/Resource/Area/CGameArea.h"
#include "Core/Resource/Cooker/CCollisionCooker.h"
#include "Core/Resource/Cooker/CScriptCooker.h"
#include <Common/Log.h>

constexpr bool gkForceDisableCompression = false;

CAreaCooker::CAreaCooker() = default;

void CAreaCooker::DetermineSectionNumbersPrime()
{
    mGeometrySecNum = 0;

    // Determine how many sections are taken up by geometry...
    // Each world mesh has 7-9 sections (depending on game) plus one section per surface.
    uint32_t GeometrySections = 0;
    const uint32_t OriginalMeshCount = mpArea->mOriginalWorldMeshCount;

    switch (mVersion)
    {
    case EGame::PrimeDemo:
    case EGame::Prime:
        GeometrySections = 1 + (7 * OriginalMeshCount); // Accounting for materials
        break;
    case EGame::EchoesDemo:
        GeometrySections = 2 + (9 * OriginalMeshCount); // Account for materials + AROT
        break;
    case EGame::Echoes:
        GeometrySections = 3 + (9 * OriginalMeshCount); // Acount for materials + AROT + an unknown section
        break;
    default:
        break;
    }

    for (const auto& worldModel : mpArea->mWorldModels)
        GeometrySections += worldModel->GetSurfaceCount();

    // Set section numbers
    uint32_t SecNum = GeometrySections;
    if (mVersion <= EGame::Prime)
        mAROTSecNum = SecNum++;
    if (mVersion >= EGame::EchoesDemo)
        mFFFFSecNum = SecNum++;

    if (mVersion >= EGame::EchoesDemo)
    {
        mSCLYSecNum = SecNum;
        SecNum += (mVersion >= EGame::Echoes ? mpArea->mScriptLayers.size() : 1);
        mSCGNSecNum = SecNum++;
    }
    else
    {
        mSCLYSecNum = SecNum++;
    }

    mCollisionSecNum = SecNum++;
    mUnknownSecNum = SecNum++;
    mLightsSecNum = SecNum++;
    mVISISecNum = SecNum++;
    mPATHSecNum = SecNum++;

    if (mVersion >= EGame::EchoesDemo)
    {
        mPTLASecNum = SecNum++;
        mEGMCSecNum = SecNum++;
    }
}

void CAreaCooker::DetermineSectionNumbersCorruption()
{
    // Because we're copying these from the original file (because not all the numbers
    // are present in every file), we don't care about any of these except SCLY and SCGN.
    for (const auto& num : mpArea->mSectionNumbers)
    {
        if (num.SectionID == CFourCC("SOBJ"))
            mSCLYSecNum = num.Index;
        else if (num.SectionID == CFourCC("SGEN"))
            mSCGNSecNum = num.Index;
        else if (num.SectionID == CFourCC("DEPS"))
            mDepsSecNum = num.Index;
        else if (num.SectionID == CFourCC("RSOS"))
            mModulesSecNum = num.Index;
    }
}

bool CAreaCooker::ApplyReplacements()
{
    const auto& Original = mpArea->mSectionDataBuffers;
    mpBuffers = &Original;
    mModelCount = mpArea->mOriginalWorldMeshCount;

    const bool Geometry = mpArea->HasReplacedWorldGeometry();
    const bool Collision = mpArea->HasReplacedCollision();
    if (!Geometry && !Collision && !mpArea->mClearPathOnCook)
        return true;
    if (mVersion != EGame::Echoes)
    {
        NLog::Error("Replacing a room's geometry, collision or pathfinding is only supported for Echoes");
        return false;
    }
    if (mSCLYSecNum > Original.size() || mEGMCSecNum >= Original.size())
    {
        NLog::Error("The area's original sections do not match its section numbers; cannot replace them");
        return false;
    }

    // Geometry is everything before the first script layer: materials, models, the render octree,
    // the area surface list and the ambient light table (CGameArea::PostConstructArea's order).
    mReplacedBuffers.clear();
    if (Geometry)
        mReplacedBuffers = mpArea->mReplacementGeometrySections;
    else
        mReplacedBuffers.assign(Original.begin(), Original.begin() + mSCLYSecNum);
    const int64_t Delta = int64_t(mReplacedBuffers.size()) - int64_t(mSCLYSecNum);
    mReplacedBuffers.insert(mReplacedBuffers.end(), Original.begin() + mSCLYSecNum, Original.end());

    for (uint32_t* pNum : {&mSCLYSecNum, &mSCGNSecNum, &mCollisionSecNum, &mUnknownSecNum, &mLightsSecNum,
                           &mVISISecNum, &mPATHSecNum, &mPTLASecNum, &mEGMCSecNum})
        *pNum = static_cast<uint32_t>(int64_t(*pNum) + Delta);
    if (Geometry)
    {
        mFFFFSecNum = mSCLYSecNum - 1;   // the section before the first layer (the header word the game
        mModelCount = mpArea->mReplacementWorldModelCount;   // reads as the render octree's, != -1)
    }

    auto AssetIDSection = [](uint32_t ID) {
        std::vector<uint8_t> Out(32, 0);
        for (int i = 0; i < 4; i++)
            Out[i] = uint8_t(ID >> (24 - 8 * i));
        return Out;
    };
    if (Geometry)
    {
        // Both index the old surfaces. A PVS section of 64 bytes or less is ignored, and an EGMC ID
        // of -1 is not loaded (PostConstructArea).
        mReplacedBuffers[mVISISecNum].assign(32, 0);
        mReplacedBuffers[mEGMCSecNum] = AssetIDSection(0xFFFFFFFF);
    }
    if (mpArea->mClearPathOnCook)
        mReplacedBuffers[mPATHSecNum] = AssetIDSection(0xFFFFFFFF);
    if (Collision)
    {
        CVectorOutStream Section(std::endian::big);
        std::string Error;
        if (!CCollisionCooker::WriteAreaCollisionSection(*mpArea->mpReplacementCollision, Section, mVersion, {}, &Error))
        {
            NLog::Error("Cannot cook the replacement collision: {}", Error);
            return false;
        }
        Section.WriteToBoundary(32, 0);
        const auto* pData = static_cast<const uint8_t*>(Section.Data());
        mReplacedBuffers[mCollisionSecNum].assign(pData, pData + Section.Size());
    }
    mpBuffers = &mReplacedBuffers;
    return true;
}

// ************ HEADER ************
void CAreaCooker::WritePrimeHeader(IOutputStream& rOut)
{
    rOut.WriteU32(0xDEADBEEF);
    rOut.WriteU32(GetMREAVersion(mVersion));
    mpArea->mTransform.Write(rOut);
    rOut.WriteU32(mModelCount);
    if (mVersion >= EGame::Echoes)
        rOut.WriteU32(static_cast<uint32_t>(mpArea->mScriptLayers.size()));
    rOut.WriteU32(static_cast<uint32_t>(mpBuffers->size()));

    rOut.WriteU32(mGeometrySecNum);
    rOut.WriteU32(mSCLYSecNum);
    if (mVersion >= EGame::EchoesDemo)
        rOut.WriteU32(mSCGNSecNum);
    rOut.WriteU32(mCollisionSecNum);
    rOut.WriteU32(mUnknownSecNum);
    rOut.WriteU32(mLightsSecNum);
    rOut.WriteU32(mVISISecNum);
    rOut.WriteU32(mPATHSecNum);
    if (mVersion <= EGame::Prime)
        rOut.WriteU32(mAROTSecNum);

    else
    {
        rOut.WriteU32(mFFFFSecNum);
        rOut.WriteU32(mPTLASecNum);
        rOut.WriteU32(mEGMCSecNum);
    }

    if (mVersion >= EGame::EchoesDemo)
    {
        if (mVersion >= EGame::Echoes)
            rOut.WriteU32(static_cast<uint32_t>(mCompressedBlocks.size()));
        rOut.WriteToBoundary(32, 0);
    }

    for (const uint32_t size : mSectionSizes)
        rOut.WriteU32(size);
    rOut.WriteToBoundary(32, 0);

    if (mVersion >= EGame::Echoes)
        WriteCompressionHeader(rOut);
}

void CAreaCooker::WriteCorruptionHeader(IOutputStream& rOut)
{
    rOut.WriteU32(0xDEADBEEF);
    rOut.WriteU32(GetMREAVersion(mVersion));
    mpArea->mTransform.Write(rOut);
    rOut.WriteU32(mpArea->mOriginalWorldMeshCount);
    rOut.WriteU32(static_cast<uint32_t>(mpArea->mScriptLayers.size()));
    rOut.WriteU32(static_cast<uint32_t>(mpArea->mSectionDataBuffers.size()));
    rOut.WriteU32(static_cast<uint32_t>(mCompressedBlocks.size()));
    rOut.WriteU32(static_cast<uint32_t>(mpArea->mSectionNumbers.size()));
    rOut.WriteToBoundary(32, 0);

    for (const uint32_t size : mSectionSizes)
        rOut.WriteU32(size);

    rOut.WriteToBoundary(32, 0);

    WriteCompressionHeader(rOut);

    for (const auto& num : mpArea->mSectionNumbers)
    {
        rOut.WriteU32(num.SectionID.ToU32());
        rOut.WriteU32(num.Index);
    }
    rOut.WriteToBoundary(32, 0);
}

void CAreaCooker::WriteCompressionHeader(IOutputStream& rOut)
{
    for (const auto& block : mCompressedBlocks)
    {
        const bool IsCompressed = block.CompressedSize != 0;

        rOut.WriteU32(IsCompressed ? block.DecompressedSize + 0x120 : block.DecompressedSize);
        rOut.WriteU32(block.DecompressedSize);
        rOut.WriteU32(block.CompressedSize);
        rOut.WriteU32(block.NumSections);
    }

    rOut.WriteToBoundary(32, 0);
}

void CAreaCooker::WriteAreaData(IOutputStream& rOut)
{
    rOut.WriteBytes(mAreaData.Data(), mAreaData.Size());
    rOut.WriteToBoundary(32, 0);
}

// ************ SCLY ************
void CAreaCooker::WritePrimeSCLY(IOutputStream& rOut)
{
    // This function covers both Prime 1 and the Echoes demo.
    // The Echoes demo has a similar SCLY format but with minor layout differences and with SCGN.
    rOut.WriteFourCC(CFourCC("SCLY"));
    mVersion <= EGame::Prime ? rOut.WriteU32(1) : rOut.WriteU8(1);

    const auto NumLayers = static_cast<uint32_t>(mpArea->mScriptLayers.size());
    rOut.WriteU32(NumLayers);

    const uint32_t LayerSizesStart = rOut.Tell();
    for (uint32_t LayerIdx = 0; LayerIdx < NumLayers; LayerIdx++)
        rOut.WriteU32(0);

    // SCLY
    CScriptCooker ScriptCooker(mVersion, true);
    std::vector<uint32_t> LayerSizes(NumLayers);

    for (uint32_t LayerIdx = 0; LayerIdx < NumLayers; LayerIdx++)
    {
        const uint32_t LayerStart = rOut.Tell();
        ScriptCooker.WriteLayer(rOut, mpArea->mScriptLayers[LayerIdx].get());

        // Pad the layer to 32 bytes
        const uint32_t LayerSize = rOut.Tell() - LayerStart;
        const uint32_t PaddedSize = (LayerSize + 31) & ~31;
        const uint32_t NumPadBytes = PaddedSize - LayerSize;

        for (uint32_t Pad = 0; Pad < NumPadBytes; Pad++)
            rOut.WriteU8(0);

        LayerSizes[LayerIdx] = PaddedSize;
    }

    const auto LayersEnd = rOut.Tell();
    rOut.Seek(LayerSizesStart, SEEK_SET);

    for (uint32_t LayerIdx = 0; LayerIdx < NumLayers; LayerIdx++)
        rOut.WriteU32(LayerSizes[LayerIdx]);

    rOut.Seek(LayersEnd, SEEK_SET);
    FinishSection(false);

    // SCGN
    if (mVersion == EGame::EchoesDemo)
    {
        rOut.WriteFourCC(CFourCC("SCGN"));
        rOut.WriteU8(1);
        ScriptCooker.WriteGeneratedLayer(rOut);
        FinishSection(false);
    }
}

void CAreaCooker::WriteEchoesSCLY(IOutputStream& rOut)
{
    // SCLY
    CScriptCooker ScriptCooker(mVersion);

    for (uint32_t LayerIdx = 0; LayerIdx < mpArea->mScriptLayers.size(); LayerIdx++)
    {
        rOut.WriteFourCC(CFourCC("SCLY"));
        rOut.WriteU8(1);
        rOut.WriteU32(LayerIdx);
        ScriptCooker.WriteLayer(rOut, mpArea->mScriptLayers[LayerIdx].get());
        FinishSection(true);
    }

    // SCGN
    rOut.WriteFourCC(CFourCC("SCGN"));
    rOut.WriteU8(1);
    ScriptCooker.WriteGeneratedLayer(rOut);
    FinishSection(true);
}

void CAreaCooker::WriteDependencies(IOutputStream& rOut)
{
    // Build dependency list
    std::list<CAssetID> Dependencies;
    std::list<uint32_t> LayerOffsets;

    CAreaDependencyListBuilder Builder(mpArea->Entry());
    Builder.BuildDependencyList(Dependencies, LayerOffsets);

    // Write
    rOut.WriteU32(static_cast<uint32_t>(Dependencies.size()));

    for (const auto& dependency : Dependencies)
    {
        const CResourceEntry* pEntry = mpArea->Entry()->ResourceStore()->FindEntry(dependency);
        dependency.Write(rOut);
        pEntry->CookedExtension().Write(rOut);
    }

    rOut.WriteU32(static_cast<uint32_t>(LayerOffsets.size()));

    for (const uint32_t offset : LayerOffsets)
        rOut.WriteU32(offset);

    FinishSection(false);
}

void CAreaCooker::WriteModules(IOutputStream& rOut)
{
    // Build module list
    std::vector<TString> ModuleNames;
    std::vector<uint32_t> LayerOffsets;

    CAreaDependencyTree *pAreaDeps = static_cast<CAreaDependencyTree*>(mpArea->Entry()->Dependencies());
    pAreaDeps->GetModuleDependencies(mpArea->Game(), ModuleNames, LayerOffsets);

    // Write
    rOut.WriteU32(static_cast<uint32_t>(ModuleNames.size()));

    for (const auto& name : ModuleNames)
        rOut.WriteString(name);

    rOut.WriteU32(static_cast<uint32_t>(LayerOffsets.size()));

    for (const uint32_t offset : LayerOffsets)
        rOut.WriteU32(offset);

    FinishSection(false);
}

// ************ SECTION MANAGEMENT ************
void CAreaCooker::AddSectionToBlock()
{
    mCompressedData.WriteBytes(mSectionData.Data(), mSectionData.Size());
    mCompressedData.WriteToBoundary(32, 0);
    mCurBlock.DecompressedSize += mSectionData.Size();
    mCurBlock.NumSections++;
}

void CAreaCooker::FinishSection(bool SingleSectionBlock)
{
    // Our section data is now finished in mSection...
    const uint32_t kSizeThreshold = 0x20000;
    mSectionData.WriteToBoundary(32, 0);

    const uint32_t SecSize = mSectionData.Size();
    mSectionSizes.push_back(SecSize);

    // Only track compressed blocks for MP2+. Write everything to one block for MP1.
    if (mVersion >= EGame::Echoes)
    {
        // Finish the current block if this is a single section block OR if the new section would push the block over the size limit.
        if (mCurBlock.NumSections > 0 && (mCurBlock.DecompressedSize + SecSize > kSizeThreshold || SingleSectionBlock))
            FinishBlock();

        AddSectionToBlock();

        // And finally for a single section block, finish the new block.
        if (SingleSectionBlock)
            FinishBlock();
    }
    else
    {
        AddSectionToBlock();
    }

    mSectionData.Clear();
}

void CAreaCooker::FinishBlock()
{
    if (mCurBlock.NumSections == 0) return;

    std::vector<uint8_t> CompressedBuf(mCompressedData.Size() * 2);
    const bool EnableCompression = (mVersion >= EGame::Echoes) && mpArea->mUsesCompression && !gkForceDisableCompression;
    const bool UseZlib = (mVersion == EGame::DKCReturns);

    uint32_t CompressedSize = 0;
    bool WriteCompressedData = false;

    if (EnableCompression)
    {
        const bool Success = CompressionUtil::CompressSegmentedData(static_cast<uint8_t*>(mCompressedData.Data()), mCompressedData.Size(), CompressedBuf.data(), CompressedSize, UseZlib, true);
        const uint32_t PadBytes = (32 - (CompressedSize % 32)) & 0x1F;
        WriteCompressedData = Success && (CompressedSize + PadBytes < static_cast<uint32_t>(mCompressedData.Size()));
    }

    if (WriteCompressedData)
    {
        uint32_t PadBytes = 32 - (CompressedSize % 32);
        PadBytes &= 0x1F;

        for (uint32_t iPad = 0; iPad < PadBytes; iPad++)
            mAreaData.WriteU8(0);

        mAreaData.WriteBytes(CompressedBuf.data(), CompressedSize);
        mCurBlock.CompressedSize = CompressedSize;
    }

    else
    {
        mAreaData.WriteBytes(mCompressedData.Data(), mCompressedData.Size());
        mAreaData.WriteToBoundary(32, 0);
        mCurBlock.CompressedSize = 0;
    }

    mCompressedData.Clear();
    mCompressedBlocks.push_back(mCurBlock);
    mCurBlock = SCompressedBlock();
}

// ************ STATIC ************
bool CAreaCooker::CookMREA(CGameArea *pArea, IOutputStream& rOut)
{
    return CookMREA(pArea, pArea->Game(), rOut);
}

bool CAreaCooker::CookMREA(CGameArea *pArea, EGame Game, IOutputStream& rOut)
{
    CAreaCooker Cooker;
    Cooker.mpArea = pArea;
    Cooker.mVersion = Game;

    if (Cooker.mVersion <= EGame::Echoes)
        Cooker.DetermineSectionNumbersPrime();
    else
        Cooker.DetermineSectionNumbersCorruption();

    if (!Cooker.ApplyReplacements())
        return false;
    const auto& Buffers = *Cooker.mpBuffers;

    // Write pre-SCLY data sections
    for (uint32_t iSec = 0; iSec < Cooker.mSCLYSecNum; iSec++)
    {
        if (iSec == Cooker.mDepsSecNum)
            Cooker.WriteDependencies(Cooker.mSectionData);

        else
        {
            Cooker.mSectionData.WriteBytes(Buffers[iSec].data(), Buffers[iSec].size());
            Cooker.FinishSection(false);
        }
    }

    // Write SCLY
    if (Cooker.mVersion <= EGame::EchoesDemo)
        Cooker.WritePrimeSCLY(Cooker.mSectionData);
    else
        Cooker.WriteEchoesSCLY(Cooker.mSectionData);

    // Write post-SCLY data sections
    const uint32_t PostSCLY = (Cooker.mVersion <= EGame::Prime ? Cooker.mSCLYSecNum + 1 : Cooker.mSCGNSecNum + 1);
    for (size_t iSec = PostSCLY; iSec < Buffers.size(); iSec++)
    {
        if (iSec == Cooker.mModulesSecNum)
        {
            Cooker.WriteModules(Cooker.mSectionData);
        }
        else
        {
            Cooker.mSectionData.WriteBytes(Buffers[iSec].data(), Buffers[iSec].size());
            Cooker.FinishSection(false);
        }
    }

    Cooker.FinishBlock();

    // Write to actual file
    if (Cooker.mVersion <= EGame::Echoes)
        Cooker.WritePrimeHeader(rOut);
    else
        Cooker.WriteCorruptionHeader(rOut);

    Cooker.WriteAreaData(rOut);
    return true;
}

uint32_t CAreaCooker::GetMREAVersion(EGame Version)
{
    switch (Version)
    {
    case EGame::PrimeDemo:          return 0xC;
    case EGame::Prime:              return 0xF;
    case EGame::EchoesDemo:         return 0x15;
    case EGame::Echoes:             return 0x19;
    case EGame::CorruptionProto:    return 0x1D;
    case EGame::Corruption:         return 0x1E;
    case EGame::DKCReturns:         return 0x20;
    default:                        return 0;
    }
}
