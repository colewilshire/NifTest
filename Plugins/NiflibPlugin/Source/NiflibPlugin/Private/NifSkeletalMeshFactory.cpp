#include "NifSkeletalMeshFactory.h"
#include "NiflibBridge.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "MeshUtilities.h"
#include "MeshUtilitiesCommon.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "ImportUtils/SkeletalMeshImportUtils.h"
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"
#include "BoneWeights.h"

UNifSkeletalMeshFactory::UNifSkeletalMeshFactory()
{
    bEditorImport = true;
    SupportedClass = USkeletalMesh::StaticClass();
    Formats.Add(TEXT("nif;Gamebryo NIF"));
}

bool UNifSkeletalMeshFactory::FactoryCanImport(const FString& Filename)
{
    return Filename.EndsWith(TEXT(".nif"), ESearchCase::IgnoreCase);
}

// Create a unique package under the same folder as the selected destination
static UPackage* MakeAssetPackage(const FString& BasePath, const FString& AssetName, FString& OutObjectName)
{
    FString PackageName;
    FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    AssetTools.Get().CreateUniqueAssetName(BasePath / AssetName, TEXT(""), PackageName, OutObjectName);
    return CreatePackage(*PackageName);
}

// Small helper to build one LOD from FNifMeshData into the SkeletalMesh
static bool BuildOneLOD(
    int32 LODIndex,
    const FNifMeshData& Mesh,
    USkeletalMesh* SkeletalMesh,
    const FReferenceSkeleton& RefSkeleton,
    bool& bOutHasImportNormals)
{
    using namespace SkeletalMeshImportData;

    // Points
    TArray<FVector3f> Points;
    Points.Reserve(Mesh.Vertices.Num());
    bOutHasImportNormals = false;

    for (const FNifVertex& V : Mesh.Vertices)
    {
        Points.Add(V.Position);
        if (!bOutHasImportNormals && !V.Normal.IsNearlyZero(1e-6f))
            bOutHasImportNormals = true;
    }

    // Wedges/Faces
    TArray<FMeshWedge> Wedges;
    Wedges.Reserve(Mesh.Faces.Num() * 3);
    TArray<FMeshFace> Faces;
    Faces.Reserve(Mesh.Faces.Num());

    for (const FNifFace& F : Mesh.Faces)
    {
        FMeshFace Face{};
        Face.MeshMaterialIndex = (uint16)F.MaterialIndex;
        Face.SmoothingGroups = 1;

        for (int32 c = 0; c < 3; ++c)
        {
            const int32 VertIdx = F.Indices[c];
            const FNifVertex& V = Mesh.Vertices[VertIdx];

            FMeshWedge W{};
            W.iVertex = (uint32)VertIdx;
            W.UVs[0] = V.UV;
            W.Color = FColor::White;

            Face.iWedge[c] = (uint32)Wedges.Add(W);
            Face.TangentX[c] = FVector3f::ZeroVector;
            Face.TangentY[c] = FVector3f::ZeroVector;
            Face.TangentZ[c] = bOutHasImportNormals ? V.Normal : FVector3f::ZeroVector;
        }
        Faces.Add(Face);
    }

    // Influences
    TArray<FVertInfluence> Influences;
    Influences.Reserve(Mesh.Vertices.Num() * 4);
    for (int32 VertIndex = 0; VertIndex < Mesh.Vertices.Num(); ++VertIndex)
    {
        for (const FNifVertexInfluence& Inf : Mesh.Vertices[VertIndex].Influences)
        {
            if (Inf.BoneIndex < 0) continue;
            FVertInfluence R{};
            R.Weight = Inf.Weight;
            R.VertIndex = VertIndex;
            R.BoneIndex = (FBoneIndexType)Inf.BoneIndex;
            Influences.Add(R);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[NIF] LOD%d Pre-normalization: Wedges=%d, Faces=%d, Influences=%d"),
        LODIndex, Wedges.Num(), Faces.Num(), Influences.Num());

    // Normalize influences (engine utility)
    int32 ZeroInfluenceVertexCount = 0;
    {
        FSkeletalMeshImportData Temp;
        for (const FVertInfluence& v : Influences)
        {
            SkeletalMeshImportData::FRawBoneInfluence Raw{};
            Raw.Weight = v.Weight;
            Raw.VertexIndex = v.VertIndex;
            Raw.BoneIndex = v.BoneIndex;
            Temp.Influences.Add(Raw);
        }

        SkeletalMeshImportUtils::ProcessImportMeshInfluences(Temp, SkeletalMesh->GetName());

        // Count zero-influence verts
        TArray<int32> InfCount;
        InfCount.Init(0, Points.Num());
        for (const auto& Raw : Temp.Influences)
            if (Raw.VertexIndex >= 0 && Raw.VertexIndex < InfCount.Num())
                ++InfCount[Raw.VertexIndex];

        for (int32 i = 0; i < InfCount.Num(); ++i)
            if (InfCount[i] == 0) ++ZeroInfluenceVertexCount;

        Influences.Empty(Temp.Influences.Num());
        for (const auto& Raw : Temp.Influences)
        {
            FVertInfluence v{};
            v.Weight = Raw.Weight;
            v.VertIndex = Raw.VertexIndex;
            v.BoneIndex = (FBoneIndexType)Raw.BoneIndex;
            Influences.Add(v);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("[NIF] LOD%d Post-normalization: Influences=%d, ZeroInfluenceVerts=%d"),
        LODIndex, Influences.Num(), ZeroInfluenceVertexCount);

    // Validate influences
    {
        const uint32 NumPointsU = static_cast<uint32>(Points.Num());
        const uint32 NumBonesU = static_cast<uint32>(RefSkeleton.GetRawBoneNum());

        for (int32 i = Influences.Num() - 1; i >= 0; --i)
        {
            const FVertInfluence& I = Influences[i];
            const bool bBadVert = (static_cast<uint32>(I.VertIndex) >= NumPointsU);
            const bool bBadBone = (static_cast<uint32>(I.BoneIndex) >= NumBonesU);
            const bool bBadW = (!FMath::IsFinite(I.Weight) || I.Weight <= 0.f);
            if (bBadVert || bBadBone || bBadW)
            {
                Influences.RemoveAtSwap(i, 1, false);
            }
        }
    }

    // Identity map
    TArray<int32> PointToOriginalMap;
    PointToOriginalMap.Reserve(Points.Num());
    for (int32 i = 0; i < Points.Num(); ++i)
        PointToOriginalMap.Add(i);

    // Ensure LODInfo entry exists
    while (SkeletalMesh->GetLODNum() <= LODIndex)
    {
        SkeletalMesh->AddLODInfo();
    }
    FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(LODIndex);
    check(LODInfo);

    // Build settings
    LODInfo->BuildSettings.bRecomputeNormals = !bOutHasImportNormals;
    LODInfo->BuildSettings.bRecomputeTangents = true;
    LODInfo->BuildSettings.bUseMikkTSpace = true;

    FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
    check(ImportedModel);
    while (ImportedModel->LODModels.Num() <= LODIndex)
    {
        ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
    }
    FSkeletalMeshLODModel* NewLODModel = &ImportedModel->LODModels[LODIndex];

    // Build render/CPU LOD data
    IMeshUtilities& MeshUtils = FModuleManager::LoadModuleChecked<IMeshUtilities>("MeshUtilities");
    IMeshUtilities::MeshBuildOptions BuildOptions;
    BuildOptions.bComputeNormals = !bOutHasImportNormals;
    BuildOptions.bComputeTangents = true;
    BuildOptions.bUseMikkTSpace = true;

    TArray<FText> WarningMsgs;
    TArray<FName> WarningNames;

    const bool bBuilt = MeshUtils.BuildSkeletalMesh(
        *NewLODModel,
        SkeletalMesh->GetName(),
        RefSkeleton,
        Influences,
        Wedges,
        Faces,
        Points,
        PointToOriginalMap,
        BuildOptions,
        &WarningMsgs,
        &WarningNames
    );

    for (const FText& W : WarningMsgs)
    {
        UE_LOG(LogTemp, Warning, TEXT("[NIF] LOD%d %s"), LODIndex, *W.ToString());
    }

    if (!bBuilt)
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Skeletal mesh build failed for LOD%d."), LODIndex);
        return false;
    }

    // Ensure LOD reports at least one UV channel (NumTexCoords lives on LODModel in UE 5.4)
    NewLODModel->NumTexCoords = FMath::Max<uint32>(NewLODModel->NumTexCoords, 1u);

    // Sanity: count non-zero UV0s across sections after build (verifies they made it through)
    {
        int32 NonZeroSectionUV0 = 0;
        int32 TotalSectionVerts = 0;
        for (const FSkelMeshSection& Sec : NewLODModel->Sections)
        {
            TotalSectionVerts += Sec.NumVertices;
            for (int32 vi = 0; vi < Sec.SoftVertices.Num(); ++vi)
            {
                const FVector2f& uv0 = Sec.SoftVertices[vi].UVs[0];
                if (!uv0.IsNearlyZero(1e-6f))
                {
                    ++NonZeroSectionUV0;
                }
            }
        }
        UE_LOG(LogTemp, Log, TEXT("[NIF] LOD%d UV0 non-zero verts: %d / %d"),
            LODIndex, NonZeroSectionUV0, TotalSectionVerts);
    }

    // Validate LOD
    if (NewLODModel->Sections.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Built LOD%d has no sections."), LODIndex);
        return false;
    }

    // Materials slots (minimum)
    int32 MaxSectionMatIndex = -1;
    for (const FSkelMeshSection& Sec : NewLODModel->Sections)
        MaxSectionMatIndex = FMath::Max(MaxSectionMatIndex, (int32)Sec.MaterialIndex);

    if (MaxSectionMatIndex >= 0)
        while (SkeletalMesh->GetMaterials().Num() <= MaxSectionMatIndex)
            SkeletalMesh->GetMaterials().Add(FSkeletalMaterial());

    // Bounds (from this LOD’s points)
    {
        FBox BoundsBox(ForceInit);
        for (const FVector3f& P : Points)
            BoundsBox += (FVector)P;
        if (BoundsBox.IsValid)
            SkeletalMesh->SetImportedBounds(FBoxSphereBounds(BoundsBox));
    }

    return true;
}

UObject* UNifSkeletalMeshFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    UE_LOG(LogTemp, Log, TEXT("[NIF] Importing %s"), *Filename);

    // First: build LOD0 (explicit LOD request = 0)
    FNifMeshData MeshLOD0;
    FNifAnimationData Anim0;
    if (!FNiflibBridge::ParseNifFileWithLOD(Filename, 0, MeshLOD0, Anim0))
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Parse failed (LOD0): %s"), *Filename);
        bOutOperationCanceled = true;
        return nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("[NIF] Raw LOD0 counts: Bones=%d, Vertices=%d, Faces=%d, Materials=%d"),
        MeshLOD0.Bones.Num(), MeshLOD0.Vertices.Num(), MeshLOD0.Faces.Num(), MeshLOD0.Materials.Num());

    // Create packages/assets
    const FString BasePath = InParent->GetOutermost()->GetName();

    FString SkelObjName, MeshObjName;
    UPackage* SkelPkg = MakeAssetPackage(BasePath, InName.ToString() + TEXT("_Skeleton"), SkelObjName);
    UPackage* MeshPkg = MakeAssetPackage(BasePath, InName.ToString(), MeshObjName);

    USkeleton* Skeleton = NewObject<USkeleton>(SkelPkg, *SkelObjName, RF_Public | RF_Standalone);
    USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(MeshPkg, *MeshObjName, RF_Public | RF_Standalone);
    SkeletalMesh->SetSkeleton(Skeleton);

    // Reference skeleton from LOD0
    FReferenceSkeleton RefSkeleton(true);
    {
        FReferenceSkeletonModifier Mod(RefSkeleton, nullptr);
        for (int32 i = 0; i < MeshLOD0.Bones.Num(); ++i)
        {
            const FNifBone& B = MeshLOD0.Bones[i];
            const int32 ParentIndex = FMath::Max(-1, B.ParentIndex);

#if WITH_EDITORONLY_DATA
            FMeshBoneInfo BoneInfo(*B.Name, B.Name, ParentIndex);
#else
            FMeshBoneInfo BoneInfo(*B.Name, FString(), ParentIndex);
#endif
            Mod.Add(BoneInfo, FTransform(B.BindPose), false);
        }
    }
    SkeletalMesh->SetRefSkeleton(RefSkeleton);

    // Build LOD0
    SkeletalMesh->GetImportedModel()->LODModels.Empty();
    SkeletalMesh->GetLODInfoArray().Empty();
    bool bHasImportNormalsLOD = false;
    SkeletalMesh->AddLODInfo();
    if (!BuildOneLOD(0, MeshLOD0, SkeletalMesh, RefSkeleton, bHasImportNormalsLOD))
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Failed building LOD0."));
        bOutOperationCanceled = true;
        return nullptr;
    }

    // Minimal material slots from LOD0 names
    {
        FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
        int32 MaxSectionMatIndex = -1;
        for (const FSkelMeshSection& Sec : ImportedModel->LODModels[0].Sections)
            MaxSectionMatIndex = FMath::Max(MaxSectionMatIndex, (int32)Sec.MaterialIndex);

        if (MaxSectionMatIndex >= 0)
            while (SkeletalMesh->GetMaterials().Num() <= MaxSectionMatIndex)
                SkeletalMesh->GetMaterials().Add(FSkeletalMaterial());

        for (int32 SlotIdx = 0; SlotIdx < MeshLOD0.Materials.Num(); ++SlotIdx)
            if (SkeletalMesh->GetMaterials().IsValidIndex(SlotIdx))
                SkeletalMesh->GetMaterials()[SlotIdx].MaterialSlotName = *MeshLOD0.Materials[SlotIdx].Name;

        UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
        for (int32 SlotIdx = 0; SlotIdx < SkeletalMesh->GetMaterials().Num(); ++SlotIdx)
        {
            FSkeletalMaterial& Slot = SkeletalMesh->GetMaterials()[SlotIdx];
            if (Slot.MaterialInterface == nullptr)
                Slot.MaterialInterface = DefaultMat;
        }
    }

    // Try to add successive LODs: LOD1, LOD2, ... until parse returns no faces
    // Cap by authored LOD count to avoid generating extra slots
    int32 AuthoredLODCount = FNiflibBridge::GetAuthoredLODCount(Filename);
    // We already built LOD0; start at 1 and stop before AuthoredLODCount
    const int32 MaxRequestedLOD = FMath::Max(1, AuthoredLODCount - 1);

    for (int32 LodIdx = 1; LodIdx <= MaxRequestedLOD; ++LodIdx)
    {
        FNifMeshData MeshLodN;
        FNifAnimationData AnimN;
        if (!FNiflibBridge::ParseNifFileWithLOD(Filename, LodIdx, MeshLodN, AnimN))
        {
            UE_LOG(LogTemp, Log, TEXT("[NIF] LOD%d parse returned no geometry; stopping."), LodIdx);
            break;
        }

        if (MeshLodN.Faces.Num() == 0 || MeshLodN.Vertices.Num() == 0)
        {
            UE_LOG(LogTemp, Log, TEXT("[NIF] LOD%d empty; stopping."), LodIdx);
            break;
        }

        UE_LOG(LogTemp, Log, TEXT("[NIF] Raw LOD%d counts: Bones=%d, Vertices=%d, Faces=%d, Materials=%d"),
            LodIdx, MeshLodN.Bones.Num(), MeshLodN.Vertices.Num(), MeshLodN.Faces.Num(), MeshLodN.Materials.Num());

        bool bHasImportNormalsThisLOD = false;
        SkeletalMesh->AddLODInfo();
        if (!BuildOneLOD(LodIdx, MeshLodN, SkeletalMesh, RefSkeleton, bHasImportNormalsThisLOD))
        {
            UE_LOG(LogTemp, Warning, TEXT("[NIF] Failed building LOD%d; stopping further LODs."), LodIdx);
            break;
        }
    }


    SkeletalMesh->InvalidateDeriveDataCacheGUID();

    // Finalize
    Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
    SkeletalMesh->CalculateInvRefMatrices();
    SkeletalMesh->PostEditChange();
    Skeleton->PostEditChange();

    // Register
    FAssetRegistryModule::AssetCreated(Skeleton);
    FAssetRegistryModule::AssetCreated(SkeletalMesh);
    SkelPkg->MarkPackageDirty();
    MeshPkg->MarkPackageDirty();

    UE_LOG(LogTemp, Log, TEXT("[NIF] Imported SkeletalMesh %s  (LODs: %d)"),
        *MeshObjName, SkeletalMesh->GetImportedModel()->LODModels.Num());

    // Force reload to auto-generate missing MeshDescription
    SkeletalMesh->PostLoad();

    return SkeletalMesh;
}
