#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "NifSkeletalMeshFactoryOld.generated.h"

/**
 * Factory for importing .nif files as Skeletal Meshes
 */
UCLASS()
class UNifSkeletalMeshFactoryOld : public UFactory
{
    GENERATED_BODY()

public:
    UNifSkeletalMeshFactoryOld();

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
};