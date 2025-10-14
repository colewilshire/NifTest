#pragma once

// Engine
#include "CoreMinimal.h"
#include "Factories/Factory.h"

// Niflib
#include <obj/NiNode.h>
#include <obj/NiTriShape.h>

// Generated
#include "NifSkeletalMeshFactory.generated.h"

UCLASS()
class UNifSkeletalMeshFactory : public UFactory
{
	GENERATED_BODY()

public:
	UNifSkeletalMeshFactory();

	// UFactory interface
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
	void GetNiTriShapes(const FString& Filename);
	Niflib::NiNodeRef FindFirstAncestorThatIsALod(const Niflib::NiNodeRef& niNodeRef);
	void GetNifSkeleton(const Niflib::NiTriShapeRef& niTriShapeRef);
};
