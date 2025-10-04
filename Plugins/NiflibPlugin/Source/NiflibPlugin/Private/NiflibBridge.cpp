#include "NiflibBridge.h"
#include "Logging/LogMacros.h"
// #include <niflib.h>          // add actual Niflib includes when you wire parsing
// #include <obj/NiTriShape.h> ...

namespace FNiflibBridge
{
    bool ParseNifFile(const FString& Path, FNifMeshData& OutMesh, FNifAnimationData& OutAnim)
    {
        UE_LOG(LogTemp, Log, TEXT("ParseNifFile: %s"), *Path);

        // TEMP: return a tiny triangle bound to one root bone so the pipeline runs
        OutMesh.Bones.Add({TEXT("Root"), INDEX_NONE, FTransform::Identity});
        OutMesh.Materials.Add({TEXT("Default"), TEXT("")});

        FNifVertex a; a.Position={0,0,0};   a.UV={0,0}; a.Influences.Add({0,1});
        FNifVertex b; b.Position={100,0,0}; b.UV={1,0}; b.Influences.Add({0,1});
        FNifVertex c; c.Position={0,100,0}; c.UV={0,1}; c.Influences.Add({0,1});
        OutMesh.Vertices.Append({a,b,c});

        FNifFace f; f.Indices[0]=0; f.Indices[1]=1; f.Indices[2]=2; f.MaterialIndex=0;
        OutMesh.Faces.Add(f);

        OutAnim.Duration = 0.f; // no animation yet
        return true;
    }
}
