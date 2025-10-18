#pragma once

// Engine
#include "CoreMinimal.h"
#include "Factories/Factory.h"

// Niflib
#include <obj/NiNode.h>
#include <obj/NiTriShape.h>

// Generated
#include "NifSkeletalMeshFactory.generated.h"

using namespace Niflib;

UCLASS()
class UNifSkeletalMeshFactory : public UFactory
{
	GENERATED_BODY()

public:
	UNifSkeletalMeshFactory();

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled
	) override;

private:
	struct FNifReferenceSkeleton
	{
		TArray<FName> BoneNames;
		TArray<int32> ParentIndices;
		TArray<FTransform> RefPose;
	};
	struct FNifLODGeometry
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		TArray<FVector3f> Normals;
		TArray<FVector4f> Tangents;
		TArray<FVector2f> UVs;
		TArray<FColor> VertexColors;
	};

	FNifLODGeometry ParseNifLODGeometry(const vector<NiTriShapeRef>& LODTriShapes);
	std::vector<NiTriShapeRef> GetDescendantTriShapes(const NiNodeRef& LOD, std::vector<NiTriShapeRef>& FoundTriShapes);
	NiNodeRef FindFirstAncestorThatIsALOD(const NiNodeRef& Node);
};
