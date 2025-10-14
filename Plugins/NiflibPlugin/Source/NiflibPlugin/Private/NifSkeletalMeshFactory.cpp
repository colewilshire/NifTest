#include "NifSkeletalMeshFactory.h"

// Engine
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"
#include "EditorFramework/AssetImportData.h"
#include "UObject/Package.h"
#include "ReferenceSkeleton.h"

// Niflib
#include <niflib.h>
#include <obj/NiLODNode.h>
#include <obj/NiSkinInstance.h>

using namespace Niflib;

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

	UE_LOG(LogTemp, Log, TEXT("[NIF] Importing skeletal mesh from: %s"), *Filename);

	GetNiTriShapes(Filename);

	// Create the skeletal mesh asset shell
	USkeletalMesh* NewMesh = NewObject<USkeletalMesh>(InParent, InClass, InName, Flags | RF_Public | RF_Standalone);
	if (!NewMesh)
	{
		bOutOperationCanceled = true;
		return nullptr;
	}

	NewMesh->MarkPackageDirty();
	NewMesh->PostEditChange();

	return NewMesh;
}

// Find NiTriShape, a child of a LOD
// Its child NiTriShapeData contains the mesh triangles, while its other child NiSkinInstance contains the bones

void UNifSkeletalMeshFactory::GetNiTriShapes(const FString& Filename)
{
	std::vector<NiObjectRef> nifList = ReadNifList(TCHAR_TO_UTF8(*Filename));
	std::vector<NiTriShapeRef> niTriShapeList;

	// Get all NiTriShapes in the Nif tree
	for (NiObjectRef& niObjectRef : nifList)
	{
		NiTriShapeRef niTriShapeRef = DynamicCast<NiTriShape>(niObjectRef);
		if (niTriShapeRef)
		{
			niTriShapeList.push_back(niTriShapeRef);
		}
	}

	// Search ancestors for LODs
	std::unordered_map<NiNode*, std::vector<NiTriShapeRef>> lods;
	for (NiTriShapeRef& niTriShapeRef : niTriShapeList)
	{
		NiNodeRef ancestorLOD = FindFirstAncestorThatIsALod(niTriShapeRef->GetParent());

		if (ancestorLOD)
		{
			lods[ancestorLOD].push_back(niTriShapeRef);
		}
	}

	// If no LODs were found, we can assume all NiTriShapes are part of LOD0
	if (lods.empty())	//TODO: Handle no LODs
	{

	}

	// NiTriShapeData
	for (std::pair<NiNodeRef, std::vector<NiTriShapeRef>> pair : lods)
	{
		for (NiTriShapeRef& niTriShapeRef : pair.second)
		{
			GetNifSkeleton(niTriShapeRef);
			break;	//TODO: Handle other LODs
		}
		break;	//TODO: Handle Other LODs
	}
}

NiNodeRef UNifSkeletalMeshFactory::FindFirstAncestorThatIsALod(const NiNodeRef& niNodeRef)
{
	if (!niNodeRef) { return NULL; }
	NiNodeRef parent = niNodeRef->GetParent();

	if (!parent) { return NULL; }	// There are no more valid ancestors, so there must be no LOD
	if (DynamicCast<NiLODNode>(parent)) { return niNodeRef; }	// This NiNode's parent is an NiLODNode, therefore it must be a LOD

	return FindFirstAncestorThatIsALod(parent);	// Recurse up the Nif tree
}

void UNifSkeletalMeshFactory::GetNifSkeleton(const NiTriShapeRef& niTriShapeRef)
{
	NiSkinInstanceRef niSkinInstanceRef = niTriShapeRef->GetSkinInstance();
	std::vector<NiNodeRef> bones = niSkinInstanceRef->GetBones();	// GetSkeletalRoot, then iterating through children may be the way to go, as we are missing the thing called "root"
	TArray<FName> boneNames;										// We could theoretically just use that to get the real root, but I believe there is a NiNode between the node called Root, and "hip" (the real root?)
	TArray<int32> parentIndices;									// I also doubt static meshes like Bull even have an NiSkinInstance
	TArray<FTransform> refPose;
	TMap<FName, int32> parentLookupTable;

	for (int i = 0; i < bones.size(); i++)
	{
		Vector3 location = bones[i]->GetLocalTranslation();
		Quaternion rotation = bones[i]->GetLocalRotation().AsQuaternion();
		float scale = bones[i]->GetLocalScale();

		FTransform transform;
		transform.SetLocation(FVector(location.x, location.y, location.z));
		transform.SetRotation(FQuat(rotation.x, rotation.y, rotation.z, rotation.w));
		transform.SetScale3D(FVector(scale));

		FName boneName = bones[i]->GetName().c_str();
		boneNames.Add(boneName);
		refPose.Add(transform);
		parentLookupTable.Add(boneName, i);

		if (i != 0)
		{
			NiNodeRef parent = bones[i]->GetParent();
			FName parentName = parent->GetName().c_str();
			int32 parentIndex = *parentLookupTable.Find(parentName);
			parentIndices.Add(parentIndex);
		}
		else
		{
			parentIndices.Add(-1);
		}
	}

	for (int i = 0; i < boneNames.Num(); i++)
	{
		UE_LOG(LogTemp, Log, TEXT("[NIF] Name: %s"), *boneNames[i].ToString());
		UE_LOG(LogTemp, Log, TEXT("[NIF] Index: %d"), i);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Parent: %d"), parentIndices[i]);
		UE_LOG(LogTemp, Log, TEXT("Transform - Location: %s, Rotation: %s, Scale: %s"),
			*refPose[i].GetLocation().ToString(),
			*refPose[i].GetRotation().Rotator().ToString(),
			*refPose[i].GetScale3D().ToString());
	}

	//BuildReferenceSkeleton(boneNames, parentIndices, refPose);
}

FReferenceSkeleton BuildReferenceSkeleton(
	const TArray<FName>& BoneNames,
	const TArray<int32>& ParentIndices,
	const TArray<FTransform>& RefPose)
{
	FReferenceSkeleton RefSkeleton(/*bUseRawData=*/true);
	FReferenceSkeletonModifier Mod(RefSkeleton, /*USkeleton=*/nullptr);

	const int32 NumBones = BoneNames.Num();
	for (int32 i = 0; i < NumBones; ++i)
	{
		FMeshBoneInfo BoneInfo(BoneNames[i], FString(), ParentIndices[i]);
		Mod.Add(BoneInfo, RefPose[i], /*bInsertIntoSorted=*/false);
	}

	return RefSkeleton;
}
