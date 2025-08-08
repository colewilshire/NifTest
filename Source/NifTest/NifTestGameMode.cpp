#include "NifTestGameMode.h"

// Niflib includes
#include "niflib.h"
#include "obj/NiTriShape.h"
#include "obj/NiTriShapeData.h"
#include "obj/NiTriStrips.h"
#include "obj/NiTriStripsData.h"

void ANifTestGameMode::BeginPlay()
{
    Super::BeginPlay();

    // Path to your NIF file (edit as needed)
    std::string NifPath = "E:\\Program Files (x86)\\Steam\\steamapps\\common\\The Guild 2 Renaissance\\Objects\\Animals\\bull.nif";

    Niflib::NiObjectRef root;
    root = Niflib::ReadNifTree(NifPath);
    if (!root)
    {
        return;
    }
    bSpawnedMesh = false;
    TraverseNifNodes(root);
}

void ANifTestGameMode::TraverseNifNodes(Niflib::NiObjectRef node, int depth)
{
    if (!node || bSpawnedMesh) return; // Only spawn one mesh for this test

    // Check for mesh type: NiTriShape
    if (Niflib::NiTriShapeRef triShape = Niflib::DynamicCast<Niflib::NiTriShape>(node))
    {
        Niflib::NiTriShapeDataRef meshData = Niflib::DynamicCast<Niflib::NiTriShapeData>(triShape->GetData());
        if (meshData)
        {
            std::vector<Niflib::Vector3> verts = meshData->GetVertices();
            std::vector<Niflib::Triangle> tris = meshData->GetTriangles();

            TArray<FVector> Positions;
            Positions.Reserve(verts.size());
            for (const auto& v : verts)
                Positions.Add(FVector(v.x, v.y, v.z));

            TArray<int32> Triangles;
            Triangles.Reserve(tris.size() * 3);
            for (const auto& tri : tris)
            {
                Triangles.Add(tri.v1);
                Triangles.Add(tri.v2);
                Triangles.Add(tri.v3);
            }

            TArray<FVector2D> UVs;
            if (meshData->GetUVSetCount() > 0)
            {
                const auto& uv0 = meshData->GetUVSet(0);
                UVs.Reserve(uv0.size());
                for (const auto& uv : uv0)
                    UVs.Add(FVector2D(uv.u, uv.v));
            }
            else
            {
                UVs.Init(FVector2D::ZeroVector, verts.size());
            }

            // Show the mesh in Unreal
            ShowProceduralMesh(Positions, Triangles, UVs);
            bSpawnedMesh = true;
            return;
        }
    }
    // Check for mesh type: NiTriStrips
    else if (Niflib::NiTriStripsRef triStrips = Niflib::DynamicCast<Niflib::NiTriStrips>(node))
    {
        Niflib::NiTriStripsDataRef meshData = Niflib::DynamicCast<Niflib::NiTriStripsData>(triStrips->GetData());
        if (meshData)
        {
            std::vector<Niflib::Vector3> verts = meshData->GetVertices();
            std::vector<Niflib::Triangle> tris = meshData->GetTriangles();

            TArray<FVector> Positions;
            Positions.Reserve(verts.size());
            for (const auto& v : verts)
                Positions.Add(FVector(v.x, v.y, v.z));

            TArray<int32> Triangles;
            Triangles.Reserve(tris.size() * 3);
            for (const auto& tri : tris)
            {
                Triangles.Add(tri.v1);
                Triangles.Add(tri.v2);
                Triangles.Add(tri.v3);
            }

            TArray<FVector2D> UVs;
            if (meshData->GetUVSetCount() > 0)
            {
                const auto& uv0 = meshData->GetUVSet(0);
                UVs.Reserve(uv0.size());
                for (const auto& uv : uv0)
                    UVs.Add(FVector2D(uv.u, uv.v));
            }
            else
            {
                UVs.Init(FVector2D::ZeroVector, verts.size());
            }

            // Show the mesh in Unreal
            ShowProceduralMesh(Positions, Triangles, UVs);
            bSpawnedMesh = true;
            return;
        }
    }

    // If this is a NiNode, recurse into children
    if (Niflib::NiNodeRef niNode = Niflib::DynamicCast<Niflib::NiNode>(node))
    {
        const std::vector<Niflib::NiAVObjectRef>& children = niNode->GetChildren();
        for (const auto& child : children)
        {
            TraverseNifNodes(Niflib::StaticCast<Niflib::NiObject>(child), depth + 1);
            if (bSpawnedMesh) return; // Stop after first mesh
        }
    }
}

void ANifTestGameMode::ShowProceduralMesh(const TArray<FVector>& PositionsIn, const TArray<int32>& Triangles, const TArray<FVector2D>& UVs)
{
    UWorld* World = GetWorld();
    if (!World) return;

    AActor* MeshActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);

    if (!MeshActor)
    {
        return;
    }

    TArray<FVector> Positions = PositionsIn;

    FVector SpawnOffset(0.0f, 0.0f, 50.0f);
    for (FVector& V : Positions)
    {
        V += SpawnOffset;
    }

    UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(MeshActor);
    MeshActor->SetRootComponent(PMC);
    PMC->RegisterComponent();

    PMC->CreateMeshSection_LinearColor(
        0,
        Positions,
        Triangles,
        TArray<FVector>(),        // Normals (empty, optional)
        UVs,
        TArray<FLinearColor>(),   // Vertex Colors (optional)
        TArray<FProcMeshTangent>(), // Tangents (optional)
        true                     // Enable collision
    );
}
