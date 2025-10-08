#pragma once
#include "CoreMinimal.h"

/** One bone weight on a vertex. */
struct FNifVertexInfluence
{
	int32 BoneIndex = INDEX_NONE;
	float Weight = 0.f;
};

/** One vertex as fed to the factory (UE space). */
struct FNifVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FVector3f Normal = FVector3f::ZeroVector;
	FVector2f UV = FVector2f::ZeroVector;           // Always valid; synthesize (0,0) if missing
	TArray<FNifVertexInfluence> Influences;               // Must end up non-empty (factory normalizes/limits)
};

/** Triangle (indices point into Vertices). */
struct FNifFace
{
	int32 Indices[3] = { 0, 0, 0 };
	int32 MaterialIndex = 0;                               // Slot index; factory ensures slots exist
};

/** Minimal material info (slot naming; texture path optional). */
struct FNifMaterial
{
	FString Name;
	FString DiffuseTexturePath;
};

/** Simple bone definition for the reference skeleton. */
struct FNifBone
{
	FString   Name;
	int32     ParentIndex = INDEX_NONE;                    // -1 for root
	FTransform BindPose = FTransform::Identity;         // UE-space bind transform
};

/** Whole mesh payload from the bridge. */
struct FNifMeshData
{
	TArray<FNifVertex>   Vertices;
	TArray<FNifFace>     Faces;
	TArray<FNifMaterial> Materials;
	TArray<FNifBone>     Bones;
};

/** Per-bone keyframes (optional). */
struct FNifKeyframeTrack
{
	int32 BoneIndex = INDEX_NONE;
	TArray<float>     Times;         // seconds
	TArray<FVector3f> Translations;  // per-key
	TArray<FQuat4f>   Rotations;     // per-key
	TArray<FVector3f> Scales;        // per-key
};

/** Animation container (optional). */
struct FNifAnimationData
{
	float Duration = 0.f;            // seconds
	TArray<FNifKeyframeTrack> Tracks;
};

namespace FNiflibBridge
{
	/** Parse a .nif into simple structs (UE-space, units fixed). Return false to cancel import. */
	bool ParseNifFile(const FString& Path, FNifMeshData& OutMesh, FNifAnimationData& OutAnim);
	bool ParseNifFileWithLOD(const FString& Path, int32 RequestedLOD, FNifMeshData& OutMesh, FNifAnimationData& OutAnim);
	int32 GetAuthoredLODCount(const FString& Path);
}
