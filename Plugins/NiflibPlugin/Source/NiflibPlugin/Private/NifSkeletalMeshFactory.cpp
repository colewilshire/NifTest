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

    // 1) Parse NIF -> intermediate structs (now collects ALL triangles, UVs, skin, materials)
    FNifMeshData Mesh;
    FNifAnimationData Anim;
    if (!FNiflibBridge::ParseNifFile(Filename, Mesh, Anim))
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Parse failed: %s"), *Filename);
        bOutOperationCanceled = true;
        return nullptr;
    }

    // Diagnostics: show raw counts from the parser
    UE_LOG(LogTemp, Log, TEXT("[NIF] Raw counts: Bones=%d, Vertices=%d, Faces=%d, Materials=%d"),
        Mesh.Bones.Num(), Mesh.Vertices.Num(), Mesh.Faces.Num(), Mesh.Materials.Num());

    // 2) Create packages and assets
    const FString BasePath = InParent->GetOutermost()->GetName();

    FString SkelObjName, MeshObjName;
    UPackage* SkelPkg = MakeAssetPackage(BasePath, InName.ToString() + TEXT("_Skeleton"), SkelObjName);
    UPackage* MeshPkg = MakeAssetPackage(BasePath, InName.ToString(), MeshObjName);

    USkeleton* Skeleton = NewObject<USkeleton>(SkelPkg, *SkelObjName, RF_Public | RF_Standalone);
    USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(MeshPkg, *MeshObjName, RF_Public | RF_Standalone);
    SkeletalMesh->SetSkeleton(Skeleton);

    // 3) Build a Reference Skeleton with FReferenceSkeletonModifier
    FReferenceSkeleton RefSkeleton(/*bOnlyOneRootAllowed*/true);
    {
        FReferenceSkeletonModifier Mod(RefSkeleton, /*USkeleton*/ nullptr);

        // Bones must be added in parent-before-child order
        for (int32 i = 0; i < Mesh.Bones.Num(); ++i)
        {
            const FNifBone& B = Mesh.Bones[i];
            const int32 ParentIndex = FMath::Max(-1, B.ParentIndex);

#if WITH_EDITORONLY_DATA
            FMeshBoneInfo BoneInfo(*B.Name, B.Name, ParentIndex);
#else
            FMeshBoneInfo BoneInfo(*B.Name, FString(), ParentIndex);
#endif
            const FTransform BonePose(B.BindPose);
            Mod.Add(BoneInfo, BonePose, /*bAllowMultipleRoots*/ false);
        }
    }

    SkeletalMesh->SetRefSkeleton(RefSkeleton);

    // 4) Prepare raw arrays for FMeshUtilities::BuildSkeletalMesh
    using namespace SkeletalMeshImportData;

    // Points
    TArray<FVector3f> Points;
    Points.Reserve(Mesh.Vertices.Num());
    for (const FNifVertex& V : Mesh.Vertices)
    {
        Points.Add(V.Position);
    }

    // Wedges + Faces
    TArray<FMeshWedge> Wedges;
    Wedges.Reserve(Mesh.Faces.Num() * 3);
    TArray<FMeshFace>  Faces;
    Faces.Reserve(Mesh.Faces.Num());

    for (const FNifFace& F : Mesh.Faces)
    {
        FMeshFace Face{};
        Face.MeshMaterialIndex = static_cast<uint16>(F.MaterialIndex);
        Face.SmoothingGroups = 1;

        for (int32 c = 0; c < 3; ++c)
        {
            const int32 VertIdx = F.Indices[c];
            const FNifVertex& V = Mesh.Vertices[VertIdx];

            FMeshWedge W{};
            W.iVertex = static_cast<uint32>(VertIdx);
            W.UVs[0] = V.UV;
            W.Color = FColor::White;

            const int32 WedgeIdx = Wedges.Add(W);
            Face.iWedge[c] = static_cast<uint32>(WedgeIdx);

            Face.TangentX[c] = FVector3f::ZeroVector;
            Face.TangentY[c] = FVector3f::ZeroVector;
            Face.TangentZ[c] = FVector3f::ZeroVector;
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

    UE_LOG(LogTemp, Log, TEXT("[NIF] Pre-normalization: Wedges=%d, Faces=%d, Influences=%d"),
        Wedges.Num(), Faces.Num(), Influences.Num());

    // Normalize influences
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

        // Count zero-influence vertices
        {
            TArray<int32> InfCount;
            InfCount.Init(0, Points.Num());
            for (const auto& Raw : Temp.Influences)
            {
                if (Raw.VertexIndex >= 0 && Raw.VertexIndex < InfCount.Num())
                    ++InfCount[Raw.VertexIndex];
            }
            for (int32 i = 0; i < InfCount.Num(); ++i)
                if (InfCount[i] == 0) ++ZeroInfluenceVertexCount;
        }

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

    UE_LOG(LogTemp, Log, TEXT("[NIF] Post-normalization: Influences=%d, ZeroInfluenceVerts=%d"),
        Influences.Num(), ZeroInfluenceVertexCount);

    // Identity map
    TArray<int32> PointToOriginalMap;
    PointToOriginalMap.Reserve(Points.Num());
    for (int32 i = 0; i < Points.Num(); ++i)
        PointToOriginalMap.Add(i);

    // 5) Ensure LOD0
    FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
    check(ImportedModel);
    ImportedModel->LODModels.Empty();

    SkeletalMesh->GetLODInfoArray().Empty();
    SkeletalMesh->AddLODInfo();
    FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(0);
    check(LODInfo);
    LODInfo->BuildSettings.bRecomputeNormals = true;
    LODInfo->BuildSettings.bRecomputeTangents = true;
    LODInfo->BuildSettings.bUseMikkTSpace = true;

    FSkeletalMeshLODModel* NewLODModel = new FSkeletalMeshLODModel();
    ImportedModel->LODModels.Add(NewLODModel);

    // 6) Build
    IMeshUtilities& MeshUtils = FModuleManager::LoadModuleChecked<IMeshUtilities>("MeshUtilities");

    IMeshUtilities::MeshBuildOptions BuildOptions;
    BuildOptions.bComputeNormals = true;
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
        UE_LOG(LogTemp, Warning, TEXT("[NIF] %s"), *W.ToString());

    if (!bBuilt)
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Skeletal mesh build failed."));
        bOutOperationCanceled = true;
        return nullptr;
    }

    // Validate LOD
    if (NewLODModel->NumTexCoords < 1u)
    {
        UE_LOG(LogTemp, Warning, TEXT("[NIF] Built LOD has 0 UV channels; forcing to 1."));
        NewLODModel->NumTexCoords = 1u;
    }

    if (NewLODModel->Sections.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Built LOD has no sections. Aborting."));
        bOutOperationCanceled = true;
        return nullptr;
    }

    // 7) Materials
    int32 MaxSectionMatIndex = -1;
    for (const FSkelMeshSection& Sec : NewLODModel->Sections)
        MaxSectionMatIndex = FMath::Max(MaxSectionMatIndex, (int32)Sec.MaterialIndex);

    if (MaxSectionMatIndex >= 0)
        while (SkeletalMesh->GetMaterials().Num() <= MaxSectionMatIndex)
            SkeletalMesh->GetMaterials().Add(FSkeletalMaterial());

    for (int32 SlotIdx = 0; SlotIdx < Mesh.Materials.Num(); ++SlotIdx)
        if (SkeletalMesh->GetMaterials().IsValidIndex(SlotIdx))
            SkeletalMesh->GetMaterials()[SlotIdx].MaterialSlotName = *Mesh.Materials[SlotIdx].Name;

    {
        UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
        for (int32 SlotIdx = 0; SlotIdx < SkeletalMesh->GetMaterials().Num(); ++SlotIdx)
        {
            FSkeletalMaterial& Slot = SkeletalMesh->GetMaterials()[SlotIdx];
            if (Slot.MaterialInterface == nullptr)
                Slot.MaterialInterface = DefaultMat;
        }
    }

    // Bounds
    {
        FBox BoundsBox(ForceInit);
        for (const FVector3f& P : Points)
            BoundsBox += (FVector)P;
        if (BoundsBox.IsValid)
            SkeletalMesh->SetImportedBounds(FBoxSphereBounds(BoundsBox));
    }

    SkeletalMesh->InvalidateDeriveDataCacheGUID();

    // 8) Finalize
    Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
    SkeletalMesh->CalculateInvRefMatrices();
    SkeletalMesh->PostEditChange();
    Skeleton->PostEditChange();

    // 9) Register
    FAssetRegistryModule::AssetCreated(Skeleton);
    FAssetRegistryModule::AssetCreated(SkeletalMesh);
    SkelPkg->MarkPackageDirty();
    MeshPkg->MarkPackageDirty();

    UE_LOG(LogTemp, Log, TEXT("[NIF] Imported SkeletalMesh %s  (Bones: %d, Tris: %d)"),
        *MeshObjName, Mesh.Bones.Num(), Mesh.Faces.Num());

    return SkeletalMesh;
}
