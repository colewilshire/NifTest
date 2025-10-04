// NifSkeletalMeshFactory.cpp

#include "NifSkeletalMeshFactory.h"
#include "Engine/SkeletalMesh.h"
#include "Logging/LogMacros.h"

// Bridge API (NIF -> intermediate structs)
#include "NiflibBridge.h"

UNifSkeletalMeshFactory::UNifSkeletalMeshFactory()
{
    bEditorImport = true;
    SupportedClass = USkeletalMesh::StaticClass();
    Formats.Add(TEXT("nif;Gamebryo NIF"));
}

bool UNifSkeletalMeshFactory::FactoryCanImport(const FString& Filename)
{
    return Filename.EndsWith(TEXT(".nif"), ESearchCase::IgnoreCase);
}

UObject* UNifSkeletalMeshFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    UE_LOG(LogTemp, Log, TEXT("[NIF] Import requested: %s"), *Filename);

    // 1) Parse the .nif into engine-agnostic structs
    FNifMeshData MeshData;
    FNifAnimationData AnimData;

    if (!FNiflibBridge::ParseNifFile(Filename, MeshData, AnimData))
    {
        UE_LOG(LogTemp, Error, TEXT("[NIF] Parse failed for: %s"), *Filename);
        bOutOperationCanceled = true;
        return nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("[NIF] Parsed OK  | Verts: %d  Faces: %d  Bones: %d  Mats: %d  Tracks: %d"),
        MeshData.Vertices.Num(),
        MeshData.Faces.Num(),
        MeshData.Bones.Num(),
        MeshData.Materials.Num(),
        AnimData.Tracks.Num());

    // 2) STEP 3 (next): build USkeleton + USkeletalMesh (+ optional AnimSequence)
    // For now, return a placeholder SkeletalMesh so we can verify the importer path works.
    USkeletalMesh* NewMesh = NewObject<USkeletalMesh>(InParent, InClass, InName, Flags);

    // Optional: tag the asset so we can recognize it was created by this importer
    if (NewMesh)
    {
        NewMesh->MarkPackageDirty();
        UE_LOG(LogTemp, Log, TEXT("[NIF] Created placeholder SkeletalMesh asset: %s"), *InName.ToString());
    }

    return NewMesh;
}
