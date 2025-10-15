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

// Niflib
#include <niflib.h>
#include <obj/NiLODNode.h>
#include <obj/NiSkinInstance.h>
#include <obj/NiMultiTargetTransformController.h>

using namespace Niflib;

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

	// Parse NIF for mesh data
	UNifSkeletalMeshFactory::FNifReferenceSkeleton NifReferenceSkeleton = ParseNif(Filename);

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

	for (int i = 0; i < Skeleton.BoneNames.Num(); i++)
	{
		UE_LOG(LogTemp, Log, TEXT("[NIF] Name: %s"), *Skeleton.BoneNames[i].ToString());
		UE_LOG(LogTemp, Log, TEXT("[NIF] Index: %d"), i);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Parent: %d"), Skeleton.ParentIndices[i]);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Transform - Location: %s, Rotation: %s, Scale: %s"),
			*Skeleton.RefPose[i].GetLocation().ToString(),
			*Skeleton.RefPose[i].GetRotation().Rotator().ToString(),
			*Skeleton.RefPose[i].GetScale3D().ToString());
	}

	return Skeleton;
}

UNifSkeletalMeshFactory::FNifReferenceSkeleton UNifSkeletalMeshFactory::ParseNifSkeleton(const NiNodeRef& Bone, const int32 PreviousIndex, const int32 ParentIndex, UNifSkeletalMeshFactory::FNifReferenceSkeleton Skeleton)
{
	Vector3 location = Bone->GetLocalTranslation();
	Quaternion rotation = Bone->GetLocalRotation().AsQuaternion();
	float scale = Bone->GetLocalScale();

	FTransform transform;
	transform.SetLocation(FVector(location.x, location.y, location.z));
	transform.SetRotation(FQuat(rotation.x, rotation.y, rotation.z, rotation.w));
	transform.SetScale3D(FVector(scale));

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

// Find NiTriShape, a child of a LOD
// Its child NiTriShapeData contains the mesh triangles, while its other child NiSkinInstance contains the bones

std::map<NiNodeRef, std::vector<NiTriShapeRef>> UNifSkeletalMeshFactory::GetNiTriShapes(const FString& Filename)
{
	std::vector<NiObjectRef> nifList = ReadNifList(TCHAR_TO_UTF8(*Filename));
	std::vector<NiTriShapeRef> niTriShapeList;

	// Get all NiTriShapes in the Nif tree
	for (const NiObjectRef& niObjectRef : nifList)
	{
		NiTriShapeRef niTriShapeRef = DynamicCast<NiTriShape>(niObjectRef);
		if (niTriShapeRef)
		{
			niTriShapeList.push_back(niTriShapeRef);
		}
	}

	// Search ancestors for LODs
	std::map<NiNodeRef, std::vector<NiTriShapeRef>> lods;
	for (const NiTriShapeRef& niTriShapeRef : niTriShapeList)
	{
		NiNodeRef ancestorLOD = FindFirstAncestorThatIsALod(niTriShapeRef->GetParent());

		if (ancestorLOD)
		{
			lods[ancestorLOD].push_back(niTriShapeRef);
		}
	}

	return lods;	// TODO: Return here; clean up code below

	// If no LODs were found, we can assume all NiTriShapes are part of LOD0
	//if (lods.empty())	//TODO: Handle no LODs
	//{

	//}

	// NiTriShapeData
	//for (std::pair<NiNodeRef, std::vector<NiTriShapeRef>> pair : lods)
	//{
	//	for (NiTriShapeRef& niTriShapeRef : pair.second)
	//	{
	//		GetNifSkeleton(niTriShapeRef);
	//		break;	//TODO: Handle other LODs
	//	}
	//	break;	//TODO: Handle Other LODs
	//}
}

NiNodeRef UNifSkeletalMeshFactory::FindFirstAncestorThatIsALod(const NiNodeRef& niNodeRef)
{
	if (!niNodeRef) { return NULL; }
	NiNodeRef parent = niNodeRef->GetParent();

	if (!parent) { return NULL; }	// There are no more valid ancestors, so there must be no LOD
	if (DynamicCast<NiLODNode>(parent)) { return niNodeRef; }	// This NiNode's parent is an NiLODNode, therefore it must be a LOD

	return FindFirstAncestorThatIsALod(parent);	// Recurse up the Nif tree
}

UNifSkeletalMeshFactory::FNifReferenceSkeleton UNifSkeletalMeshFactory::GetNifSkeleton(const NiTriShapeRef& niTriShapeRef)
{
	FString s = UTF8_TO_TCHAR(niTriShapeRef->GetIDString().c_str());
	UE_LOG(LogTemp, Log, TEXT("[NIF] IDString: %s"), *s);
	NiSkinInstanceRef niSkinInstanceRef = niTriShapeRef->GetSkinInstance();
	FString t = UTF8_TO_TCHAR(niSkinInstanceRef->GetIDString().c_str());
	UE_LOG(LogTemp, Log, TEXT("[NIF] IDString: %s"), *t);
	std::vector<NiNodeRef> bones = niTriShapeRef->GetSkinInstance()->GetBones();	// GetSkeletalRoot, then iterating through children may be the way to go, as we are missing the thing called "root"
	//TArray<FName> boneNames;										// We could theoretically just use that to get the real root, but I believe there is a NiNode between the node called Root, and "hip" (the real root?)
	//TArray<int32> parentIndices;									// I also doubt static meshes like Bull even have an NiSkinInstance
	//TArray<FTransform> refPose;
	UNifSkeletalMeshFactory::FNifReferenceSkeleton referenceSkeleton;
	TMap<FName, int32> parentLookupTable;

	UE_LOG(LogTemp, Log, TEXT("[NIF] Bones Count: %d"), bones.size());

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
		referenceSkeleton.BoneNames.Add(boneName);
		referenceSkeleton.RefPose.Add(transform);
		parentLookupTable.Add(boneName, i);

		if (i != 0)
		{
			NiNodeRef parent = bones[i]->GetParent();
			FName parentName = parent->GetName().c_str();
			int32 parentIndex = *parentLookupTable.Find(parentName);
			referenceSkeleton.ParentIndices.Add(parentIndex);
		}
		else
		{
			referenceSkeleton.ParentIndices.Add(-1);	// The root bone should always have a parent index of -1
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[NIF] SkelBones Count: %d"), referenceSkeleton.BoneNames.Num());

	for (int i = 0; i < referenceSkeleton.BoneNames.Num(); i++)
	{
		UE_LOG(LogTemp, Log, TEXT("[NIF] Name: %s"), *referenceSkeleton.BoneNames[i].ToString());
		UE_LOG(LogTemp, Log, TEXT("[NIF] Index: %d"), i);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Parent: %d"), referenceSkeleton.ParentIndices[i]);
		UE_LOG(LogTemp, Log, TEXT("Transform - Location: %s, Rotation: %s, Scale: %s"),
			*referenceSkeleton.RefPose[i].GetLocation().ToString(),
			*referenceSkeleton.RefPose[i].GetRotation().Rotator().ToString(),
			*referenceSkeleton.RefPose[i].GetScale3D().ToString());
	}

	//BuildReferenceSkeleton(referenceSkeleton.BoneNames, parentIndices, refPose);
	return referenceSkeleton;
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
