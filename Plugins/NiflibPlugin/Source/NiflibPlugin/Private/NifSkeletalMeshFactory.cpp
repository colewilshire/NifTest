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
#include "MeshUtilities.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"

// Niflib
#include <niflib.h>
#include <obj/NiLODNode.h>
#include <obj/NiTriShapeData.h>
#include <obj/NiSkinInstance.h>
#include <obj/NiSkinData.h>
#include <obj/NiMultiTargetTransformController.h>
#include <obj/NiMaterialProperty.h>

using namespace Niflib;

static TArray<SkeletalMeshImportData::FRawBoneInfluence> GetRawBoneInfluences(const NiSkinInstanceRef& SkinInstance)
{
	const std::vector<NiNodeRef>& Bones = SkinInstance->GetBones();
	const NiSkinDataRef& SkinData = SkinInstance->GetSkinData();
	TArray<SkeletalMeshImportData::FRawBoneInfluence> RawBoneInfluences;

	for (int BoneIndex = 0; BoneIndex < Bones.size(); BoneIndex++)	// Bone indices will be off by 2 from the reference skeleton, since its only counting children of the skin. Adjust.
	{
		const std::vector<SkinWeight>& SkinWeights = SkinData->GetBoneWeights(BoneIndex);
		for (SkinWeight SkinWeight : SkinWeights)
		{
			SkeletalMeshImportData::FRawBoneInfluence RawBoneInfluence{ SkinWeight.index, BoneIndex, SkinWeight.weight };
			RawBoneInfluences.Add(RawBoneInfluence);
		}
	}

	return RawBoneInfluences;
}

static TArray<SkeletalMeshImportData::FVertInfluence> GetVertexInfluences(const NiSkinInstanceRef& SkinInstance, const NiMultiTargetTransformControllerRef& MultiTargetTransformController)
{
	const std::vector<NiNodeRef>& Bones = SkinInstance->GetBones();
	const NiSkinDataRef& SkinData = SkinInstance->GetSkinData();
	const int TrueBoneCount = MultiTargetTransformController->GetExtraTargets().size() + 1;	//GetExtraTargets dose not cover the Target Bone, so increment by 1.
	const int InfluencelessBoneCount = TrueBoneCount - SkinInstance->GetBoneCount();
	TArray<SkeletalMeshImportData::FVertInfluence> VertexInfluences;

	// An NiSkinData only tracks the bones it influences, so we must offset the starting index by the difference between all bones and those influence by the skin.
	for (int BoneIndex = 0; BoneIndex < Bones.size(); BoneIndex++)
	{
		const std::vector<SkinWeight>& SkinWeights = SkinData->GetBoneWeights(BoneIndex);
		for (SkinWeight SkinWeight : SkinWeights)
		{
			SkeletalMeshImportData::FVertInfluence VertexInfluence{ SkinWeight.weight, SkinWeight.index, BoneIndex + InfluencelessBoneCount };
			VertexInfluences.Add(VertexInfluence);
		}
	}

	return VertexInfluences;
}

//static TArray<SkeletalMeshImportData::FMeshWedge> GetMeshWedges(const NiGeometryDataRef& GeometryData)
//{
//	const std::vector<Vector3>& Vertices = GeometryData->GetVertices();
//	const std::vector<Color4>& Colors = GeometryData->GetColors();
//	TArray<SkeletalMeshImportData::FMeshWedge> MeshWedges;
//
//	for (int VertexIndex = 0; VertexIndex < Vertices.size(); VertexIndex++)
//	{
//		SkeletalMeshImportData::FMeshWedge MeshWedge = {};
//		MeshWedge.iVertex = VertexIndex;
//
//		for (int i = 0; i < GeometryData->GetUVSetCount(); i++)
//		{
//			const std::vector<TexCoord>& UVSet = GeometryData->GetUVSet(i);
//			const FVector2f UV = { UVSet[VertexIndex].u, UVSet[VertexIndex].v};
//			MeshWedge.UVs[i] = UV;
//		}
//
//		if (!Colors.empty())
//		{
//			const FColor Color(
//				(uint8)(Colors[VertexIndex].r * 255.0f),
//				(uint8)(Colors[VertexIndex].g * 255.0f),
//				(uint8)(Colors[VertexIndex].b * 255.0f),
//				(uint8)(Colors[VertexIndex].a * 255.0f)
//			);
//			MeshWedge.Color = Color;
//		}
//
//		MeshWedges.Add(MeshWedge);
//	}
//
//	return MeshWedges;
//}

static TArray<SkeletalMeshImportData::FMeshWedge> GetMeshWedges(const NiGeometryDataRef& GeometryData)
{
	const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(GeometryData);
	const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
	const std::vector<Color4>& Colors = TriShapeData->GetColors();
	TArray<SkeletalMeshImportData::FMeshWedge> MeshWedges;

	for (int TriangleIndex = 0; TriangleIndex < Triangles.size(); TriangleIndex++)
	{
		SkeletalMeshImportData::FMeshWedge MeshWedge[3] = {};
		MeshWedge[0].iVertex = Triangles[TriangleIndex].v1;
		MeshWedge[1].iVertex = Triangles[TriangleIndex].v2;
		MeshWedge[2].iVertex = Triangles[TriangleIndex].v3;

		for (int i = 0; i < std::size(MeshWedge); i++)
		{
			for (int j = 0; j < TriShapeData->GetUVSetCount(); j++)
			{
				const std::vector<TexCoord>& UVSet = TriShapeData->GetUVSet(j);
				const FVector2f UV = { UVSet[MeshWedge[i].iVertex].u, UVSet[MeshWedge[i].iVertex].v };
				MeshWedge[i].UVs[j] = UV;
			}

			if (!Colors.empty())
			{
				const FColor Color(
					(uint8)(Colors[MeshWedge[i].iVertex].r * 255.0f),
					(uint8)(Colors[MeshWedge[i].iVertex].g * 255.0f),
					(uint8)(Colors[MeshWedge[i].iVertex].b * 255.0f),
					(uint8)(Colors[MeshWedge[i].iVertex].a * 255.0f)
				);
				MeshWedge[i].Color = Color;
			}

			MeshWedges.Add(MeshWedge[i]);
		}
	}

	return MeshWedges;
}

static TArray<SkeletalMeshImportData::FMeshFace> GetMeshFaces(const NiGeometryDataRef& GeometryData)
{
	const NiTriShapeDataRef& TriShapeData = DynamicCast<NiTriShapeData>(GeometryData);
	const std::vector<Triangle>& Triangles = TriShapeData->GetTriangles();
	const std::vector<Vector3>& Tangents = TriShapeData->GetTangents();
	const std::vector<Vector3>& Bitangents = TriShapeData->GetBitangents();
	const std::vector<Vector3>& Normals = TriShapeData->GetNormals();
	TArray<SkeletalMeshImportData::FMeshFace> MeshFaces;

	for (Triangle Triangle : Triangles)
	{
		SkeletalMeshImportData::FMeshFace MeshFace = {};
		const uint32 VertexIndices[3] = { Triangle.v1, Triangle.v2, Triangle.v3 };

		MeshFace.MeshMaterialIndex = 0;
		MeshFace.SmoothingGroups = 1;

		for (int i = 0; i < std::size(VertexIndices); i++)
		{
			const FVector3f& Tangent = { Tangents[VertexIndices[i]].x, Tangents[VertexIndices[i]].y, Tangents[VertexIndices[i]].z };
			const FVector3f& Bitangent = { Bitangents[VertexIndices[i]].x, Bitangents[VertexIndices[i]].y, Bitangents[VertexIndices[i]].z };
			const FVector3f& Normal = { Normals[VertexIndices[i]].x, Normals[VertexIndices[i]].y, Normals[VertexIndices[i]].z };

			MeshFace.iWedge[i] = VertexIndices[i];
			MeshFace.TangentX[i] = Tangent;
			MeshFace.TangentY[i] = Bitangent;
			MeshFace.TangentZ[i] = Normal;
		}

		MeshFaces.Add(MeshFace);
	}
	/*const NiPropertyRef& Property = TriShape->GetPropertyByType(NiMaterialProperty::TYPE);
	const NiMaterialPropertyRef& MaterialProperty = DynamicCast<NiMaterialProperty>(Property);
	MaterialProperty->get*/

	return MeshFaces;
}

static TArray<SkeletalMeshImportData::FMeshFace> BuildFacesFromWedges(const TArray<SkeletalMeshImportData::FMeshWedge>& Wedges)
{
	TArray<SkeletalMeshImportData::FMeshFace> Faces;
	Faces.Reserve(Wedges.Num() / 3);

	for (int32 Base = 0; Base + 2 < Wedges.Num(); Base += 3)
	{
		SkeletalMeshImportData::FMeshFace Face{};
		Face.MeshMaterialIndex = 0;
		Face.SmoothingGroups = 1;

		Face.iWedge[0] = static_cast<uint32>(Base + 0);
		Face.iWedge[1] = static_cast<uint32>(Base + 1);
		Face.iWedge[2] = static_cast<uint32>(Base + 2);

		// Leave tangents zero; let the builder recompute if needed
		Faces.Add(Face);
	}
	return Faces;
}

static TArray<FVector3f> GetPoints(const NiGeometryDataRef& GeometryData)
{
	TArray<FVector3f> Points;

	for (const Vector3& Vertex : GeometryData->GetVertices())
	{
		FVector3f Point = { Vertex.x, Vertex.y, Vertex.z };
		Points.Add(Point);
	}

	return Points;
}

static TArray<int32> GetPointsToOriginalMap(const NiGeometryDataRef& GeometryData)
{
	TArray<int32> PointsToOriginalMap;
	const std::vector<Vector3>& Vertices = GeometryData->GetVertices();

	for (int i = 0; i < Vertices.size(); i++)
	{
		PointsToOriginalMap.Add(i);
	}

	return PointsToOriginalMap;
}

struct FNifReferenceSkeleton
{
	TArray<FName> BoneNames;
	TArray<int32> ParentIndices;
	TArray<FTransform> RefPose;
};

static FNifReferenceSkeleton ParseNifSkeleton(const NiNodeRef& Bone, const int32 PreviousIndex = -1, const int32 ParentIndex = -1, FNifReferenceSkeleton Skeleton = {})
{
	const Vector3 Location = Bone->GetLocalTranslation();
	const Quaternion Rotation = Bone->GetLocalRotation().AsQuaternion();
	const float Scale = Bone->GetLocalScale();

	FTransform Transform;
	Transform.SetLocation(FVector(Location.x, Location.y, Location.z));
	Transform.SetRotation(FQuat(Rotation.x, Rotation.y, Rotation.z, Rotation.w));
	Transform.SetScale3D(FVector(Scale));

	FName BoneName = Bone->GetName().c_str();
	Skeleton.BoneNames.Add(BoneName);
	Skeleton.ParentIndices.Add(ParentIndex);
	Skeleton.RefPose.Add(Transform);

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

static FReferenceSkeleton BuildReferenceSkeleton(
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

static void Test(const FString& Filename, USkeletalMesh* SkeletalMesh)
{
	FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
	ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
	FSkeletalMeshLODModel* NewLODModel = &ImportedModel->LODModels[0];

	std::vector<NiObjectRef> NifList = ReadNifList(TCHAR_TO_UTF8(*Filename));
	IMeshUtilities& MeshUtilities = FModuleManager::LoadModuleChecked<IMeshUtilities>("MeshUtilities");
	NiMultiTargetTransformControllerRef MultiTargetTransformController;
	FNifReferenceSkeleton NifSkeleton;

	for (const NiObjectRef& Object : NifList)
	{
		// NiSkinInstanceRef->GetSkeletalRoot() and NiSkinInstanceRef->GetBones()[0] do not reliably return the true skeletal root
		MultiTargetTransformController = DynamicCast<NiMultiTargetTransformController>(Object);
		if (MultiTargetTransformController)
		{
			// The target of NiMultiTargetTransformController should always be the true skeletal root, or else animations wouldn't work, I think
			const NiObjectNETRef& Target = MultiTargetTransformController->GetTarget();
			const NiNodeRef& RootBone = DynamicCast<NiNode>(Target);
			if (RootBone)
			{
				NifSkeleton = ParseNifSkeleton(RootBone);

				break;	// There should only be one NiMultiTargetTransformController, I think, so bail once it is found.
			}
		}
	}

	for (const NiObjectRef& Object : NifList)
	{
		const NiTriShapeRef TriShape = DynamicCast<NiTriShape>(Object);
		if (TriShape)
		{
			const FReferenceSkeleton& ReferenceSkeleton = BuildReferenceSkeleton(NifSkeleton.BoneNames, NifSkeleton.ParentIndices, NifSkeleton.RefPose);
			const TArray<SkeletalMeshImportData::FVertInfluence>& VertexInfluences = GetVertexInfluences(TriShape->GetSkinInstance(), MultiTargetTransformController);
			const TArray<SkeletalMeshImportData::FMeshWedge>& MeshWedges = GetMeshWedges(TriShape->GetData());
			const TArray<SkeletalMeshImportData::FMeshFace>& MeshFaces = BuildFacesFromWedges(MeshWedges);//GetMeshFaces(TriShape->GetData());
			const TArray<FVector3f>& Points = GetPoints(TriShape->GetData());
			const TArray<int32>& PointsToOriginalMap = GetPointsToOriginalMap(TriShape->GetData());
			IMeshUtilities::MeshBuildOptions BuildOptions;
			BuildOptions.bComputeNormals = true;
			BuildOptions.bComputeTangents = true;
			BuildOptions.bUseMikkTSpace = true;

			/////
			int32 NumInfluenceErrors = 0;
			int32 NumWedgeErrors = 0;
			int32 NumFaceErrors = 0;
			int32 NumPointErrors = 0;
			int32 NumPointMapErrors = 0;

			const int64 PointsCount64 = static_cast<int64>(Points.Num());

			// --- Preflight summary & invariants ---
			UE_LOG(LogTemp, Log, TEXT("Preflight: Faces=%d Wedges=%d Points=%d RawPointMap=%d Influences=%d"),
				MeshFaces.Num(), MeshWedges.Num(), Points.Num(), PointsToOriginalMap.Num(), VertexInfluences.Num());

			if (MeshFaces.Num() * 3 != MeshWedges.Num())
			{
				UE_LOG(LogTemp, Error, TEXT("Invariant failed: Faces*3 != Wedges (%d*3 != %d)"),
					MeshFaces.Num(), MeshWedges.Num());
			}

			if (PointsToOriginalMap.Num() != Points.Num())
			{
				UE_LOG(LogTemp, Warning, TEXT("PointToOriginalMap size mismatch: %d vs Points %d"),
					PointsToOriginalMap.Num(), Points.Num());
			}

			// Degenerate geometry checks
			int32 DegenerateFaceCount = 0;
			for (int32 fi = 0; fi < MeshFaces.Num(); ++fi)
			{
				const auto& F = MeshFaces[fi];
				const int32 i0 = (int32)F.iWedge[0];
				const int32 i1 = (int32)F.iWedge[1];
				const int32 i2 = (int32)F.iWedge[2];

				// duplicate wedge indices?
				if (i0 == i1 || i1 == i2 || i0 == i2)
				{
					UE_LOG(LogTemp, Warning, TEXT("Face %d has duplicate wedge indices (%d,%d,%d)"), fi, i0, i1, i2);
					++DegenerateFaceCount;
					continue;
				}

				// zero-area triangle?
				const int32 v0 = (int32)MeshWedges[i0].iVertex;
				const int32 v1 = (int32)MeshWedges[i1].iVertex;
				const int32 v2 = (int32)MeshWedges[i2].iVertex;
				if (v0 < 0 || v1 < 0 || v2 < 0 || v0 >= Points.Num() || v1 >= Points.Num() || v2 >= Points.Num())
				{
					UE_LOG(LogTemp, Error, TEXT("Face %d references out-of-range vertex indices (%d,%d,%d)"), fi, v0, v1, v2);
					++DegenerateFaceCount;
					continue;
				}

				const FVector3f& P0 = Points[v0];
				const FVector3f& P1 = Points[v1];
				const FVector3f& P2 = Points[v2];
				const FVector3f E0 = P1 - P0;
				const FVector3f E1 = P2 - P0;
				const float Area2 = FVector3f::CrossProduct(E0, E1).SizeSquared();

				if (!FMath::IsFinite(Area2) || Area2 <= KINDA_SMALL_NUMBER)
				{
					UE_LOG(LogTemp, Warning, TEXT("Face %d is degenerate (area ~ 0)"), fi);
					++DegenerateFaceCount;
				}
			}
			UE_LOG(LogTemp, Log, TEXT("Preflight: DegenerateFaces=%d"), DegenerateFaceCount);

			// Influence coverage summary
			TArray<float> WeightSums;
			WeightSums.Init(0.f, Points.Num());
			for (const auto& Inf : VertexInfluences)
			{
				const int32 vi = (int32)Inf.VertIndex;
				if (vi >= 0 && vi < WeightSums.Num() && FMath::IsFinite(Inf.Weight))
				{
					WeightSums[vi] += Inf.Weight;
				}
			}

			int32 ZeroInfluenceVerts = 0, BadSumVerts = 0;
			for (int32 vi = 0; vi < WeightSums.Num(); ++vi)
			{
				const float s = WeightSums[vi];
				if (s <= SMALL_NUMBER) { ++ZeroInfluenceVerts; }
				else if (!FMath::IsFinite(s) || s < 0.f || s > 1.f + 0.05f) { ++BadSumVerts; }
			}
			UE_LOG(LogTemp, Log, TEXT("Influence coverage: ZeroInfluenceVerts=%d  BadSumVerts=%d"),
				ZeroInfluenceVerts, BadSumVerts);


			// ---- Influences ----
			for (int32 i = 0; i < VertexInfluences.Num(); i++)
			{
				const auto& Influence = VertexInfluences[i];

				const int64 vIdx64 = static_cast<int64>(Influence.VertIndex);
				const bool bBadVert =
					(vIdx64 < 0) || (vIdx64 >= PointsCount64) || !FMath::IsFinite(Influence.Weight) ||
					(Influence.Weight < 0.0f) || (Influence.Weight > 1.0f);

				if (bBadVert)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[Influence %d] Invalid: VertIndex=%lld (Points=%lld), BoneIndex=%d, Weight=%f"),
						i, vIdx64, PointsCount64, (int32)Influence.BoneIndex, Influence.Weight);
				}

				UE_LOG(LogTemp, Log, TEXT("[Influence %d] VertexIndex=%lld, BoneIndex=%d, Weight=%f"),
					i, vIdx64, (int32)Influence.BoneIndex, Influence.Weight);
			}

			// ---- Wedges ----
			for (int32 i = 0; i < MeshWedges.Num(); i++)
			{
				const auto& Wedge = MeshWedges[i];

				const int64 iVert64 = static_cast<int64>(Wedge.iVertex);
				const bool bBad =
					(iVert64 < 0) || (iVert64 >= PointsCount64) ||
					!FMath::IsFinite(Wedge.UVs[0].X) || !FMath::IsFinite(Wedge.UVs[0].Y);

				if (bBad)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[Wedge %d] Invalid: iVertex=%lld (Points=%lld), UV=(%f,%f)"),
						i, iVert64, PointsCount64, Wedge.UVs[0].X, Wedge.UVs[0].Y);
				}

				UE_LOG(LogTemp, Log,
					TEXT("[Wedge %d] iVertex=%lld, UV=(%f,%f), Color=(R=%d,G=%d,B=%d,A=%d)"),
					i, iVert64, Wedge.UVs[0].X, Wedge.UVs[0].Y,
					Wedge.Color.R, Wedge.Color.G, Wedge.Color.B, Wedge.Color.A);
			}

			// ---- Faces ----
			const int64 NumWedges64 = static_cast<int64>(MeshWedges.Num());

			for (int32 i = 0; i < MeshFaces.Num(); i++)
			{
				const auto& Face = MeshFaces[i];

				int64 w0 = static_cast<int64>(Face.iWedge[0]);
				int64 w1 = static_cast<int64>(Face.iWedge[1]);
				int64 w2 = static_cast<int64>(Face.iWedge[2]);

				bool bBad = false;
				if (w0 < 0 || w0 >= NumWedges64) { UE_LOG(LogTemp, Error, TEXT("[Face %d] iWedge[0]=%lld OOB (NumWedges=%lld)"), i, w0, NumWedges64); bBad = true; }
				if (w1 < 0 || w1 >= NumWedges64) { UE_LOG(LogTemp, Error, TEXT("[Face %d] iWedge[1]=%lld OOB (NumWedges=%lld)"), i, w1, NumWedges64); bBad = true; }
				if (w2 < 0 || w2 >= NumWedges64) { UE_LOG(LogTemp, Error, TEXT("[Face %d] iWedge[2]=%lld OOB (NumWedges=%lld)"), i, w2, NumWedges64); bBad = true; }

				if (w0 == w1 || w1 == w2 || w0 == w2)
				{
					UE_LOG(LogTemp, Warning, TEXT("[Face %d] Degenerate: iWedge=(%lld,%lld,%lld)"), i, w0, w1, w2);
				}

				UE_LOG(LogTemp, Log, TEXT("[Face %d] iWedge=(%lld,%lld,%lld), MatIndex=%d, Smoothing=%u"),
					i, w0, w1, w2, Face.MeshMaterialIndex, Face.SmoothingGroups);
			}

			// Points
			for (int32 i = 0; i < Points.Num(); i++)
			{
				const auto& P = Points[i];
				if (P.ContainsNaN() || !FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z))
				{
					UE_LOG(LogTemp, Error, TEXT("[Point %d] Invalid (NaN/Inf): (%f,%f,%f)"), i, P.X, P.Y, P.Z);
				}
				UE_LOG(LogTemp, Log, TEXT("[Point %d] (%f,%f,%f)"), i, P.X, P.Y, P.Z);
			}

			// Point map
			for (int32 i = 0; i < PointsToOriginalMap.Num(); i++)
			{
				int64 orig64 = static_cast<int64>(PointsToOriginalMap[i]);
				if (orig64 < 0 /* or compare to original source count if you track it */)
				{
					UE_LOG(LogTemp, Warning, TEXT("[PointMap %d] Invalid OriginalIndex=%lld"), i, orig64);
				}
				UE_LOG(LogTemp, Log, TEXT("[PointMap %d] OriginalIndex=%lld"), i, orig64);
			}

			// ---- Summary ----
			UE_LOG(LogTemp, Warning, TEXT("Summary of validation: Influences=%d errors, Wedges=%d errors, Faces=%d errors, Points=%d errors, PointMap=%d errors"),
				NumInfluenceErrors, NumWedgeErrors, NumFaceErrors, NumPointErrors, NumPointMapErrors);

			/////

			MeshUtilities.BuildSkeletalMesh(
				*NewLODModel,
				SkeletalMesh->GetName(),
				ReferenceSkeleton,
				VertexInfluences,
				MeshWedges,
				MeshFaces,
				Points,
				PointsToOriginalMap,
				BuildOptions
			);

			SkeletalMesh->AddLODInfo();
			FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(0);
			LODInfo->BuildSettings.bRecomputeNormals = true;
			LODInfo->BuildSettings.bRecomputeTangents = true;
			LODInfo->BuildSettings.bUseMikkTSpace = true;
			//LODInfo.ScreenSize.Default = 1.0f;

			/////
			// Ensure LOD reports at least one UV channel (NumTexCoords lives on LODModel in UE 5.4)
			NewLODModel->NumTexCoords = FMath::Max<uint32>(NewLODModel->NumTexCoords, 1u);

			// Materials slots (minimum)
			int32 MaxSectionMatIndex = -1;
			for (const FSkelMeshSection& Sec : NewLODModel->Sections)
				MaxSectionMatIndex = FMath::Max(MaxSectionMatIndex, (int32)Sec.MaterialIndex);

			if (MaxSectionMatIndex >= 0)
				while (SkeletalMesh->GetMaterials().Num() <= MaxSectionMatIndex)
					SkeletalMesh->GetMaterials().Add(FSkeletalMaterial());

			// Bounds (from this LOD’s points)
			{
				FBox BoundsBox(ForceInit);
				for (const FVector3f& P : Points)
					BoundsBox += (FVector)P;
				if (BoundsBox.IsValid)
					SkeletalMesh->SetImportedBounds(FBoxSphereBounds(BoundsBox));
			}
			/////

			break; // TODO: Remove once I can handle multiple LODs, This will only get TriShape of the LOD, not the full LOD
		}
	}
}

// Create a unique package under the same folder as the selected destination
static UPackage* MakeAssetPackage(const FString& BasePath, const FString& AssetName, FString& OutObjectName)
{
	FString PackageName;
	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	AssetTools.Get().CreateUniqueAssetName(BasePath / AssetName, TEXT(""), PackageName, OutObjectName);
	return CreatePackage(*PackageName);
}

// Given tangent, bitangent, and normal from NIF data, produce an Unreal-compatible tangent
static FVector4f MakeUnrealTangent(const FVector3f& Tangent, const FVector3f& Bitangent, const FVector3f& Normal)
{
	// Ensure all are normalized
	FVector3f T = Tangent.GetSafeNormal();
	FVector3f B = Bitangent.GetSafeNormal();
	FVector3f N = Normal.GetSafeNormal();

	// Calculate handedness: +1 if B matches cross(N,T), -1 if opposite
	float Handedness = (FVector3f::DotProduct(FVector3f::CrossProduct(N, T), B) < 0.0f) ? -1.0f : 1.0f;

	// Unreal packs XYZ in Tangent, and W as sign of bitangent
	return FVector4f(T.X, T.Y, T.Z, Handedness);
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
	//Test(Filename);

	// Create packages/assets
	const FString BasePath = InParent->GetOutermost()->GetName();
	FString SkelObjName, MeshObjName;

	// Create mesh
	UPackage* MeshPkg = MakeAssetPackage(BasePath, InName.ToString(), MeshObjName);
	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(MeshPkg, *MeshObjName, RF_Public | RF_Standalone);

	// Build reference skeleton
	FReferenceSkeleton ReferenceSkeleton = BuildReferenceSkeleton(NifReferenceSkeleton.BoneNames, NifReferenceSkeleton.ParentIndices, NifReferenceSkeleton.RefPose);
	SkeletalMesh->SetRefSkeleton(ReferenceSkeleton);

	/////
	Test(Filename, SkeletalMesh);
	/////

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

	/*for (int i = 0; i < Skeleton.BoneNames.Num(); i++)
	{
		UE_LOG(LogTemp, Log, TEXT("[NIF] Name: %s"), *Skeleton.BoneNames[i].ToString());
		UE_LOG(LogTemp, Log, TEXT("[NIF] Index: %d"), i);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Parent: %d"), Skeleton.ParentIndices[i]);
		UE_LOG(LogTemp, Log, TEXT("[NIF] Transform - Location: %s, Rotation: %s, Scale: %s"),
			*Skeleton.RefPose[i].GetLocation().ToString(),
			*Skeleton.RefPose[i].GetRotation().Rotator().ToString(),
			*Skeleton.RefPose[i].GetScale3D().ToString());
	}*/

	//std::map<NiNodeRef, std::vector<NiTriShapeRef>> TriShapesByLOD;
	//for (const NiObjectRef& Object : NifList)
	//{
	//	const NiTriShapeRef& TriShape = DynamicCast<NiTriShape>(Object);
	//	if (TriShape)
	//	{
	//		const NiNodeRef& LOD = FindFirstAncestorThatIsALOD(TriShape->GetParent());	// NiLODNodeRef->GetChildre() is numerically indexed, so perhaps this is how LOD order is determined
	//		if (LOD)	// The shadow NiTriShape in dyn_patron is not under a LOD, but the ones in Horse are, meaning the former will return null
	//		{			// It is unclear how to define a shadow. Perhaps NiTriShape lacking a NiTexturingProperty could work, although that may be too broad. NiStencilProperty? NiZBufferProperty?
	//			TriShapesByLOD[LOD].push_back(TriShape);
	//			// TriShape->GetData()->GetNormals();	//GetData seems to give us most of the data we need for the first step
	//		}
	//	}
	//}

	std::map <NiNodeRef, std::vector<NiTriShapeRef>> Map;
	std::vector<std::vector<NiTriShapeRef>> qwerty;	// The LODs themselves don't hold much info but their names and childre, so we could ditch their pointer and just identify by index
	for (const NiObjectRef& Object : NifList)
	{
		const NiLODNodeRef& LODNode = DynamicCast<NiLODNode>(Object);
		if (LODNode)
		{
			// This should only return valid LODs. NiRangeLODData is a property, not a child, and any other NiNode would erroneously be considered a LOD by NiRangeLODData if it was a child of NiLODNode.
			for (const NiAVObjectRef& Child : LODNode->GetChildren())
			{
				const NiNodeRef& LOD = DynamicCast<NiNode>(Child);
				if (LOD)
				{
					GetDescendantTriShapes(LOD, Map[LOD]);
				}
			}
			break;	// There should only be one NiLODNode per NIF
		}
	}

	/*for (const auto& Pair : Map)
	{
		FString s = UTF8_TO_TCHAR(Pair.first->GetIDString().c_str());
		UE_LOG(LogTemp, Log, TEXT("[NIF] IDString: %s"), *s);

		for (const auto& Child : Pair.second)
		{
			FString t = UTF8_TO_TCHAR(Child->GetIDString().c_str());
			UE_LOG(LogTemp, Log, TEXT("	[NIF] IDString: %s"), *t);
		}
	}*/

	return Skeleton;
}

UNifSkeletalMeshFactory::FNifReferenceSkeleton UNifSkeletalMeshFactory::ParseNifSkeleton(const NiNodeRef& Bone, const int32 PreviousIndex, const int32 ParentIndex, UNifSkeletalMeshFactory::FNifReferenceSkeleton Skeleton)
{
	const Vector3 Location = Bone->GetLocalTranslation();
	const Quaternion Rotation = Bone->GetLocalRotation().AsQuaternion();
	const float Scale = Bone->GetLocalScale();

	FTransform transform;
	transform.SetLocation(FVector(Location.x, Location.y, Location.z));
	transform.SetRotation(FQuat(Rotation.x, Rotation.y, Rotation.z, Rotation.w));
	transform.SetScale3D(FVector(Scale));

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

UNifSkeletalMeshFactory::FNifLODGeometry UNifSkeletalMeshFactory::ParseNifLODGeometry(const vector<NiTriShapeRef>& LODTriShapes)
{
	UNifSkeletalMeshFactory::FNifLODGeometry LODGeometry;

	for (const NiTriShapeRef& TriShape : LODTriShapes)
	{
		const NiGeometryDataRef& GeometryData = TriShape->GetData();
		if (GeometryData)
		{
			const std::vector<Vector3>& Vertices = GeometryData->GetVertices();
			const std::vector<Vector3>& Tangents = GeometryData->GetTangents();
			const std::vector<Vector3>& Bitangents = GeometryData->GetBitangents();
			const std::vector<Vector3>& Normals = GeometryData->GetNormals();
			const std::vector<TexCoord>& UVSet = GeometryData->GetUVSet(0);	// Need to null handle index, and figure out how to derive it programmatically

			for (int i = 0; i < Vertices.size(); i++)
			{
				FVector3f Position = { Vertices[i].x, Vertices[i].y, Vertices[i].z };
				FVector2f UV = { UVSet[i].u, UVSet[i].v};
				FVector3f Tangent = { Tangents[i].x, Tangents[i].y, Tangents[i].z };
				FVector3f Bitangent = { Bitangents[i].x, Bitangents[i].y, Bitangents[i].z };
				FVector3f Normal = { Normals[i].x, Normals[i].y, Normals[i].z };
				FVector4f UnrealTangent = MakeUnrealTangent(Tangent, Bitangent, Normal);

				LODGeometry.Positions.Add(Position);
				LODGeometry.Normals.Add(Normal);
				LODGeometry.Tangents.Add(UnrealTangent);

				// Handle vertex colors (NifShapeData has a property for if vertex colors are used or not
			}

			for (const int& Index : GeometryData->GetVertexIndices())
			{
				LODGeometry.Indices.Add(Index);
			}
		}
	}

	return LODGeometry;
}

std::vector<NiTriShapeRef> UNifSkeletalMeshFactory::GetDescendantTriShapes(const NiNodeRef& Parent, std::vector<NiTriShapeRef>& FoundTriShapes)
{
	for (const NiAVObjectRef& Child : Parent->GetChildren())
	{
		const NiTriShapeRef& TriShape = DynamicCast<NiTriShape>(Child);
		if (TriShape)
		{
			FoundTriShapes.push_back(TriShape);
		}
		else
		{
			const NiNodeRef& Node = DynamicCast<NiNode>(Child);
			if (Node)
			{
				FoundTriShapes = GetDescendantTriShapes(Node, FoundTriShapes);
			}
		}
	}

	return FoundTriShapes;
}

NiNodeRef UNifSkeletalMeshFactory::FindFirstAncestorThatIsALOD(const NiNodeRef& Node)
{
	if (!Node) { return NULL; }
	NiNodeRef Parent = Node->GetParent();

	if (!Parent) { return NULL; }	// There are no more valid ancestors, so there must be no LOD
	if (DynamicCast<NiLODNode>(Parent)) { return Node; }	// This NiNode's parent is an NiLODNode, therefore it must be a LOD

	return FindFirstAncestorThatIsALOD(Parent);	// Recurse up the Nif tree
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
