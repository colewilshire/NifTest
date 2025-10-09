#include "NiflibBridge.h"
#include "Logging/LogMacros.h"

// --- Niflib headers ---
#include <niflib.h>
#include <obj/NiObject.h>
#include <obj/NiNode.h>
#include <obj/NiLODNode.h>
#include <obj/NiAVObject.h>
#include <obj/NiGeometry.h>
#include <obj/NiGeometryData.h>
#include <obj/NiTriShape.h>
#include <obj/NiTriShapeData.h>
#include <obj/NiTriStrips.h>
#include <obj/NiTriStripsData.h>
#include <obj/NiMaterialProperty.h>
#include <obj/NiTexturingProperty.h>
#include <obj/NiSourceTexture.h>
#include <obj/NiStencilProperty.h>
#include <gen/enums.h>
#include <obj/NiSkinInstance.h>
#include <obj/NiSkinData.h>
#include <obj/NiSkinPartition.h>

using namespace Niflib;

namespace
{
    // --------- toggles ---------
    static constexpr bool bCreateStubBonesForUnmappedSkinBones = true;

    // --------- small helpers ---------
    static FORCEINLINE FVector3f ToUE(const Vector3& v)
    {
        return FVector3f((float)v.x, (float)v.y, (float)v.z);
    }
    static FORCEINLINE FVector2f ToUE_NoFlipV(const TexCoord& uv)
    {
        return FVector2f((float)uv.u, (float)uv.v);
    }
    static FORCEINLINE FVector2f ToUE_FlipV(const TexCoord& uv)
    {
        const float u = FMath::IsFinite((float)uv.u) ? (float)uv.u : 0.0f;
        const float v = FMath::IsFinite((float)uv.v) ? (float)uv.v : 0.0f;
        return FVector2f(u, 1.0f - v);
    }

    static FTransform LocalToFTransform(const NiAVObjectRef& Obj)
    {
        const Vector3 T = Obj->GetLocalTranslation();
        const Matrix33 R = Obj->GetLocalRotation();
        const float S = Obj->GetLocalScale();

        FMatrix Rot(
            FPlane((float)R[0][0], (float)R[0][1], (float)R[0][2], 0.f),
            FPlane((float)R[1][0], (float)R[1][1], (float)R[1][2], 0.f),
            FPlane((float)R[2][0], (float)R[2][1], (float)R[2][2], 0.f),
            FPlane((float)T.x, (float)T.y, (float)T.z, 1.f)
        );
        FTransform Xf(Rot);
        if (!FMath::IsNearlyEqual(S, 1.f))
        {
            Xf.SetScale3D(FVector(S));
        }
        return Xf;
    }

    static FString CanonName(const FString& In)
    {
        FString S = In;
        if (S.StartsWith(TEXT("Game_"), ESearchCase::CaseSensitive))
        {
            S.RightChopInline(5, false);
        }
        return S;
    }

    // ---------- traversal context ----------
    struct FTraversalCtx
    {
        FNifMeshData& Mesh;
        int32 VertexBase = 0;

        TMap<const void*, int32> NodeToBoneIndex;
        TMap<FString, int32> NameToBoneIndex;
        TSet<const void*> VisitedGeoData;

        bool bBonesBuilt = false;

        int32 RequestedLOD = -1;
        int32 PrimaryRootIndex = INDEX_NONE;
    };

    static int32 EnsureBoneForNode(const NiAVObjectRef& Node, FTraversalCtx& Ctx)
    {
        if (!Node) return INDEX_NONE;

        const void* Key = Node.operator->();
        if (int32* Found = Ctx.NodeToBoneIndex.Find(Key))
        {
            return *Found;
        }

        int32 ParentIdx = INDEX_NONE;
        if (NiNodeRef P = Node->GetParent())
        {
            NiAVObjectRef PAsAV = DynamicCast<NiAVObject>(P);
            ParentIdx = EnsureBoneForNode(PAsAV, Ctx);
        }
        else
        {
            if (Ctx.PrimaryRootIndex != INDEX_NONE)
            {
                ParentIdx = Ctx.PrimaryRootIndex;
            }
        }

        FString BoneName = UTF8_TO_TCHAR(Node->GetName().c_str());
        if (BoneName.IsEmpty())
        {
            BoneName = TEXT("Bone");
        }

        FNifBone UEbone;
        UEbone.Name = BoneName;
        UEbone.ParentIndex = ParentIdx;
        UEbone.BindPose = LocalToFTransform(Node);

        const int32 NewIdx = Ctx.Mesh.Bones.Add(UEbone);

        if (ParentIdx == INDEX_NONE && Ctx.PrimaryRootIndex == INDEX_NONE)
        {
            Ctx.PrimaryRootIndex = NewIdx;
        }

        Ctx.NodeToBoneIndex.Add(Key, NewIdx);
        Ctx.NameToBoneIndex.FindOrAdd(UEbone.Name, NewIdx);
        Ctx.NameToBoneIndex.FindOrAdd(CanonName(UEbone.Name), NewIdx);
        return NewIdx;
    }

    static int32 EnsureStubBoneByName(const FString& BoneNameRaw, FTraversalCtx& Ctx)
    {
        const FString Canon = CanonName(BoneNameRaw);

        if (int32* FoundCanon = Ctx.NameToBoneIndex.Find(Canon))
        {
            return *FoundCanon;
        }
        if (int32* FoundExact = Ctx.NameToBoneIndex.Find(BoneNameRaw))
        {
            return *FoundExact;
        }

        FNifBone UEbone;
        UEbone.Name = BoneNameRaw;
        UEbone.ParentIndex = (Ctx.PrimaryRootIndex != INDEX_NONE) ? Ctx.PrimaryRootIndex : INDEX_NONE;
        UEbone.BindPose = FTransform::Identity;

        const int32 NewIdx = Ctx.Mesh.Bones.Add(UEbone);
        if (UEbone.ParentIndex == INDEX_NONE && Ctx.PrimaryRootIndex == INDEX_NONE)
        {
            Ctx.PrimaryRootIndex = NewIdx;
        }

        Ctx.NameToBoneIndex.FindOrAdd(UEbone.Name, NewIdx);
        Ctx.NameToBoneIndex.FindOrAdd(Canon, NewIdx);
        return NewIdx;
    }

    static void BuildBonesFromSkin(const NiSkinInstanceRef& Skin, FTraversalCtx& Ctx)
    {
        if (!Skin) return;

        const std::vector<NiNodeRef> BoneNodes = Skin->GetBones();
        if (BoneNodes.empty())
        {
            return;
        }

        if (NiNodeRef SkelRoot = Skin->GetSkeletonRoot())
        {
            NiAVObjectRef SkelRootAsAV = DynamicCast<NiAVObject>(SkelRoot);
            if (SkelRootAsAV)
            {
                EnsureBoneForNode(SkelRootAsAV, Ctx);
            }
        }

        for (const NiNodeRef& B : BoneNodes)
        {
            if (!B) continue;
            NiAVObjectRef BAsAV = DynamicCast<NiAVObject>(B);
            if (BAsAV)
            {
                EnsureBoneForNode(BAsAV, Ctx);
            }
        }

        Ctx.bBonesBuilt = true;
    }

    static FString GetDiffuseTexturePath(const NiGeometryRef& Geo)
    {
        if (!Geo) return FString();

        const std::vector<NiPropertyRef> props = Geo->GetProperties();
        for (const NiPropertyRef& p : props)
        {
            if (NiTexturingPropertyRef TP = DynamicCast<NiTexturingProperty>(p))
            {
                if (TP->HasTexture(Niflib::BASE_MAP))
                {
                    const TexDesc& Base = TP->GetTexture(Niflib::BASE_MAP);
                    if (Base.source)
                    {
                        const std::string& File = Base.source->GetTextureFileName();
                        if (!File.empty())
                        {
                            return UTF8_TO_TCHAR(File.c_str());
                        }
                    }
                }
            }
        }
        return FString();
    }

    static bool HasStencilProperty(const NiGeometryRef& Geo)
    {
        if (!Geo) return false;
        const std::vector<NiPropertyRef> props = Geo->GetProperties();
        for (const NiPropertyRef& p : props)
        {
            if (DynamicCast<NiStencilProperty>(p))
            {
                return true;
            }
        }
        return false;
    }

    static int32 GetTriangleCount(const NiAVObjectRef& Obj)
    {
        if (!Obj) return 0;
        NiGeometryRef Geo = DynamicCast<NiGeometry>(Obj);
        if (!Geo) return 0;

        NiGeometryDataRef GeoData = Geo->GetData();
        if (!GeoData) return 0;

        if (NiTriShapeRef TriShape = DynamicCast<NiTriShape>(Obj))
        {
            if (NiTriShapeDataRef TriData = DynamicCast<NiTriShapeData>(GeoData))
            {
                return (int32)TriData->GetTriangles().size();
            }
        }
        else if (NiTriStripsRef Strips = DynamicCast<NiTriStrips>(Obj))
        {
            if (NiTriStripsDataRef StripsData = DynamicCast<NiTriStripsData>(GeoData))
            {
                return (int32)StripsData->GetTriangles().size();
            }
        }
        return 0;
    }

    // ---------- variant selection helpers ----------
    struct FGeoCand
    {
        NiGeometryRef Geo;
        FTransform    WorldXf;
        FString       Name;
        FString       BaseKey;
        int32         Tris = 0;
        int32         LodTag = -1; // parsed "lodN" or trailing number, if any
    };

    static int32 ExtractLODTag(const FString& NameLower)
    {
        // Prefer "lod<N>" pattern
        int32 LodPos = NameLower.Find(TEXT("lod"), ESearchCase::IgnoreCase);
        if (LodPos != INDEX_NONE)
        {
            int32 i = LodPos + 3;
            int32 Val = 0;
            bool bAny = false;
            while (i < NameLower.Len() && FChar::IsDigit(NameLower[i]))
            {
                bAny = true;
                Val = Val * 10 + (NameLower[i] - TCHAR('0'));
                ++i;
            }
            if (bAny) return Val;
        }

        // Fallback: trailing digits
        int32 i = NameLower.Len() - 1;
        int32 mul = 1;
        int32 val = 0;
        bool bAny = false;
        while (i >= 0 && FChar::IsDigit(NameLower[i]))
        {
            bAny = true;
            val += (NameLower[i] - TCHAR('0')) * mul;
            mul *= 10;
            --i;
        }
        return bAny ? val : -1;
    }

    static FString BaseKeyFromName(FString InName)
    {
        FString L = InName.ToLower();

        // drop suffix "shape"
        if (L.EndsWith(TEXT("shape")))
        {
            L.LeftChopInline(5, false);
        }

        // strip proxy hints entirely to group with the base
        int32 ShadowPos = L.Find(TEXT("shadow"));
        if (ShadowPos != INDEX_NONE)
        {
            L = L.Left(ShadowPos);
        }

        // strip "_lodN..." or "lodN..." suffixes
        int32 LodPos = L.Find(TEXT("lod"));
        if (LodPos != INDEX_NONE)
        {
            L = L.Left(LodPos);
        }

        // strip trailing digits
        int32 End = L.Len();
        while (End > 0 && FChar::IsDigit(L[End - 1]))
        {
            --End;
        }
        L.LeftInline(End, false);

        // trim trailing underscores/spaces
        while (L.Len() > 0 && (L[L.Len() - 1] == TCHAR('_') || L[L.Len() - 1] == TCHAR(' ')))
        {
            L.LeftChopInline(1, false);
        }

        return L;
    }

    // Append *selected* geometry (NiGeometryRef directly)
    static void AppendGeometryFromGeo(const NiGeometryRef& Geo, const FTransform& WorldXf, FTraversalCtx& Ctx)
    {
        if (!Geo) return;

        // Quick duplicate-data guard
        if (NiGeometryDataRef GeoData = Geo->GetData())
        {
            const void* DataKey = GeoData.operator->();
            if (Ctx.VisitedGeoData.Contains(DataKey))
            {
                UE_LOG(LogTemp, Verbose, TEXT("[NIF] Skipping duplicate geometry data: %s"),
                    *FString(UTF8_TO_TCHAR(Geo->GetName().c_str())));
                return;
            }
            Ctx.VisitedGeoData.Add(DataKey);
        }
        else
        {
            return;
        }

        const FString GeoName = UTF8_TO_TCHAR(Geo->GetName().c_str());
        NiGeometryDataRef GeoData = Geo->GetData();
        if (!GeoData) return;

        const int NumVerts = GeoData->GetVertexCount();
        if (NumVerts <= 0) return;

        // Streams we need to compute the proxy/shadow heuristic
        const int UVSetCount = GeoData->GetUVSetCount();
        std::vector<std::vector<TexCoord>> UVSets;
        UVSets.resize(FMath::Max(0, UVSetCount));
        for (int setIdx = 0; setIdx < UVSetCount; ++setIdx)
        {
            UVSets[setIdx] = GeoData->GetUVSet(setIdx);
        }

        // Compute UV0 non-zero coverage
        int32 UV0Count = 0, UV0NonZero = 0;
        if (UVSetCount > 0)
        {
            const auto& Set0 = UVSets[0];
            UV0Count = (int32)Set0.size();
            for (int32 i = 0; i < UV0Count; ++i)
            {
                const TexCoord& uv = Set0[i];
                if ((float)uv.u != 0.0f || (float)uv.v != 0.0f)
                {
                    ++UV0NonZero;
                }
            }
        }
        const float UV0Coverage = (UV0Count > 0) ? (float)UV0NonZero / (float)UV0Count : 0.0f;

        // Diffuse / stencil signals
        const FString DiffusePath = GetDiffuseTexturePath(Geo);
        const bool bStencil = HasStencilProperty(Geo);
        const bool bLooksProxyByUV = (UV0Count == 0) || (UV0Coverage < 0.20f);
        const bool bLooksProxy = ((DiffusePath.IsEmpty() && bLooksProxyByUV) || bStencil);

        if (bLooksProxy)
        {
            UE_LOG(LogTemp, Log,
                TEXT("[NIF] Skipping proxy/shadow geo '%s'  UV0=%d/%d (%.1f%%)  Diffuse='%s'  Stencil=%s"),
                *GeoName,
                UV0NonZero, UV0Count, UV0Coverage * 100.f,
                *DiffusePath,
                bStencil ? TEXT("true") : TEXT("false"));
            return;
        }

        // Build triangle index list
        TArray<uint32> Indices;
        if (NiTriShapeRef TriShape = DynamicCast<NiTriShape>(Geo))
        {
            if (NiTriShapeDataRef TriData = DynamicCast<NiTriShapeData>(GeoData))
            {
                const std::vector<Triangle> tris = TriData->GetTriangles();
                Indices.Reserve((int32)tris.size() * 3);
                for (const Triangle& t : tris)
                {
                    Indices.Add(t.v1); Indices.Add(t.v3); Indices.Add(t.v2);
                }
            }
        }
        else if (NiTriStripsRef Strips = DynamicCast<NiTriStrips>(Geo))
        {
            if (NiTriStripsDataRef StripsData = DynamicCast<NiTriStripsData>(GeoData))
            {
                const std::vector<Triangle> tris = StripsData->GetTriangles();
                Indices.Reserve((int32)tris.size() * 3);
                for (const Triangle& t : tris)
                {
                    Indices.Add(t.v1); Indices.Add(t.v3); Indices.Add(t.v2);
                }
            }
        }
        if (Indices.Num() == 0) return;

        // Fetch remaining streams for building vertices
        const std::vector<Vector3> Verts = GeoData->GetVertices();
        const std::vector<Vector3> Normals = GeoData->GetNormals();
        const bool bHasNormals = !Normals.empty();

        // ---- UV sets (log only) ----
        if (UVSetCount > 0)
        {
            FString Sizes; Sizes.Reserve(64);
            for (int setIdx = 0; setIdx < UVSetCount; ++setIdx)
            {
                Sizes += (setIdx == 0 ? TEXT("") : TEXT(", "));
                Sizes += FString::Printf(TEXT("%d:%d"), setIdx, (int32)UVSets[setIdx].size());
            }
            UE_LOG(LogTemp, Log, TEXT("[NIF] Geo='%s' UV sets: %d  (sizes: %s)"),
                *GeoName, UVSetCount, *Sizes);

            for (int setIdx = 0; setIdx < UVSetCount; ++setIdx)
            {
                int32 NonZero = 0;
                const auto& Set = UVSets[setIdx];
                const int32 Count = (int32)Set.size();
                for (int32 i = 0; i < Count; ++i)
                {
                    const TexCoord& uv = Set[i];
                    if ((float)uv.u != 0.0f || (float)uv.v != 0.0f)
                    {
                        ++NonZero;
                    }
                }
                UE_LOG(LogTemp, Verbose, TEXT("[NIF] Geo='%s' UV%d non-zero verts: %d / %d"),
                    *GeoName, setIdx, NonZero, Count);
            }
        }
        else
        {
            UE_LOG(LogTemp, Verbose, TEXT("[NIF] Geo='%s' has no UV sets."), *GeoName);
        }
        // ----------------------------

        // Skin
        NiSkinInstanceRef Skin = Geo->GetSkinInstance();
        NiSkinDataRef SkinData = Skin ? Skin->GetSkinData() : NiSkinDataRef();

        if (Skin && !Ctx.bBonesBuilt)
        {
            BuildBonesFromSkin(Skin, Ctx);
        }

        int32 TotalCollectedWeights = 0;
        int32 MissedBoneMapPtr = 0;
        int32 MissedNameOrCanon = 0;
        int32 MissedCanonOnly = 0;
        int32 ZeroInfluenceVertsPreFallback = 0;
        TSet<int32> BonesUsed;
        TArray<int32> PerVertCount;
        PerVertCount.Init(0, NumVerts);

        TArray<TArray<FNifVertexInfluence>> PerVertInfl;
        TArray<FString> UnmappedNames;

        if (Skin && SkinData && (Ctx.NodeToBoneIndex.Num() > 0 || Ctx.NameToBoneIndex.Num() > 0))
        {
            PerVertInfl.SetNum(NumVerts);
            const std::vector<NiNodeRef> BoneNodes = Skin->GetBones();

            for (unsigned int boneIdx = 0; boneIdx < BoneNodes.size(); ++boneIdx)
            {
                const NiNodeRef BoneNode = BoneNodes[boneIdx];
                if (!BoneNode) continue;

                const void* Key = BoneNode.operator->();
                int32 UEBoneIndex = INDEX_NONE;
                if (int32* FoundByPtr = Ctx.NodeToBoneIndex.Find(Key))
                {
                    UEBoneIndex = *FoundByPtr;
                }
                else
                {
                    ++MissedBoneMapPtr;
                    const FString BoneName = UTF8_TO_TCHAR(BoneNode->GetName().c_str());
                    const FString Canon = CanonName(BoneName);

                    if (int32* FoundByName = Ctx.NameToBoneIndex.Find(BoneName))
                    {
                        UEBoneIndex = *FoundByName;
                    }
                    else if (int32* FoundByCanon = Ctx.NameToBoneIndex.Find(Canon))
                    {
                        UEBoneIndex = *FoundByCanon;
                    }
                    else
                    {
                        ++MissedNameOrCanon;
                        ++MissedCanonOnly;
                        UnmappedNames.AddUnique(BoneName);

                        if (bCreateStubBonesForUnmappedSkinBones)
                        {
                            UEBoneIndex = EnsureStubBoneByName(BoneName, Ctx);
                        }
                    }
                }

                if (UEBoneIndex == INDEX_NONE) continue;

                const std::vector<SkinWeight>& Weights = SkinData->GetBoneWeights(boneIdx);
                for (const SkinWeight& SW : Weights)
                {
                    const int v = (int)SW.index;
                    if (v >= 0 && v < NumVerts && SW.weight > 0.0f)
                    {
                        FNifVertexInfluence I;
                        I.BoneIndex = UEBoneIndex;
                        I.Weight = (float)SW.weight;
                        PerVertInfl[v].Add(I);

                        ++PerVertCount[v];
                        ++TotalCollectedWeights;
                        BonesUsed.Add(UEBoneIndex);
                    }
                }
            }

            for (int32 vi = 0; vi < NumVerts; ++vi)
            {
                if (PerVertCount[vi] == 0)
                {
                    ++ZeroInfluenceVertsPreFallback;
                }
            }

            UE_LOG(LogTemp, Log, TEXT("[NIF][Skin] Geo='%s' Verts=%d  TotalWeights=%d  ZeroInfVerts(pre-fallback)=%d  MissPtr=%d  MissNameOrCanon=%d  MissCanon=%d  BonesUsed=%d"),
                *GeoName,
                NumVerts,
                TotalCollectedWeights,
                ZeroInfluenceVertsPreFallback,
                MissedBoneMapPtr,
                MissedNameOrCanon,
                MissedCanonOnly,
                BonesUsed.Num());

            if (UnmappedNames.Num() > 0 && !bCreateStubBonesForUnmappedSkinBones)
            {
                UE_LOG(LogTemp, Warning, TEXT("[NIF][Skin] Unmapped skin bones on Geo='%s': %s"),
                    *GeoName, *FString::Join(UnmappedNames, TEXT(", ")));
            }
        }
        else if (Skin && !SkinData)
        {
            UE_LOG(LogTemp, Warning, TEXT("[NIF][Skin] Geo='%s' has NiSkinInstance but no NiSkinData."), *GeoName);
        }

        const int32 Base = Ctx.VertexBase;

        for (int i = 0; i < NumVerts; ++i)
        {
            FNifVertex Vtx;

            FVector3f p = ToUE(GeoData->GetVertices()[i]);
            FVector P3 = FVector(p);
            P3 = WorldXf.TransformPosition(P3);
            Vtx.Position = (FVector3f)P3;

            if (!GeoData->GetNormals().empty() && i < (int)GeoData->GetNormals().size())
            {
                FVector3f n = ToUE(GeoData->GetNormals()[i]);
                FVector N3 = FVector(n);
                N3 = WorldXf.TransformVectorNoScale(N3);
                Vtx.Normal = (FVector3f)N3.GetSafeNormal();
            }
            else
            {
                Vtx.Normal = FVector3f::ZeroVector;
            }

            if (UVSetCount > 0 && i < (int)UVSets[0].size())
            {
                Vtx.UV = ToUE_NoFlipV(UVSets[0][i]);
            }
            else
            {
                Vtx.UV = FVector2f(0, 0);
            }

            if (PerVertInfl.Num() > 0 && i < PerVertInfl.Num() && PerVertInfl[i].Num() > 0)
            {
                Vtx.Influences = MoveTemp(PerVertInfl[i]);
            }
            else
            {
                FNifVertexInfluence Infl; Infl.BoneIndex = (Ctx.PrimaryRootIndex != INDEX_NONE) ? Ctx.PrimaryRootIndex : 0;
                Infl.Weight = 1.0f;
                Vtx.Influences.Add(Infl);
            }

            Ctx.Mesh.Vertices.Add(Vtx);
        }

        int32 MatIndex = 0;
        {
            FString MatName = TEXT("NifMat");
            const std::vector<NiPropertyRef> props = Geo->GetProperties();
            for (const NiPropertyRef& p : props)
            {
                if (NiMaterialPropertyRef mp = DynamicCast<NiMaterialProperty>(p))
                {
                    if (!mp->GetName().empty())
                    {
                        MatName = UTF8_TO_TCHAR(mp->GetName().c_str());
                        break;
                    }
                }
            }

            const FString DiffusePathForMat = GetDiffuseTexturePath(Geo);

            int32 Found = INDEX_NONE;
            for (int32 i = 0; i < Ctx.Mesh.Materials.Num(); ++i)
            {
                if (Ctx.Mesh.Materials[i].Name == MatName) { Found = i; break; }
            }

            if (Found == INDEX_NONE)
            {
                FNifMaterial NewMat;
                NewMat.Name = MatName;
                NewMat.DiffuseTexturePath = DiffusePathForMat;
                Ctx.Mesh.Materials.Add(NewMat);
                MatIndex = Ctx.Mesh.Materials.Num() - 1;
            }
            else
            {
                MatIndex = Found;
                FNifMaterial& Existing = Ctx.Mesh.Materials[Found];
                if (Existing.DiffuseTexturePath.IsEmpty() && !DiffusePathForMat.IsEmpty())
                {
                    Existing.DiffuseTexturePath = DiffusePathForMat;
                }
                else if (!DiffusePathForMat.IsEmpty() && !Existing.DiffuseTexturePath.IsEmpty() &&
                    !Existing.DiffuseTexturePath.Equals(DiffusePathForMat, ESearchCase::IgnoreCase))
                {
                    UE_LOG(LogTemp, Warning,
                        TEXT("[NIF] Material '%s' appears with different diffuse textures: '%s' vs '%s'"),
                        *MatName, *Existing.DiffuseTexturePath, *DiffusePathForMat);
                }
            }
        }

        // Emit faces
        for (int32 i = 0; i < Indices.Num(); i += 3)
        {
            FNifFace F;
            F.Indices[0] = Base + (int32)Indices[i + 0];
            F.Indices[1] = Base + (int32)Indices[i + 1];
            F.Indices[2] = Base + (int32)Indices[i + 2];
            F.MaterialIndex = MatIndex;
            Ctx.Mesh.Faces.Add(F);
        }

        Ctx.VertexBase += NumVerts;
    }

    // ------------- collection traversal (no appending here) -------------
    static void TraverseCollect(const NiAVObjectRef& Obj, const FTransform& ParentXf, FTraversalCtx& Ctx, TArray<FGeoCand>& OutCands)
    {
        if (!Obj) return;

        if (NiLODNodeRef LOD = DynamicCast<NiLODNode>(Obj))
        {
            const auto& children = LOD->GetChildren();
            if (!children.empty())
            {
                if (Ctx.RequestedLOD >= 0)
                {
                    if (Ctx.RequestedLOD < (int32)children.size())
                    {
                        TraverseCollect(children[Ctx.RequestedLOD], ParentXf, Ctx, OutCands);
                    }
                }
                else
                {
                    int32 BestIdx = -1;
                    int32 BestTris = -1;
                    for (int32 i = 0; i < (int32)children.size(); ++i)
                    {
                        const int32 TriCount = GetTriangleCount(children[i]);
                        if (TriCount > BestTris)
                        {
                            BestTris = TriCount;
                            BestIdx = i;
                        }
                    }
                    if (BestIdx < 0) BestIdx = 0;
                    TraverseCollect(children[BestIdx], ParentXf, Ctx, OutCands);
                }
            }
            return;
        }

        FTransform LocalXf = LocalToFTransform(Obj);
        FTransform WorldXf = LocalXf * ParentXf;

        if (NiGeometryRef Geo = DynamicCast<NiGeometry>(Obj))
        {
            const FString GeoName = UTF8_TO_TCHAR(Geo->GetName().c_str());
            const FString NameLower = GeoName.ToLower();

            FGeoCand C;
            C.Geo = Geo;
            C.WorldXf = WorldXf;
            C.Name = GeoName;
            C.BaseKey = BaseKeyFromName(GeoName);
            C.Tris = GetTriangleCount(Obj);
            C.LodTag = ExtractLODTag(NameLower);
            OutCands.Add(C);
        }

        if (NiNodeRef Node = DynamicCast<NiNode>(Obj))
        {
            const auto& children = Node->GetChildren();
            for (const NiAVObjectRef& c : children)
            {
                if (c) TraverseCollect(c, WorldXf, Ctx, OutCands);
            }
        }
    }

    static int32 GetTriangleCountGeo(const NiGeometryRef& Geo)
    {
        if (!Geo) return 0;
        NiGeometryDataRef GeoData = Geo->GetData();
        if (!GeoData) return 0;
        if (NiTriShapeRef TriShape = DynamicCast<NiTriShape>(Geo))
        {
            if (NiTriShapeDataRef TriData = DynamicCast<NiTriShapeData>(GeoData))
            {
                return (int32)TriData->GetTriangles().size();
            }
        }
        else if (NiTriStripsRef Strips = DynamicCast<NiTriStrips>(Geo))
        {
            if (NiTriStripsDataRef StripsData = DynamicCast<NiTriStripsData>(GeoData))
            {
                return (int32)StripsData->GetTriangles().size();
            }
        }
        return 0;
    }
} // anonymous namespace

static int32 ScanAuthoredLODCount(const std::vector<NiObjectRef>& Roots)
{
    int32 MaxChildren = 1; // at least LOD0
    TArray<NiAVObjectRef> Stack;

    for (const NiObjectRef& RootObj : Roots)
    {
        if (NiAVObjectRef RootAV = DynamicCast<NiAVObject>(RootObj))
        {
            Stack.Add(RootAV);
        }
    }

    while (Stack.Num() > 0)
    {
        NiAVObjectRef Obj = Stack.Pop(false);
        if (!Obj) continue;

        if (NiLODNodeRef LOD = DynamicCast<NiLODNode>(Obj))
        {
            const auto& children = LOD->GetChildren();
            if (!children.empty())
            {
                MaxChildren = FMath::Max<int32>(MaxChildren, (int32)children.size());
                for (const NiAVObjectRef& c : children)
                {
                    if (c) Stack.Add(c);
                }
                continue;
            }
        }

        if (NiNodeRef Node = DynamicCast<NiNode>(Obj))
        {
            const auto& children = Node->GetChildren();
            for (const NiAVObjectRef& c : children)
            {
                if (c) Stack.Add(c);
            }
        }
    }

    return MaxChildren;
}

namespace FNiflibBridge
{
    bool ParseNifFileWithLOD(const FString& Path, int32 RequestedLOD, FNifMeshData& OutMesh, FNifAnimationData& OutAnim)
    {
        UE_LOG(LogTemp, Log, TEXT("ParseNifFile: %s (RequestedLOD=%d)"), *Path, RequestedLOD);

        std::string NativePath = TCHAR_TO_UTF8(*Path);
        NifInfo info;
        vector<NiObjectRef> Roots = ReadNifList(NativePath, &info);
        if (Roots.empty()) {
            UE_LOG(LogTemp, Error, TEXT("[NIF] No root objects in file."));
            return false;
        }

        OutMesh.Bones.Empty();
        OutMesh.Materials.Empty();
        OutMesh.Vertices.Empty();
        OutMesh.Faces.Empty();

        FTraversalCtx Ctx{ OutMesh };
        Ctx.RequestedLOD = RequestedLOD;

        // Phase 1: collect all candidate geos
        TArray<FGeoCand> Cands;
        for (const NiObjectRef& RootObj : Roots)
        {
            if (NiAVObjectRef RootAV = DynamicCast<NiAVObject>(RootObj))
            {
                TraverseCollect(RootAV, FTransform::Identity, Ctx, Cands);
            }
        }

        // Phase 2: choose ONE per base key
        TMap<FString, int32> BestByKey; // key -> index into Cands
        for (int32 i = 0; i < Cands.Num(); ++i)
        {
            const FGeoCand& C = Cands[i];

            int32* Found = BestByKey.Find(C.BaseKey);
            if (!Found)
            {
                BestByKey.Add(C.BaseKey, i);
                continue;
            }

            const FGeoCand& Cur = Cands[*Found];

            // If a LOD was explicitly requested, prefer an exact LOD tag match
            if (Ctx.RequestedLOD >= 0)
            {
                const bool NewMatch = (C.LodTag == Ctx.RequestedLOD);
                const bool CurMatch = (Cur.LodTag == Ctx.RequestedLOD);

                if (NewMatch && !CurMatch)
                {
                    *Found = i;
                    continue;
                }
                if (CurMatch && !NewMatch)
                {
                    continue;
                }
                // else tie-break: fall through to triangle count
            }

            // Otherwise or on tie: prefer higher triangle count
            const int32 CurTris = Cur.Tris;
            const int32 NewTris = (C.Tris > 0) ? C.Tris : GetTriangleCountGeo(C.Geo);
            if (NewTris > CurTris)
            {
                *Found = i;
            }
        }

        // Phase 3: append only the selected geometries
        for (const TPair<FString, int32>& Pair : BestByKey)
        {
            const FGeoCand& C = Cands[Pair.Value];
            AppendGeometryFromGeo(C.Geo, C.WorldXf, Ctx);
        }

        if (OutMesh.Bones.Num() == 0)
        {
            FNifBone Root;
            Root.Name = TEXT("Root");
            Root.ParentIndex = INDEX_NONE;
            Root.BindPose = FTransform::Identity;
            OutMesh.Bones.Add(Root);
        }

        if (OutMesh.Materials.Num() == 0 && OutMesh.Faces.Num() > 0)
        {
            FNifMaterial M; M.Name = TEXT("NifMat");
            OutMesh.Materials.Add(M);
        }

        OutAnim.Duration = 0.0f;

        UE_LOG(LogTemp, Log, TEXT("[NIF] Accumulated: Vertices=%d Faces=%d Materials=%d Bones=%d (PrimaryRoot=%d)"),
            OutMesh.Vertices.Num(), OutMesh.Faces.Num(), OutMesh.Materials.Num(), OutMesh.Bones.Num(), 0);

        for (int32 i = 0; i < OutMesh.Materials.Num(); ++i)
        {
            const auto& M = OutMesh.Materials[i];
            UE_LOG(LogTemp, Log, TEXT("[NIF] Material[%d] '%s' Diffuse='%s'"), i, *M.Name, *M.DiffuseTexturePath);
        }

        return OutMesh.Faces.Num() > 0;
    }

    bool ParseNifFile(const FString& Path, FNifMeshData& OutMesh, FNifAnimationData& OutAnim)
    {
        return ParseNifFileWithLOD(Path, -1, OutMesh, OutAnim);
    }

    int32 GetAuthoredLODCount(const FString& Path)
    {
        std::string NativePath = TCHAR_TO_UTF8(*Path);
        NifInfo info;
        vector<NiObjectRef> Roots = ReadNifList(NativePath, &info);
        if (Roots.empty())
            return 1;
        return ScanAuthoredLODCount(Roots);
    }
}
