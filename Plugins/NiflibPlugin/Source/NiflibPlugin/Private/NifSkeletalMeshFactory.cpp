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
#include "BoneWeights.h" // UE::AnimationCore::FBoneWeight / FBoneWeights

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

    // 1) Parse NIF -> intermediate structs
    FNifMeshData Mesh;
    FNifAnimationData Anim;
    if (!FNiflibBridge::ParseNifFile(Filename, Mesh, Anim))
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Parse failed: %s"), *Filename);
        bOutOperationCanceled = true;
        return nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("[NIF] Raw counts: Bones=%d, Vertices=%d, Faces=%d, Materials=%d"),
        Mesh.Bones.Num(), Mesh.Vertices.Num(), Mesh.Faces.Num(), Mesh.Materials.Num());

    // 2) Create packages/assets
    const FString BasePath = InParent->GetOutermost()->GetName();

    FString SkelObjName, MeshObjName;
    UPackage* SkelPkg = MakeAssetPackage(BasePath, InName.ToString() + TEXT("_Skeleton"), SkelObjName);
    UPackage* MeshPkg = MakeAssetPackage(BasePath, InName.ToString(), MeshObjName);

    USkeleton* Skeleton = NewObject<USkeleton>(SkelPkg, *SkelObjName, RF_Public | RF_Standalone);
    USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(MeshPkg, *MeshObjName, RF_Public | RF_Standalone);
    SkeletalMesh->SetSkeleton(Skeleton);

    // 3) Reference skeleton
    FReferenceSkeleton RefSkeleton(true);
    {
        FReferenceSkeletonModifier Mod(RefSkeleton, nullptr);
        for (int32 i = 0; i < Mesh.Bones.Num(); ++i)
        {
            const FNifBone& B = Mesh.Bones[i];
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

    // 4) Build import arrays
    using namespace SkeletalMeshImportData;

    // Points
    TArray<FVector3f> Points;
    Points.Reserve(Mesh.Vertices.Num());
    for (const FNifVertex& V : Mesh.Vertices)
    {
        Points.Add(V.Position);
    }

    // Detect whether we actually have imported normals
    bool bHasImportNormals = false;
    {
        for (const FNifVertex& V : Mesh.Vertices)
        {
            if (!V.Normal.IsNearlyZero(1e-6f))
            {
                bHasImportNormals = true;
                break;
            }
        }
        UE_LOG(LogTemp, Log, TEXT("[NIF] Imported Normals: %s"), bHasImportNormals ? TEXT("yes") : TEXT("no"));
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

            // Use imported normal if present; otherwise leave zero and let UE recompute
            Face.TangentZ[c] = bHasImportNormals ? V.Normal : FVector3f::ZeroVector;
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

    UE_LOG(LogTemp, Log, TEXT("[NIF] Post-normalization: Influences=%d, ZeroInfluenceVerts=%d"),
        Influences.Num(), ZeroInfluenceVertexCount);

    // Validate influences (unsigned bounds checks)
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

    // 5) LOD0 (render data)
    FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
    check(ImportedModel);
    ImportedModel->LODModels.Empty();

    SkeletalMesh->GetLODInfoArray().Empty();
    SkeletalMesh->AddLODInfo();
    FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(0);
    check(LODInfo);

    // If we have imported normals, tell the builder NOT to recompute them.
    LODInfo->BuildSettings.bRecomputeNormals = !bHasImportNormals;
    LODInfo->BuildSettings.bRecomputeTangents = true;
    LODInfo->BuildSettings.bUseMikkTSpace = true;

    FSkeletalMeshLODModel* NewLODModel = new FSkeletalMeshLODModel();
    ImportedModel->LODModels.Add(NewLODModel);

    // 6) Build render/CPU LOD data
    IMeshUtilities& MeshUtils = FModuleManager::LoadModuleChecked<IMeshUtilities>("MeshUtilities");
    IMeshUtilities::MeshBuildOptions BuildOptions;
    BuildOptions.bComputeNormals = !bHasImportNormals; // match BuildSettings
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

    // 6.5) Provide a MeshDescription for editor tools WITHOUT committing (keeps render skinning)
    {
        FMeshDescription* MeshDesc = SkeletalMesh->CreateMeshDescription(0); // stored by the mesh
        if (MeshDesc)
        {
            FSkeletalMeshAttributes Attrs(*MeshDesc);
            Attrs.Register();

            // Create vertices
            TArray<FVertexID> VertIDs;
            VertIDs.SetNum(Points.Num());
            auto Positions = Attrs.GetVertexPositions();
            for (int32 i = 0; i < Points.Num(); ++i)
            {
                const FVertexID VId = MeshDesc->CreateVertex();
                VertIDs[i] = VId;
                Positions[VId] = Points[i];
            }

            // UVs (at least 1 channel)
            auto UVs = Attrs.GetVertexInstanceUVs();
            const int32 UVChannels = FMath::Max<int32>(1, (int32)NewLODModel->NumTexCoords);
            UVs.SetNumChannels(UVChannels);

            // One polygon group
            const FPolygonGroupID PGId = MeshDesc->CreatePolygonGroup();

            // Triangles from Faces/Wedges
            for (const SkeletalMeshImportData::FMeshFace& Face : Faces)
            {
                TArray<FVertexInstanceID> VInstIDs;
                VInstIDs.Reserve(3);

                for (int32 c = 0; c < 3; ++c)
                {
                    const uint32 WedgeIdx = Face.iWedge[c];
                    const auto& W = Wedges[WedgeIdx];

                    const FVertexID VId = VertIDs[(int32)W.iVertex];
                    const FVertexInstanceID VI = MeshDesc->CreateVertexInstance(VId);

                    // Channel 0 UV from wedge
                    UVs.Set(VI, 0, FVector2f(W.UVs[0].X, W.UVs[0].Y));
                    VInstIDs.Add(VI);
                }

                MeshDesc->CreateTriangle(PGId, VInstIDs);
            }

            // Skin Weights (write to MeshDescription for editor tools)
            {
                const int32 MaxInf = 8;
                TArray<TArray<TPair<int32, float>>> PerVertex;
                PerVertex.SetNum(Points.Num());

                for (const FVertInfluence& I : Influences)
                {
                    if (I.Weight > 0.f && I.VertIndex >= 0 && I.VertIndex < (uint32)Points.Num())
                    {
                        PerVertex[I.VertIndex].Add(TPair<int32, float>((int32)I.BoneIndex, I.Weight));
                    }
                }

                // Sort/trim/renormalize per vertex
                for (int32 v = 0; v < PerVertex.Num(); ++v)
                {
                    auto& L = PerVertex[v];
                    if (L.Num() == 0)
                    {
                        L.Add(TPair<int32, float>(0, 1.f));
                    }

                    L.Sort([](const TPair<int32, float>& A, const TPair<int32, float>& B) { return A.Value > B.Value; });
                    if (L.Num() > MaxInf) L.SetNum(MaxInf);

                    float Sum = 0.f; for (const auto& P : L) Sum += P.Value;
                    if (Sum > SMALL_NUMBER) for (auto& P : L) P.Value /= Sum;
                }

                auto VertexSkinWeights = Attrs.GetVertexSkinWeights();
                if (VertexSkinWeights.IsValid())
                {
                    constexpr float kScale = 65535.0f;

                    for (int32 v = 0; v < VertIDs.Num(); ++v)
                    {
                        const FVertexID VId = VertIDs[v];

                        TArray<UE::AnimationCore::FBoneWeight> Weights;
                        Weights.Reserve(PerVertex[v].Num());

                        for (const auto& P : PerVertex[v])
                        {
                            const int32 BoneIdx = FMath::Clamp(P.Key, 0, RefSkeleton.GetRawBoneNum() - 1);
                            const float Wf = FMath::Clamp(P.Value, 0.f, 1.f);
                            const uint16 W16 = (uint16)FMath::Clamp((int32)FMath::RoundToInt(Wf * kScale), 0, 65535);

                            Weights.Emplace((uint16)BoneIdx, W16);
                        }

                        VertexSkinWeights.Set(VId, MakeArrayView(Weights));
                    }
                }
            }

            // IMPORTANT: Do NOT call CommitMeshDescription here.
            // Keeping MeshDescription stored enables editor tools,
            // while the render data (with correct skin weights) stays as built above.
        }
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
