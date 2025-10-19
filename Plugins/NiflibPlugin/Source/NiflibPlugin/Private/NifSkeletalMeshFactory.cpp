#include "NifSkeletalMeshFactory.h"

// Engine
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"
#include "EditorFramework/AssetImportData.h"
#include "UObject/Package.h"
#include "ReferenceSkeleton.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MeshUtilities.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"

// Niflib
#include <niflib.h>
#include <obj/NiLODNode.h>
#include <obj/NiTriShapeData.h>
#include <obj/NiSkinInstance.h>
#include <obj/NiSkinData.h>
#include <obj/NiMultiTargetTransformController.h>
#include <obj/NiMaterialProperty.h>

using namespace Niflib;

static TArray<SkeletalMeshImportData::FVertInfluence> GetVertexInfluences(const NiSkinInstanceRef& SkinInstance, const NiMultiTargetTransformControllerRef& MultiTargetTransformController)
{
	const std::vector<NiNodeRef>& Bones = SkinInstance->GetBones();
	const NiSkinDataRef& SkinData = SkinInstance->GetSkinData();
	const int TrueBoneCount = MultiTargetTransformController->GetExtraTargets().size() + 1;	//GetExtraTargets dose not cover the Target Bone, so increment by 1.
	const int InfluencelessBoneCount = TrueBoneCount - SkinInstance->GetBoneCount();
	TArray<SkeletalMeshImportData::FVertInfluence> VertexInfluences;

	// An NiSkinData only tracks the bones it influences, so we must offset the starting index by the difference between all bones and those influence by the skin.
	for (int BoneIndex = 0; BoneIndex < Bones.size(); BoneIndex++)
	{
		const std::vector<SkinWeight>& SkinWeights = SkinData->GetBoneWeights(BoneIndex);
		for (SkinWeight SkinWeight : SkinWeights)
		{
			SkeletalMeshImportData::FVertInfluence VertexInfluence{ SkinWeight.weight, SkinWeight.index, BoneIndex + InfluencelessBoneCount };
			VertexInfluences.Add(VertexInfluence);
		}
	}

	return VertexInfluences;
}

static TArray<SkeletalMeshImportData::FMeshWedge> GetMeshWedges(const NiGeometryDataRef& GeometryData)
{
	const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(GeometryData);
	const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
	const std::vector<Color4>& Colors = TriShapeData->GetColors();
	TArray<SkeletalMeshImportData::FMeshWedge> MeshWedges;

	for (int TriangleIndex = 0; TriangleIndex < Triangles.size(); TriangleIndex++)
	{
		SkeletalMeshImportData::FMeshWedge MeshWedge[3] = {};
		MeshWedge[0].iVertex = Triangles[TriangleIndex].v1;
		MeshWedge[1].iVertex = Triangles[TriangleIndex].v3;	// Vertex v2 and v3 must be swapped for Unreal to render the mesh right side out
		MeshWedge[2].iVertex = Triangles[TriangleIndex].v2;

		for (int i = 0; i < std::size(MeshWedge); i++)
		{
			for (int j = 0; j < TriShapeData->GetUVSetCount(); j++)
			{
				const std::vector<TexCoord>& UVSet = TriShapeData->GetUVSet(j);
				const FVector2f UV = { UVSet[MeshWedge[i].iVertex].u, UVSet[MeshWedge[i].iVertex].v };
				MeshWedge[i].UVs[j] = UV;
			}

			if (!Colors.empty())
			{
				const FColor Color(
					(uint8)(Colors[MeshWedge[i].iVertex].r * 255.0f),
					(uint8)(Colors[MeshWedge[i].iVertex].g * 255.0f),
					(uint8)(Colors[MeshWedge[i].iVertex].b * 255.0f),
					(uint8)(Colors[MeshWedge[i].iVertex].a * 255.0f)
				);
				MeshWedge[i].Color = Color;
			}

			MeshWedges.Add(MeshWedge[i]);
		}
	}

	return MeshWedges;
}

static TArray<SkeletalMeshImportData::FMeshFace> GetMeshFaces(const NiGeometryDataRef& GeometryData)
{
	const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(GeometryData);
	const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
	const std::vector<Vector3>& Tangents = TriShapeData->GetTangents();
	const std::vector<Vector3>& Bitangents = TriShapeData->GetBitangents();
	const std::vector<Vector3>& Normals = TriShapeData->GetNormals();
	TArray<SkeletalMeshImportData::FMeshFace> MeshFaces;

	for (Triangle Triangle : Triangles)
	{
		SkeletalMeshImportData::FMeshFace MeshFace = {};
		const uint32 VertexIndices[3] = { Triangle.v1, Triangle.v3, Triangle.v2 };	// Vertex v2 and v3 must be swapped for Unreal to render the mesh right side out

		MeshFace.MeshMaterialIndex = 0;
		MeshFace.SmoothingGroups = 1;

		for (int i = 0; i < std::size(VertexIndices); i++)
		{
			const FVector3f& Tangent = { Tangents[VertexIndices[i]].x, Tangents[VertexIndices[i]].y, Tangents[VertexIndices[i]].z };
			const FVector3f& Bitangent = { Bitangents[VertexIndices[i]].x, Bitangents[VertexIndices[i]].y, Bitangents[VertexIndices[i]].z };
			const FVector3f& Normal = { Normals[VertexIndices[i]].x, Normals[VertexIndices[i]].y, Normals[VertexIndices[i]].z };

			MeshFace.iWedge[i] = VertexIndices[i];
			MeshFace.TangentX[i] = Tangent;
			MeshFace.TangentY[i] = Bitangent;
			MeshFace.TangentZ[i] = Normal;
		}

		MeshFaces.Add(MeshFace);
	}
	/*const NiPropertyRef& Property = TriShape->GetPropertyByType(NiMaterialProperty::TYPE);
	const NiMaterialPropertyRef& MaterialProperty = DynamicCast<NiMaterialProperty>(Property);
	MaterialProperty->get*/	//TODO: Materials

	return MeshFaces;
}

static TArray<SkeletalMeshImportData::FMeshFace> BuildFacesFromWedges(const TArray<SkeletalMeshImportData::FMeshWedge>& Wedges)
{
	TArray<SkeletalMeshImportData::FMeshFace> Faces;
	Faces.Reserve(Wedges.Num() / 3);

	for (int32 Base = 0; Base + 2 < Wedges.Num(); Base += 3)
	{
		SkeletalMeshImportData::FMeshFace Face{};
		Face.MeshMaterialIndex = 0;
		Face.SmoothingGroups = 1;

		Face.iWedge[0] = static_cast<uint32>(Base + 0);
		Face.iWedge[1] = static_cast<uint32>(Base + 1);
		Face.iWedge[2] = static_cast<uint32>(Base + 2);

		// Leave tangents zero; let the builder recompute if needed
		Faces.Add(Face);
	}
	return Faces;
}

static TArray<FVector3f> GetPoints(const NiGeometryDataRef& GeometryData)
{
	TArray<FVector3f> Points;

	for (const Vector3& Vertex : GeometryData->GetVertices())
	{
		FVector3f Point = { Vertex.x, Vertex.y, Vertex.z };
		Points.Add(Point);
	}

	return Points;
}

static TArray<int32> GetPointsToOriginalMap(const NiGeometryDataRef& GeometryData)
{
	TArray<int32> PointsToOriginalMap;
	const std::vector<Vector3>& Vertices = GeometryData->GetVertices();

	for (int i = 0; i < Vertices.size(); i++)
	{
		PointsToOriginalMap.Add(i);
	}

	return PointsToOriginalMap;
}

struct FNifReferenceSkeleton
{
	TArray<FName> BoneNames;
	TArray<int32> ParentIndices;
	TArray<FTransform> RefPose;
};

// TODO: Skeleton seems inverted. For example, the bone leg_R is on the left side
static FNifReferenceSkeleton ParseNifSkeleton(const NiNodeRef& Bone, const int32 ParentIndex = -1, FNifReferenceSkeleton Skeleton = {})
{
	const Vector3 Location = Bone->GetLocalTranslation();
	Quaternion Rotation = Bone->GetLocalRotation().AsQuaternion();
	const float Scale = Bone->GetLocalScale();

	FTransform Transform;
	Transform.SetLocation(FVector(Location.x, Location.y, Location.z));
	Transform.SetRotation(FQuat(Rotation.x, Rotation.y, Rotation.z, Rotation.w));
	Transform.SetScale3D(FVector(Scale));

	FName BoneName = Bone->GetName().c_str();
	Skeleton.BoneNames.Add(BoneName);
	Skeleton.ParentIndices.Add(ParentIndex);
	Skeleton.RefPose.Add(Transform);

	const int32 CurrentBoneIndex = Skeleton.BoneNames.Num() - 1;
	for (const NiAVObjectRef& Child : Bone->GetChildren())
	{
		NiNodeRef NextBone = DynamicCast<NiNode>(Child);
		if (NextBone)
		{
			Skeleton = ParseNifSkeleton(NextBone, CurrentBoneIndex, Skeleton);
		}
	}

	return Skeleton;
}

static FReferenceSkeleton BuildReferenceSkeleton(
	const TArray<FName>& BoneNames,
	const TArray<int32>& ParentIndices,
	const TArray<FTransform>& RefPose)
{
	FReferenceSkeleton RefSkeleton(true);
	FReferenceSkeletonModifier Mod(RefSkeleton, nullptr);

	const int32 NumBones = BoneNames.Num();
	for (int32 i = 0; i < NumBones; ++i)
	{
		FMeshBoneInfo BoneInfo(BoneNames[i], FString(), ParentIndices[i]);
		Mod.Add(BoneInfo, RefPose[i], false);
	}

	return RefSkeleton;
}

static void ParseNif(const FString& Filename, USkeletalMesh* SkeletalMesh)
{
	FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
	ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
	FSkeletalMeshLODModel* NewLODModel = &ImportedModel->LODModels[0];

	std::vector<NiObjectRef> NifList = ReadNifList(TCHAR_TO_UTF8(*Filename));
	IMeshUtilities& MeshUtilities = FModuleManager::LoadModuleChecked<IMeshUtilities>("MeshUtilities");
	NiMultiTargetTransformControllerRef MultiTargetTransformController;
	FNifReferenceSkeleton NifSkeleton;

	for (const NiObjectRef& Object : NifList)
	{
		// NiSkinInstanceRef->GetSkeletalRoot() and NiSkinInstanceRef->GetBones()[0] do not reliably return the true skeletal root
		MultiTargetTransformController = DynamicCast<NiMultiTargetTransformController>(Object);
		if (MultiTargetTransformController)
		{
			// The target of NiMultiTargetTransformController should always be the true skeletal root, or else animations wouldn't work, I think
			const NiObjectNETRef& Target = MultiTargetTransformController->GetTarget();
			const NiNodeRef& RootBone = DynamicCast<NiNode>(Target);
			if (RootBone)
			{
				NifSkeleton = ParseNifSkeleton(RootBone);

				break;	// There should only be one NiMultiTargetTransformController, I think, so bail once it is found.
			}
		}
	}

	for (const NiObjectRef& Object : NifList)
	{
		const NiTriShapeRef TriShape = DynamicCast<NiTriShape>(Object);
		if (TriShape)
		{
			const FReferenceSkeleton& ReferenceSkeleton = BuildReferenceSkeleton(NifSkeleton.BoneNames, NifSkeleton.ParentIndices, NifSkeleton.RefPose);
			SkeletalMesh->SetRefSkeleton(ReferenceSkeleton);

			const TArray<SkeletalMeshImportData::FVertInfluence>& VertexInfluences = GetVertexInfluences(TriShape->GetSkinInstance(), MultiTargetTransformController);
			const TArray<SkeletalMeshImportData::FMeshWedge>& MeshWedges = GetMeshWedges(TriShape->GetData());
			const TArray<SkeletalMeshImportData::FMeshFace>& MeshFaces = BuildFacesFromWedges(MeshWedges);
			const TArray<FVector3f>& Points = GetPoints(TriShape->GetData());
			const TArray<int32>& PointsToOriginalMap = GetPointsToOriginalMap(TriShape->GetData());
			IMeshUtilities::MeshBuildOptions BuildOptions;

			// TODO: Calculate my own UVs
			BuildOptions.bComputeNormals = true;
			BuildOptions.bComputeTangents = true;
			BuildOptions.bUseMikkTSpace = true;

			MeshUtilities.BuildSkeletalMesh(
				*NewLODModel,
				SkeletalMesh->GetName(),
				ReferenceSkeleton,
				VertexInfluences,
				MeshWedges,
				MeshFaces,
				Points,
				PointsToOriginalMap,
				BuildOptions
			);

			/////
			SkeletalMesh->AddLODInfo();
			FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(0);
			// TODO: Calculate my own UVs
			LODInfo->BuildSettings.bRecomputeNormals = true;
			LODInfo->BuildSettings.bRecomputeTangents = true;
			LODInfo->BuildSettings.bUseMikkTSpace = true;

			//// Ensure LOD reports at least one UV channel (NumTexCoords lives on LODModel in UE 5.4)	// TODO: UVs
			NewLODModel->NumTexCoords = FMath::Max<uint32>(NewLODModel->NumTexCoords, 1u);

			//// Materials slots (minimum)	// TODO: Materials
			int32 MaxSectionMatIndex = -1;
			for (const FSkelMeshSection& Sec : NewLODModel->Sections)
				MaxSectionMatIndex = FMath::Max(MaxSectionMatIndex, (int32)Sec.MaterialIndex);

			if (MaxSectionMatIndex >= 0)
				while (SkeletalMesh->GetMaterials().Num() <= MaxSectionMatIndex)
					SkeletalMesh->GetMaterials().Add(FSkeletalMaterial());

			//// Bounds (from this LOD’s points)
			{
				FBox BoundsBox(ForceInit);
				for (const FVector3f& P : Points)
					BoundsBox += (FVector)P;
				if (BoundsBox.IsValid)
					SkeletalMesh->SetImportedBounds(FBoxSphereBounds(BoundsBox));
			}
			///////

			break; // TODO: Remove once I can handle multiple LODs, This will only get one TriShape of the LOD, not the entirety of the TriShapes in the LOD
		}
	}
}

// Create a unique package under the same folder as the selected destination
static UPackage* MakeAssetPackage(const FString& BasePath, const FString& AssetName, FString& OutObjectName)
{
	FString PackageName;
	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	AssetTools.Get().CreateUniqueAssetName(BasePath / AssetName, TEXT(""), PackageName, OutObjectName);
	return CreatePackage(*PackageName);
}

UNifSkeletalMeshFactory::UNifSkeletalMeshFactory()
{
	SupportedClass = USkeletalMesh::StaticClass();
	Formats.Add(TEXT("nif;NIF File"));

	bCreateNew = false;
	bEditorImport = true;
	bText = false;
	bEditAfterNew = false;
	ImportPriority = 100;
}

bool UNifSkeletalMeshFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetExtension(Filename).Equals(TEXT("nif"), ESearchCase::IgnoreCase);
}

UObject* UNifSkeletalMeshFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled
)
{
	bOutOperationCanceled = false;

	// Create packages/assets
	const FString BasePath = InParent->GetOutermost()->GetName();
	FString SkelObjName, MeshObjName;

	// Create mesh
	UPackage* MeshPkg = MakeAssetPackage(BasePath, InName.ToString(), MeshObjName);
	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(MeshPkg, *MeshObjName, RF_Public | RF_Standalone);

	// Parse NIF for mesh data
	ParseNif(Filename, SkeletalMesh);

	// Build skeleton
	UPackage* SkelPkg = MakeAssetPackage(BasePath, InName.ToString() + TEXT("_Skeleton"), SkelObjName);
	USkeleton* Skeleton = NewObject<USkeleton>(SkelPkg, *SkelObjName, RF_Public | RF_Standalone);
	SkeletalMesh->SetSkeleton(Skeleton);

	// Finalize
	SkeletalMesh->InvalidateDeriveDataCacheGUID();
	Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
	SkeletalMesh->CalculateInvRefMatrices();

	// Register
	FAssetRegistryModule::AssetCreated(Skeleton);
	FAssetRegistryModule::AssetCreated(SkeletalMesh);
	SkelPkg->MarkPackageDirty();
	MeshPkg->MarkPackageDirty();

	SkeletalMesh->PostEditChange();
	Skeleton->PostEditChange();

	// Force asset reload to auto-generate missing MeshDescription (I think)
	UE_LOG(LogTemp, Log, TEXT("[NIF] Imported SkeletalMesh %s  (LODs: %d)"), *MeshObjName, SkeletalMesh->GetImportedModel()->LODModels.Num());	// Prevents crash from calling PostLoad, for some reason.
	SkeletalMesh->PostLoad();	// Prevents crash from trying to edit bone weights without saving asset and reloading editor first.

	return SkeletalMesh;
}
