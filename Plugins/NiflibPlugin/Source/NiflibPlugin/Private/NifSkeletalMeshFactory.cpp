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
#include <ImportUtils/SkeletalMeshImportUtils.h>

using namespace Niflib;

static TArray<SkeletalMeshImportData::FVertInfluence> GetVertexInfluences(const std::vector<NiSkinInstanceRef>& SkinInstances, const std::vector<NiGeometryDataRef>& GeometryDataRefs, const FReferenceSkeleton& ReferenceSkeleton, const FString& MeshName)
{
	TArray<SkeletalMeshImportData::FVertInfluence> VertexInfluences;
	TArray<SkeletalMeshImportData::FRawBoneInfluence> RawBoneInfluences;
	int32 Offset = 0;

	for (int32 i = 0; i < SkinInstances.size(); i++)
	{
		const std::vector<NiNodeRef>& Bones = SkinInstances[i]->GetBones();
		const NiSkinDataRef& SkinData = SkinInstances[i]->GetSkinData();

		for (int j = 0; j < Bones.size(); j++)
		{
			const std::vector<SkinWeight>& SkinWeights = SkinData->GetBoneWeights(j);
			for (SkinWeight SkinWeight : SkinWeights)
			{
				const FName BoneName = Bones[j]->GetName().c_str();
				SkeletalMeshImportData::FRawBoneInfluence RawBoneInfluence { SkinWeight.weight, (int32)SkinWeight.index + Offset, ReferenceSkeleton.FindBoneIndex(BoneName)};
				RawBoneInfluences.Add(RawBoneInfluence);
			}
		}

		Offset += GeometryDataRefs[i]->GetVertexCount();
	}

	// Reformat influences to Unreal's standards
	FSkeletalMeshImportData SkeletalMeshImportData;
	SkeletalMeshImportData.Influences = RawBoneInfluences;

	SkeletalMeshImportUtils::ProcessImportMeshInfluences(SkeletalMeshImportData, MeshName);

	for (SkeletalMeshImportData::FRawBoneInfluence RawBoneInfluence : SkeletalMeshImportData.Influences)
	{
		SkeletalMeshImportData::FVertInfluence VertexInfluence{ RawBoneInfluence.Weight, RawBoneInfluence.VertexIndex, RawBoneInfluence.BoneIndex };
		VertexInfluences.Add(VertexInfluence);
	}

	return VertexInfluences;
}

static TArray<SkeletalMeshImportData::FMeshWedge> GetMeshWedges(const std::vector<NiGeometryDataRef>& GeometryDataRefs)
{
	TArray<SkeletalMeshImportData::FMeshWedge> MeshWedges;
	int32 Offset = 0;

	for (const NiGeometryDataRef& GeometryData : GeometryDataRefs)
	{
		const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(GeometryData);
		const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
		const std::vector<Color4>& Colors = TriShapeData->GetColors();

		for (int TriangleIndex = 0; TriangleIndex < Triangles.size(); TriangleIndex++)
		{
			SkeletalMeshImportData::FMeshWedge MeshWedge[3] = {};
			MeshWedge[0].iVertex = (int32)Triangles[TriangleIndex].v1 + Offset;
			MeshWedge[1].iVertex = (int32)Triangles[TriangleIndex].v3 + Offset;	// Vertex v2 and v3 must be swapped for Unreal to render the mesh right side out
			MeshWedge[2].iVertex = (int32)Triangles[TriangleIndex].v2 + Offset;

			for (int i = 0; i < std::size(MeshWedge); i++)
			{
				for (int j = 0; j < TriShapeData->GetUVSetCount(); j++)
				{
					const std::vector<TexCoord>& UVSet = TriShapeData->GetUVSet(j);
					const FVector2f UV = { UVSet[MeshWedge[i].iVertex - Offset].u, UVSet[MeshWedge[i].iVertex - Offset].v };
					MeshWedge[i].UVs[j] = UV;
				}

				if (!Colors.empty())
				{
					const FColor Color(
						(uint8)(Colors[MeshWedge[i].iVertex - Offset].r * 255.0f),
						(uint8)(Colors[MeshWedge[i].iVertex - Offset].g * 255.0f),
						(uint8)(Colors[MeshWedge[i].iVertex - Offset].b * 255.0f),
						(uint8)(Colors[MeshWedge[i].iVertex - Offset].a * 255.0f)
					);
					MeshWedge[i].Color = Color;
				}

				MeshWedges.Add(MeshWedge[i]);
			}
		}

		Offset += GeometryData->GetVertexCount();
	}

	return MeshWedges;
}

static TArray<SkeletalMeshImportData::FMeshFace> GetMeshFaces(const TArray<SkeletalMeshImportData::FMeshWedge>& Wedges, const std::vector<NiGeometryDataRef>& GeometryDataRefs)
{
	TArray<SkeletalMeshImportData::FMeshFace> Faces;
	int32 Offset = 0;
	Faces.Reserve(Wedges.Num() / 3);

	for (int32 i = 0; i < GeometryDataRefs.size(); i++)
	{
		const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(GeometryDataRefs[i]);
		const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
		const std::vector<Vector3>& Tangents = TriShapeData->GetTangents();
		const std::vector<Vector3>& Bitangents = TriShapeData->GetBitangents();
		const std::vector<Vector3>& Normals = TriShapeData->GetNormals();

		for (int32 j = 0 + Offset; j < Triangles.size() + Offset; j++)
		{
			SkeletalMeshImportData::FMeshFace Face{};
			Face.MeshMaterialIndex = i;
			Face.SmoothingGroups = 1;

			Face.iWedge[0] = j * 3;
			Face.iWedge[1] = j * 3 + 1;
			Face.iWedge[2] = j * 3 + 2;

			Face.TangentX[0] = { Tangents[Triangles[j - Offset].v1].x, Tangents[Triangles[j - Offset].v1].y , Tangents[Triangles[j - Offset].v1].z };
			Face.TangentX[1] = { Tangents[Triangles[j - Offset].v3].x, Tangents[Triangles[j - Offset].v3].y , Tangents[Triangles[j - Offset].v3].z };
			Face.TangentX[2] = { Tangents[Triangles[j - Offset].v2].x, Tangents[Triangles[j - Offset].v2].y , Tangents[Triangles[j - Offset].v2].z };

			Face.TangentY[0] = { Bitangents[Triangles[j - Offset].v1].x, Bitangents[Triangles[j - Offset].v1].y , Bitangents[Triangles[j - Offset].v1].z };
			Face.TangentY[1] = { Bitangents[Triangles[j - Offset].v3].x, Bitangents[Triangles[j - Offset].v3].y , Bitangents[Triangles[j - Offset].v3].z };
			Face.TangentY[2] = { Bitangents[Triangles[j - Offset].v2].x, Bitangents[Triangles[j - Offset].v2].y , Bitangents[Triangles[j - Offset].v2].z };

			Face.TangentZ[0] = { Normals[Triangles[j - Offset].v1].x, Normals[Triangles[j - Offset].v1].y , Normals[Triangles[j - Offset].v1].z };
			Face.TangentZ[1] = { Normals[Triangles[j - Offset].v3].x, Normals[Triangles[j - Offset].v3].y , Normals[Triangles[j - Offset].v3].z };
			Face.TangentZ[2] = { Normals[Triangles[j - Offset].v2].x, Normals[Triangles[j - Offset].v2].y , Normals[Triangles[j - Offset].v2].z };

			Faces.Add(Face);
		}

		Offset += Triangles.size();
	}

	return Faces;
}

static TArray<FVector3f> GetPoints(const std::vector<NiGeometryDataRef>& GeometryDataRefs)
{
	TArray<FVector3f> Points;

	for (const NiGeometryDataRef& GeometryData : GeometryDataRefs)
	{
		for (const Vector3& Vertex : GeometryData->GetVertices())
		{
			const FVector3f Point = { Vertex.x, Vertex.y, Vertex.z };
			Points.Add(Point);
		}
	}

	return Points;
}

static TArray<int32> GetPointsToOriginalMap(const std::vector<NiGeometryDataRef>& GeometryDataRefs)
{
	TArray<int32> PointsToOriginalMap;

	for (const NiGeometryDataRef& GeometryData : GeometryDataRefs)
	{
		for (int i = 0; i < GeometryData->GetVertexCount(); i++)
		{
			PointsToOriginalMap.Add(i);
		}
	}

	return PointsToOriginalMap;
}

// TODO: No handling exists yet for NIFs without a skeleton and/or NiMultiTargetTransformController
// TODO: Skeleton seems inverted. For example, the bone leg_R is on the left side
static TArray<SkeletalMeshImportData::FBone> ParseNifSkeleton(const NiNodeRef& Bone, const int32 ParentIndex = -1, TArray<SkeletalMeshImportData::FBone> ReferenceBones = {})
{
	const Vector3 Location = Bone->GetLocalTranslation();
	const Quaternion Rotation = Bone->GetLocalRotation().AsQuaternion();
	const float Scale = Bone->GetLocalScale();
	const FName BoneName = Bone->GetName().c_str();

	SkeletalMeshImportData::FBone ReferenceBone;
	ReferenceBone.Name = Bone->GetName().c_str();
	ReferenceBone.NumChildren = Bone->GetChildren().size();
	ReferenceBone.ParentIndex = ParentIndex;
	ReferenceBone.BonePos.Transform = FTransform3f(
		FQuat4f(Rotation.x, Rotation.y, Rotation.z, Rotation.w),	// Rotation
		FVector3f(Location.x, Location.y, Location.z),	// Translation
		FVector3f(Scale)	// Scale
	);
	ReferenceBones.Add(ReferenceBone);

	const int32 CurrentBoneIndex = ReferenceBones.Num() - 1;
	for (const NiAVObjectRef& Child : Bone->GetChildren())
	{
		NiNodeRef NextBone = DynamicCast<NiNode>(Child);
		if (NextBone)
		{
			ReferenceBones = ParseNifSkeleton(NextBone, CurrentBoneIndex, ReferenceBones);
		}
	}

	return ReferenceBones;
}

static FReferenceSkeleton GetReferenceSkeleton(const NiNodeRef& RootBone, const USkeleton* Skeleton)
{
	FReferenceSkeleton ReferenceSkeleton;

	TArray<SkeletalMeshImportData::FBone> ReferenceBones = ParseNifSkeleton(RootBone);
	int32 BoneCount = ReferenceBones.Num();

	FSkeletalMeshImportData SkeletalMeshImportData;
	SkeletalMeshImportData.RefBonesBinary = ReferenceBones;
	SkeletalMeshImportUtils::ProcessImportMeshSkeleton(Skeleton, ReferenceSkeleton, BoneCount, SkeletalMeshImportData);

	return ReferenceSkeleton;
}

static std::vector<NiTriShapeRef> GetDescendantTriShapes(const NiNodeRef& Parent, std::vector<NiTriShapeRef> FoundTriShapes = {})
{
	for (const NiAVObjectRef& Child : Parent->GetChildren())
	{
		const NiTriShapeRef& TriShape = DynamicCast<NiTriShape>(Child);
		if (TriShape)
		{
			// TODO: Find a better way to determine if a TriShape should be thrown out
			FString TriShapeName = UTF8_TO_TCHAR(TriShape->GetName().c_str());
			TriShapeName = TriShapeName.ToLower();

			if (!TriShapeName.Contains(TEXT("shadow")))
			{
				FoundTriShapes.push_back(TriShape);
			}
		}
		else
		{
			const NiNodeRef& Node = DynamicCast<NiNode>(Child);
			if (Node)

			{
				FoundTriShapes = GetDescendantTriShapes(Node, FoundTriShapes);
			}
		}
	}

	return FoundTriShapes;
}

static void ParseNif(const FString& Filename, USkeletalMesh* SkeletalMesh, USkeleton* Skeleton)
{
	std::vector<NiObjectRef> NifList = ReadNifList(TCHAR_TO_UTF8(*Filename));
	IMeshUtilities& MeshUtilities = FModuleManager::LoadModuleChecked<IMeshUtilities>("MeshUtilities");
	NiMultiTargetTransformControllerRef MultiTargetTransformController;
	FReferenceSkeleton ReferenceSkeleton;

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
				ReferenceSkeleton = GetReferenceSkeleton(RootBone, Skeleton);
				SkeletalMesh->SetRefSkeleton(ReferenceSkeleton);

				break;	// There should only be one NiMultiTargetTransformController, I think, so bail once it is found.
			}
		}
	}

	NiLODNodeRef LODNode;
	std::vector<std::vector<NiTriShapeRef>> LODs;

	for (const NiObjectRef& Object : NifList)
	{
		LODNode = DynamicCast<NiLODNode>(Object);
		if (LODNode)
		{
			// All children of LODNode should be valid LODs (NiRangeLODData is a property, not a child)
			for (const NiAVObjectRef& Child : LODNode->GetChildren())
			{
				const NiNodeRef& LOD = DynamicCast<NiNode>(Child);
				if (LOD)
				{
					LODs.push_back(GetDescendantTriShapes(LOD));
				}
			}
			break;
		}
	}
	for (int i = 0; i < LODs.size(); i++)
	{
		std::vector<NiSkinInstanceRef> SkinInstances;
		std::vector<NiGeometryDataRef> GeometryDataRefs;

		for (NiTriShapeRef TriShape : LODs[i])
		{
			SkinInstances.push_back(TriShape->GetSkinInstance());
			GeometryDataRefs.push_back(TriShape->GetData());
		}

		const TArray<SkeletalMeshImportData::FVertInfluence>& VertexInfluences = GetVertexInfluences(SkinInstances, GeometryDataRefs, ReferenceSkeleton, SkeletalMesh->GetName());
		const TArray<SkeletalMeshImportData::FMeshWedge>& MeshWedges = GetMeshWedges(GeometryDataRefs);
		const TArray<SkeletalMeshImportData::FMeshFace>& MeshFaces = GetMeshFaces(MeshWedges, GeometryDataRefs);
		const TArray<FVector3f>& Points = GetPoints(GeometryDataRefs);
		const TArray<int32>& PointsToOriginalMap = GetPointsToOriginalMap(GeometryDataRefs);

		FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
		ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
		FSkeletalMeshLODModel* OutLODModel = &ImportedModel->LODModels[i];

		IMeshUtilities::MeshBuildOptions BuildOptions;
		BuildOptions.bComputeNormals = false;
		BuildOptions.bComputeTangents = false;
		BuildOptions.bUseMikkTSpace = false;

		MeshUtilities.BuildSkeletalMesh(
			*OutLODModel,
			SkeletalMesh->GetName(),
			ReferenceSkeleton,
			VertexInfluences,
			MeshWedges,
			MeshFaces,
			Points,
			PointsToOriginalMap,
			BuildOptions
		);

		SkeletalMesh->AddLODInfo();
		FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(i);
		LODInfo->BuildSettings.bRecomputeNormals = false;
		LODInfo->BuildSettings.bRecomputeTangents = false;
		LODInfo->BuildSettings.bUseMikkTSpace = false;

		// Ensure LOD reports at least one UV channel (NumTexCoords lives on LODModel in UE 5.4)
		OutLODModel->NumTexCoords = FMath::Max<uint32>(OutLODModel->NumTexCoords, 1u);

		// Materials slots	// TODO: Get more data from materials than them merely existing
		int32 MaxSectionMatIndex = -1;
		for (const FSkelMeshSection& Sec : OutLODModel->Sections)
			MaxSectionMatIndex = FMath::Max(MaxSectionMatIndex, (int32)Sec.MaterialIndex);

		if (MaxSectionMatIndex >= 0)
			while (SkeletalMesh->GetMaterials().Num() <= MaxSectionMatIndex)
				SkeletalMesh->GetMaterials().Add(FSkeletalMaterial());
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

	// Build skeleton
	UPackage* SkelPkg = MakeAssetPackage(BasePath, InName.ToString() + TEXT("_Skeleton"), SkelObjName);
	USkeleton* Skeleton = NewObject<USkeleton>(SkelPkg, *SkelObjName, RF_Public | RF_Standalone);
	SkeletalMesh->SetSkeleton(Skeleton);

	// Parse NIF for mesh data
	ParseNif(Filename, SkeletalMesh, Skeleton);

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
