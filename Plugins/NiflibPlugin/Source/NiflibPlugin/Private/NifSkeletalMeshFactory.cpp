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
#include <Rendering/SkeletalMeshLODImporterData.h>

// Niflib
#include <niflib.h>
#include <obj/NiLODNode.h>
#include <obj/NiTriShapeData.h>
#include <obj/NiSkinInstance.h>
#include <obj/NiSkinData.h>
#include <obj/NiMultiTargetTransformController.h>
#include <obj/NiMaterialProperty.h>

using namespace Niflib;

static TArray<SkeletalMeshImportData::FRawBoneInfluence> GetRawBoneInfluences(const NiSkinInstanceRef& SkinInstance)
{
	const std::vector<NiNodeRef>& Bones = SkinInstance->GetBones();
	const NiSkinDataRef& SkinData = SkinInstance->GetSkinData();
	TArray<SkeletalMeshImportData::FRawBoneInfluence> RawBoneInfluences;

	for (int BoneIndex = 0; BoneIndex < Bones.size(); BoneIndex++)	// Bone indices will be off by 2 from the reference skeleton, since its only counting children of the skin. Adjust.
	{
		const std::vector<SkinWeight>& SkinWeights = SkinData->GetBoneWeights(BoneIndex);
		for (SkinWeight SkinWeight : SkinWeights)
		{
			SkeletalMeshImportData::FRawBoneInfluence RawBoneInfluence{ SkinWeight.index, BoneIndex, SkinWeight.weight };
			RawBoneInfluences.Add(RawBoneInfluence);
		}
	}

	return RawBoneInfluences;
}

static TArray<SkeletalMeshImportData::FMeshWedge> GetMeshWedges(const NiGeometryDataRef& GeometryData)
{
	const std::vector<Vector3>& Vertices = GeometryData->GetVertices();
	const std::vector<Color4>& Colors = GeometryData->GetColors();
	TArray<SkeletalMeshImportData::FMeshWedge> MeshWedges;

	for (int VertexIndex = 0; VertexIndex < Vertices.size(); VertexIndex++)
	{
		SkeletalMeshImportData::FMeshWedge MeshWedge = {};
		MeshWedge.iVertex = VertexIndex;

		for (int i = 0; i < GeometryData->GetUVSetCount(); i++)
		{
			const std::vector<TexCoord>& UVSet = GeometryData->GetUVSet(i);
			const FVector2f UV = { UVSet[VertexIndex].u, UVSet[VertexIndex].v};
			MeshWedge.UVs[i] = UV;
		}

		if (!Colors.empty())
		{
			FColor Color = { Colors[VertexIndex].b, Colors[VertexIndex].g, Colors[VertexIndex].r, Colors[VertexIndex].a };
			MeshWedge.Color = Color;
		}

		MeshWedges.Add(MeshWedge);
	}

	return MeshWedges;
}

static TArray<SkeletalMeshImportData::FMeshFace> GetMeshFaces(const NiTriShapeRef& TriShape)
{
	const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(TriShape->GetData());
	const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
	const std::vector<Vector3>& Tangents = TriShapeData->GetTangents();
	const std::vector<Vector3>& Bitangents = TriShapeData->GetBitangents();
	const std::vector<Vector3>& Normals = TriShapeData->GetNormals();
	TArray<SkeletalMeshImportData::FMeshFace> MeshFaces;

	for (Triangle Triangle : Triangles)
	{
		SkeletalMeshImportData::FMeshFace MeshFace = {};
		const uint32 VertexIndices[3] = { Triangle.v1, Triangle.v2, Triangle.v3 };

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
	const NiPropertyRef& Property = TriShape->GetPropertyByType(NiMaterialProperty::TYPE);
	const auto& MaterialProperty = DynamicCast<NiMaterialProperty>(Property);

	return MeshFaces;
}

static SkeletalMeshImportData::FMeshFace GetMeshFace()
{
	SkeletalMeshImportData::FMeshFace MeshFace;
	// uint32 iWedge[3];    // The 3 wedge indices making up this face
	// uint16 MeshMaterialIndex; // Which material slot this face belongs to
	// uint32 SmoothingGroups;   // Smoothing group for shading
	// FVector3f TangentX[3];    // Per-corner tangent (optional at import)
	// FVector3f TangentY[3];    // Per-corner bitangent
	// FVector3f TangentZ[3];    // Per-corner normal

	return MeshFace;
}

static void Test(const FString& Filename)
{
	/*TriShape->GetSkinInstance()->GetSkinData()->GetBoneWeights(0)[0].
	DynamicCast<NiTriShapeData>(TriShape->GetData())->GetTriangles()[0].
	TriShape->GetData()->get
	TriShape->GetSkinInstance()->GetSkinData()->GetBoneWeights(0)[0].
	TriShape->GetSkinInstance()->GetBones()*/

	std::vector<NiObjectRef> NifList = ReadNifList(TCHAR_TO_UTF8(*Filename));

	for (const NiObjectRef& Object : NifList)
	{
		const NiTriShapeRef& TriShape = DynamicCast<NiTriShape>(Object);
		if (TriShape)
		{
			const std::vector<NiNodeRef>& Bones = TriShape->GetSkinInstance()->GetBones();
			const NiSkinDataRef& SkinData = TriShape->GetSkinInstance()->GetSkinData();
			for (int i = 0; i < Bones.size(); i++)
			{
				UE_LOG(LogTemp, Log, TEXT("[NIF] %d"), i);
				const std::vector<SkinWeight>& SkinWeights = SkinData->GetBoneWeights(i);
				for (SkinWeight SkinWeight : SkinWeights)
				{
					UE_LOG(LogTemp, Log, TEXT("	[NIF] %hu: %.2f"), SkinWeight.index, SkinWeight.weight);
				}
			}
			break;

			TArray<int32> BoneIndices;
			BoneIndices.SetNum(TriShape->GetData()->GetVertexCount());
			TArray<float> Weights;
			Weights.SetNum(TriShape->GetData()->GetVertexCount());
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

// Given tangent, bitangent, and normal from NIF data, produce an Unreal-compatible tangent
static FVector4f MakeUnrealTangent(const FVector3f& Tangent, const FVector3f& Bitangent, const FVector3f& Normal)
{
	// Ensure all are normalized
	FVector3f T = Tangent.GetSafeNormal();
	FVector3f B = Bitangent.GetSafeNormal();
	FVector3f N = Normal.GetSafeNormal();

	// Calculate handedness: +1 if B matches cross(N,T), -1 if opposite
	float Handedness = (FVector3f::DotProduct(FVector3f::CrossProduct(N, T), B) < 0.0f) ? -1.0f : 1.0f;

	// Unreal packs XYZ in Tangent, and W as sign of bitangent
	return FVector4f(T.X, T.Y, T.Z, Handedness);
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

	// Parse NIF for mesh data
	UNifSkeletalMeshFactory::FNifReferenceSkeleton NifReferenceSkeleton = ParseNif(Filename);
	Test(Filename);

	// Create packages/assets
	const FString BasePath = InParent->GetOutermost()->GetName();
	FString SkelObjName, MeshObjName;

	// Create mesh
	UPackage* MeshPkg = MakeAssetPackage(BasePath, InName.ToString(), MeshObjName);
	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(MeshPkg, *MeshObjName, RF_Public | RF_Standalone);

	// Build reference skeleton
	FReferenceSkeleton ReferenceSkeleton = BuildReferenceSkeleton(NifReferenceSkeleton.BoneNames, NifReferenceSkeleton.ParentIndices, NifReferenceSkeleton.RefPose);
	SkeletalMesh->SetRefSkeleton(ReferenceSkeleton);

	// Build skeleton
	UPackage* SkelPkg = MakeAssetPackage(BasePath, InName.ToString() + TEXT("_Skeleton"), SkelObjName);
	USkeleton* Skeleton = NewObject<USkeleton>(SkelPkg, *SkelObjName, RF_Public | RF_Standalone);
	SkeletalMesh->SetSkeleton(Skeleton);
	Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
	Skeleton->SetPreviewMesh(SkeletalMesh);

	// Compute inverse bind data
	SkeletalMesh->CalculateInvRefMatrices();
	SkeletalMesh->InvalidateDeriveDataCacheGUID();

	// Register assets
	FAssetRegistryModule::AssetCreated(Skeleton);
	FAssetRegistryModule::AssetCreated(SkeletalMesh);

	// Notify editor
	SkelPkg->MarkPackageDirty();
	MeshPkg->MarkPackageDirty();
	Skeleton->PostEditChange();
	SkeletalMesh->PostEditChange();

	// Force reload to auto-generate missing MeshDescription
	// SkeletalMesh->PostLoad();

	return SkeletalMesh;
}

UNifSkeletalMeshFactory::FNifReferenceSkeleton UNifSkeletalMeshFactory::ParseNif(const FString& Filename)
{
	std::vector<NiObjectRef> NifList = ReadNifList(TCHAR_TO_UTF8(*Filename));
	UNifSkeletalMeshFactory::FNifReferenceSkeleton Skeleton;

	for (const NiObjectRef& Object : NifList)
	{
		// NiSkinInstanceRef->GetSkeletalRoot() and NiSkinInstanceRef->GetBones()[0] do not reliably return the true skeletal root
		const NiMultiTargetTransformControllerRef& TransformController = DynamicCast<NiMultiTargetTransformController>(Object);
		if (TransformController)
		{
			// The target of NiMultiTargetTransformController should always be the true skeletal root, or else animations wouldn't work, I think
			const NiObjectNETRef& TransformControllerTarget = TransformController->GetTarget();
			const NiNodeRef& RootBone = DynamicCast<NiNode>(TransformControllerTarget);
			if (RootBone)
			{
				Skeleton = ParseNifSkeleton(RootBone);
				break;	// There should only be one NiMultiTargetTransformController, I think, so bail once it is found
			}
		}
	}

	/*for (int i = 0; i < Skeleton.BoneNames.Num(); i++)
	{
		UE_LOG(LogTemp, Log, TEXT("[NIF] Name: %s"), *Skeleton.BoneNames[i].ToString());
		UE_LOG(LogTemp, Log, TEXT("[NIF] Index: %d"), i);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Parent: %d"), Skeleton.ParentIndices[i]);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Transform - Location: %s, Rotation: %s, Scale: %s"),
			*Skeleton.RefPose[i].GetLocation().ToString(),
			*Skeleton.RefPose[i].GetRotation().Rotator().ToString(),
			*Skeleton.RefPose[i].GetScale3D().ToString());
	}*/

	//std::map<NiNodeRef, std::vector<NiTriShapeRef>> TriShapesByLOD;
	//for (const NiObjectRef& Object : NifList)
	//{
	//	const NiTriShapeRef& TriShape = DynamicCast<NiTriShape>(Object);
	//	if (TriShape)
	//	{
	//		const NiNodeRef& LOD = FindFirstAncestorThatIsALOD(TriShape->GetParent());	// NiLODNodeRef->GetChildre() is numerically indexed, so perhaps this is how LOD order is determined
	//		if (LOD)	// The shadow NiTriShape in dyn_patron is not under a LOD, but the ones in Horse are, meaning the former will return null
	//		{			// It is unclear how to define a shadow. Perhaps NiTriShape lacking a NiTexturingProperty could work, although that may be too broad. NiStencilProperty? NiZBufferProperty?
	//			TriShapesByLOD[LOD].push_back(TriShape);
	//			// TriShape->GetData()->GetNormals();	//GetData seems to give us most of the data we need for the first step
	//		}
	//	}
	//}

	std::map <NiNodeRef, std::vector<NiTriShapeRef>> Map;
	std::vector<std::vector<NiTriShapeRef>> qwerty;	// The LODs themselves don't hold much info but their names and childre, so we could ditch their pointer and just identify by index
	for (const NiObjectRef& Object : NifList)
	{
		const NiLODNodeRef& LODNode = DynamicCast<NiLODNode>(Object);
		if (LODNode)
		{
			// This should only return valid LODs. NiRangeLODData is a property, not a child, and any other NiNode would erroneously be considered a LOD by NiRangeLODData if it was a child of NiLODNode.
			for (const NiAVObjectRef& Child : LODNode->GetChildren())
			{
				const NiNodeRef& LOD = DynamicCast<NiNode>(Child);
				if (LOD)
				{
					GetDescendantTriShapes(LOD, Map[LOD]);
				}
			}
			break;	// There should only be one NiLODNode per NIF
		}
	}

	/*for (const auto& Pair : Map)
	{
		FString s = UTF8_TO_TCHAR(Pair.first->GetIDString().c_str());
		UE_LOG(LogTemp, Log, TEXT("[NIF] IDString: %s"), *s);

		for (const auto& Child : Pair.second)
		{
			FString t = UTF8_TO_TCHAR(Child->GetIDString().c_str());
			UE_LOG(LogTemp, Log, TEXT("	[NIF] IDString: %s"), *t);
		}
	}*/

	return Skeleton;
}

UNifSkeletalMeshFactory::FNifReferenceSkeleton UNifSkeletalMeshFactory::ParseNifSkeleton(const NiNodeRef& Bone, const int32 PreviousIndex, const int32 ParentIndex, UNifSkeletalMeshFactory::FNifReferenceSkeleton Skeleton)
{
	const Vector3 Location = Bone->GetLocalTranslation();
	const Quaternion Rotation = Bone->GetLocalRotation().AsQuaternion();
	const float Scale = Bone->GetLocalScale();

	FTransform transform;
	transform.SetLocation(FVector(Location.x, Location.y, Location.z));
	transform.SetRotation(FQuat(Rotation.x, Rotation.y, Rotation.z, Rotation.w));
	transform.SetScale3D(FVector(Scale));

	FName boneName = Bone->GetName().c_str();
	Skeleton.BoneNames.Add(boneName);
	Skeleton.ParentIndices.Add(ParentIndex);
	Skeleton.RefPose.Add(transform);

	for (const NiAVObjectRef& Child : Bone->GetChildren())
	{
		NiNodeRef NextBone = DynamicCast<NiNode>(Child);
		if (NextBone)
		{
			Skeleton = ParseNifSkeleton(NextBone, PreviousIndex + 1, ParentIndex + 1, Skeleton);
		}
	}

	return Skeleton;
}

UNifSkeletalMeshFactory::FNifLODGeometry UNifSkeletalMeshFactory::ParseNifLODGeometry(const vector<NiTriShapeRef>& LODTriShapes)
{
	UNifSkeletalMeshFactory::FNifLODGeometry LODGeometry;

	for (const NiTriShapeRef& TriShape : LODTriShapes)
	{
		const NiGeometryDataRef& GeometryData = TriShape->GetData();
		if (GeometryData)
		{
			const std::vector<Vector3>& Vertices = GeometryData->GetVertices();
			const std::vector<Vector3>& Tangents = GeometryData->GetTangents();
			const std::vector<Vector3>& Bitangents = GeometryData->GetBitangents();
			const std::vector<Vector3>& Normals = GeometryData->GetNormals();
			const std::vector<TexCoord>& UVSet = GeometryData->GetUVSet(0);	// Need to null handle index, and figure out how to derive it programmatically

			for (int i = 0; i < Vertices.size(); i++)
			{
				FVector3f Position = { Vertices[i].x, Vertices[i].y, Vertices[i].z };
				FVector2f UV = { UVSet[i].u, UVSet[i].v};
				FVector3f Tangent = { Tangents[i].x, Tangents[i].y, Tangents[i].z };
				FVector3f Bitangent = { Bitangents[i].x, Bitangents[i].y, Bitangents[i].z };
				FVector3f Normal = { Normals[i].x, Normals[i].y, Normals[i].z };
				FVector4f UnrealTangent = MakeUnrealTangent(Tangent, Bitangent, Normal);

				LODGeometry.Positions.Add(Position);
				LODGeometry.Normals.Add(Normal);
				LODGeometry.Tangents.Add(UnrealTangent);

				// Handle vertex colors (NifShapeData has a property for if vertex colors are used or not
			}

			for (const int& Index : GeometryData->GetVertexIndices())
			{
				LODGeometry.Indices.Add(Index);
			}
		}
	}

	return LODGeometry;
}

std::vector<NiTriShapeRef> UNifSkeletalMeshFactory::GetDescendantTriShapes(const NiNodeRef& Parent, std::vector<NiTriShapeRef>& FoundTriShapes)
{
	for (const NiAVObjectRef& Child : Parent->GetChildren())
	{
		const NiTriShapeRef& TriShape = DynamicCast<NiTriShape>(Child);
		if (TriShape)
		{
			FoundTriShapes.push_back(TriShape);
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

NiNodeRef UNifSkeletalMeshFactory::FindFirstAncestorThatIsALOD(const NiNodeRef& Node)
{
	if (!Node) { return NULL; }
	NiNodeRef Parent = Node->GetParent();

	if (!Parent) { return NULL; }	// There are no more valid ancestors, so there must be no LOD
	if (DynamicCast<NiLODNode>(Parent)) { return Node; }	// This NiNode's parent is an NiLODNode, therefore it must be a LOD

	return FindFirstAncestorThatIsALOD(Parent);	// Recurse up the Nif tree
}

FReferenceSkeleton UNifSkeletalMeshFactory::BuildReferenceSkeleton(
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
