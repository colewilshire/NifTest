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

};
