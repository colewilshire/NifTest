#include "NiflibBridge.h"
#include "Logging/LogMacros.h"

// --- Niflib headers ---
#include <niflib.h>
#include <obj/NiObject.h>
#include <obj/NiNode.h>
#include <obj/NiAVObject.h>
#include <obj/NiGeometry.h>
#include <obj/NiGeometryData.h>
#include <obj/NiTriShape.h>
#include <obj/NiTriShapeData.h>
#include <obj/NiTriStrips.h>
#include <obj/NiTriStripsData.h>
#include <obj/NiMaterialProperty.h>
#include <gen/enums.h>
#include <obj/NiSkinInstance.h>
#include <obj/NiSkinData.h>
#include <obj/NiSkinPartition.h>

using namespace Niflib;

namespace
{
    // --------- small helpers ---------
    static FORCEINLINE FVector3f ToUE(const Vector3& v)
    {
        return FVector3f((float)v.x, (float)v.y, (float)v.z);
    }
    static FORCEINLINE FVector2f ToUE(const TexCoord& uv)
    {
        return FVector2f((float)uv.u, (float)uv.v);
    }

    static FTransform LocalToFTransform(const NiAVObjectRef& Obj)
    {
        const Vector3 T = Obj->GetLocalTranslation();
        const Matrix33 R = Obj->GetLocalRotation();
        const float S = Obj->GetLocalScale();

        const FMatrix Rot(
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

    // ---------- traversal context ----------
    struct FTraversalCtx
    {
        FNifMeshData& Mesh;
        int32 VertexBase = 0;

        // Map by **pointer identity** -> UE bone index (avoids hashing Ref<>)
        TMap<const void*, int32> NodeToBoneIndex;
        bool bBonesBuilt = false;

        // Index of the single root we designate in case the NIF has multiple top-level chains
        int32 PrimaryRootIndex = INDEX_NONE;
    };

    // Recursively ensure a bone exists for Node, creating parent first.
    // Returns the UE bone index for Node.
    static int32 EnsureBoneForNode(const NiAVObjectRef& Node, FTraversalCtx& Ctx)
    {
        if (!Node) return INDEX_NONE;

        const void* Key = Node.operator->();
        if (int32* Found = Ctx.NodeToBoneIndex.Find(Key))
        {
            return *Found;
        }

        // Parent-first: create (or get) the parent bone index
        int32 ParentIdx = INDEX_NONE;
        if (NiNodeRef P = Node->GetParent())
        {
            NiAVObjectRef PAsAV = DynamicCast<NiAVObject>(P);
            ParentIdx = EnsureBoneForNode(PAsAV, Ctx);
        }
        else
        {
            // No parent: enforce a single-root skeleton.
            if (Ctx.PrimaryRootIndex != INDEX_NONE)
            {
                // Attach additional roots under the primary root to satisfy bOnlyOneRootAllowed
                ParentIdx = Ctx.PrimaryRootIndex;
            }
        }

        // Create this bone
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

        // If this is the first-ever bone with INDEX_NONE parent, mark it as the designated root
        if (ParentIdx == INDEX_NONE && Ctx.PrimaryRootIndex == INDEX_NONE)
        {
            Ctx.PrimaryRootIndex = NewIdx;
        }

        Ctx.NodeToBoneIndex.Add(Key, NewIdx);
        return NewIdx;
    }

    // Collect bones from a skin: ensure all skin bones (and their parents) exist, parent-first
    static void BuildBonesFromSkin(const NiSkinInstanceRef& Skin, FTraversalCtx& Ctx)
    {
        if (!Skin) return;

        const std::vector<NiNodeRef> BoneNodes = Skin->GetBones();
        if (BoneNodes.empty())
        {
            return;
        }

        // If the skin has an explicit skeleton root, ensure it's created first so it becomes the primary root.
        if (NiNodeRef SkelRoot = Skin->GetSkeletonRoot())
        {
            NiAVObjectRef SkelRootAsAV = DynamicCast<NiAVObject>(SkelRoot);
            if (SkelRootAsAV)
            {
                EnsureBoneForNode(SkelRootAsAV, Ctx);
            }
        }

        // Ensure every bone (and its parent chain) exists
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

    // Append one geometry's vertices, faces, and influences (if skinned)
    static void AppendGeometry(const NiAVObjectRef& Obj, const FTransform& WorldXf, FTraversalCtx& Ctx)
    {
        NiGeometryRef Geo = DynamicCast<NiGeometry>(Obj);
        if (!Geo) return;

        NiGeometryDataRef GeoData = Geo->GetData();
        if (!GeoData) return;

        const int NumVerts = GeoData->GetVertexCount();
        if (NumVerts <= 0) return;

        // Build triangle index list
        TArray<uint32> Indices;
        if (NiTriShapeRef TriShape = DynamicCast<NiTriShape>(Obj))
        {
            if (NiTriShapeDataRef TriData = DynamicCast<NiTriShapeData>(GeoData))
            {
                const std::vector<Triangle> tris = TriData->GetTriangles();
                Indices.Reserve((int32)tris.size() * 3);
                for (const Triangle& t : tris)
                {
                    Indices.Add(t.v1); Indices.Add(t.v2); Indices.Add(t.v3);
                }
            }
        }
        else if (NiTriStripsRef Strips = DynamicCast<NiTriStrips>(Obj))
        {
            if (NiTriStripsDataRef StripsData = DynamicCast<NiTriStripsData>(GeoData))
            {
                const std::vector<Triangle> tris = StripsData->GetTriangles();
                Indices.Reserve((int32)tris.size() * 3);
                for (const Triangle& t : tris)
                {
                    Indices.Add(t.v1); Indices.Add(t.v2); Indices.Add(t.v3);
                }
            }
        }

        if (Indices.Num() == 0) return;

        // Positions & UVs
        const std::vector<Vector3> Verts = GeoData->GetVertices();
        const int UVSetCount = GeoData->GetUVSetCount();
        std::vector<TexCoord> UV0;
        if (UVSetCount > 0)
        {
            UV0 = GeoData->GetUVSet(0);
        }

        // Skinning
        NiSkinInstanceRef Skin = Geo->GetSkinInstance();
        NiSkinDataRef SkinData = Skin ? Skin->GetSkinData() : NiSkinDataRef();

        if (Skin && !Ctx.bBonesBuilt)
        {
            BuildBonesFromSkin(Skin, Ctx);
        }

        TArray<TArray<FNifVertexInfluence>> PerVertInfl;
        if (Skin && SkinData && Ctx.NodeToBoneIndex.Num() > 0)
        {
            PerVertInfl.SetNum(NumVerts);
            const std::vector<NiNodeRef> BoneNodes = Skin->GetBones();

            for (unsigned int boneIdx = 0; boneIdx < BoneNodes.size(); ++boneIdx)
            {
                const NiNodeRef BoneNode = BoneNodes[boneIdx];
                if (!BoneNode) continue;

                const void* Key = BoneNode.operator->();
                int32* FoundIndex = Ctx.NodeToBoneIndex.Find(Key);
                if (!FoundIndex) continue;
                const int32 UEBoneIndex = *FoundIndex;

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
                    }
                }
            }
        }

        // Emit vertices
        const int32 Base = Ctx.VertexBase;
        for (int i = 0; i < NumVerts; ++i)
        {
            FNifVertex Vtx;

            FVector3f p = ToUE(Verts[i]);
            FVector P3 = FVector(p);
            P3 = WorldXf.TransformPosition(P3);
            Vtx.Position = (FVector3f)P3;

            if (!UV0.empty() && i < (int)UV0.size())
                Vtx.UV = ToUE(UV0[i]);
            else
                Vtx.UV = FVector2f(0, 0);

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

        // Resolve (or create) a material index for this geom
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

            int32 Found = INDEX_NONE;
            for (int32 i = 0; i < Ctx.Mesh.Materials.Num(); ++i)
            {
                if (Ctx.Mesh.Materials[i].Name == MatName) { Found = i; break; }
            }
            if (Found == INDEX_NONE)
            {
                FNifMaterial NewMat; NewMat.Name = MatName;
                Ctx.Mesh.Materials.Add(NewMat);
                MatIndex = Ctx.Mesh.Materials.Num() - 1;
            }
            else MatIndex = Found;
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

    // Depth-first traversal
    static void Traverse(const NiAVObjectRef& Obj, const FTransform& ParentXf, FTraversalCtx& Ctx)
    {
        if (!Obj) return;
        FTransform LocalXf = LocalToFTransform(Obj);
        FTransform WorldXf = LocalXf * ParentXf;

        AppendGeometry(Obj, WorldXf, Ctx);

        if (NiNodeRef Node = DynamicCast<NiNode>(Obj))
        {
            const auto& children = Node->GetChildren();
            for (const NiAVObjectRef& c : children)
            {
                if (c) Traverse(c, WorldXf, Ctx);
            }
        }
    }
}

namespace FNiflibBridge
{
    bool ParseNifFile(const FString& Path, FNifMeshData& OutMesh, FNifAnimationData& OutAnim)
    {
        UE_LOG(LogTemp, Log, TEXT("ParseNifFile: %s"), *Path);

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

        for (const NiObjectRef& RootObj : Roots)
        {
            if (NiAVObjectRef RootAV = DynamicCast<NiAVObject>(RootObj))
            {
                Traverse(RootAV, FTransform::Identity, Ctx);
            }
        }

        if (OutMesh.Bones.Num() == 0)
        {
            // Fallback single root if no skin/bones detected
            FNifBone Root;
            Root.Name = TEXT("Root");
            Root.ParentIndex = INDEX_NONE;
            Root.BindPose = FTransform::Identity;
            OutMesh.Bones.Add(Root);
            Ctx.PrimaryRootIndex = 0;
        }

        if (OutMesh.Materials.Num() == 0 && OutMesh.Faces.Num() > 0)
        {
            FNifMaterial M; M.Name = TEXT("NifMat");
            OutMesh.Materials.Add(M);
        }

        OutAnim.Duration = 0.0f;

        UE_LOG(LogTemp, Log, TEXT("[NIF] Accumulated: Vertices=%d Faces=%d Materials=%d Bones=%d (PrimaryRoot=%d)"),
            OutMesh.Vertices.Num(), OutMesh.Faces.Num(), OutMesh.Materials.Num(), OutMesh.Bones.Num(), Ctx.PrimaryRootIndex);

        return OutMesh.Faces.Num() > 0;
    }
}
