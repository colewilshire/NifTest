#pragma once
#include "CoreMinimal.h"

struct FNifVertexInfluence { int32 BoneIndex = INDEX_NONE; float Weight = 0.f; };
struct FNifVertex       { FVector3f Position{}, Normal{}; FVector2f UV{}; TArray<FNifVertexInfluence> Influences; };
struct FNifFace         { int32 Indices[3]{0,0,0}; int32 MaterialIndex = 0; };
struct FNifMaterial     { FString Name; FString DiffuseTexturePath; };
struct FNifBone         { FString Name; int32 ParentIndex = INDEX_NONE; FTransform BindPose = FTransform::Identity; };

struct FNifMeshData {
    TArray<FNifVertex>   Vertices;
    TArray<FNifFace>     Faces;
    TArray<FNifMaterial> Materials;
    TArray<FNifBone>     Bones;
};

struct FNifKeyframeTrack { int32 BoneIndex = INDEX_NONE; TArray<float> Times; TArray<FVector3f> Translations; TArray<FQuat4f> Rotations; TArray<FVector3f> Scales; };
struct FNifAnimationData { float Duration = 0.f; TArray<FNifKeyframeTrack> Tracks; };

namespace FNiflibBridge {
    /** Parse a .nif into simple structs (UE-space, units fixed). Return false to cancel import. */
    bool ParseNifFile(const FString& Path, FNifMeshData& OutMesh, FNifAnimationData& OutAnim);
}
