#pragma once
#include "Factories/Factory.h"
#include "NifSkeletalMeshFactory.generated.h"

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
};
