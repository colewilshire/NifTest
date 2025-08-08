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
    try
    {
        root = Niflib::ReadNifTree(NifPath);
        if (!root)
        {
            UE_LOG(LogTemp, Error, TEXT("Niflib: Failed to load NIF file (root is null)!"));
            return;
        }
    }
    catch (const std::exception& ex)
    {
        UE_LOG(LogTemp, Error, TEXT("Niflib: Exception loading NIF: %s"), *FString(ex.what()));
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

    // Choose a nonzero spawn location for testing
    FVector SpawnLocation(0.0f, 0.0f, 0.0f);
    FActorSpawnParameters SpawnParams;
    AActor* MeshActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator, SpawnParams);

    if (!MeshActor)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to spawn mesh actor!"));
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("Spawned Actor Location: %s"), *MeshActor->GetActorLocation().ToString());

    // Duplicate positions so we can optionally add an offset
    TArray<FVector> Positions = PositionsIn;

    // Optional: Add a test offset to all vertices (comment out if not needed)
    FVector TestOffset(0.0f, 0.0f, 20.0f); // Add 2000 units Y and Z to mesh geometry
    for (FVector& V : Positions)
    {
        V += TestOffset;
    }

    // Print mesh bounds after offset
    if (Positions.Num() > 0)
    {
        FVector Min = Positions[0], Max = Positions[0];
        for (const FVector& V : Positions)
        {
            Min = Min.ComponentMin(V);
            Max = Max.ComponentMax(V);
        }
        UE_LOG(LogTemp, Warning, TEXT("Mesh Bounds: Min(%f, %f, %f) Max(%f, %f, %f)"), Min.X, Min.Y, Min.Z, Max.X, Max.Y, Max.Z);
    }

    // Create and register the mesh component, set as root
    UProceduralMeshComponent* PMC = NewObject<UProceduralMeshComponent>(MeshActor);
    MeshActor->SetRootComponent(PMC);
    PMC->RegisterComponent();

    // **Set world location of mesh root to the desired spawn location**
    PMC->SetWorldLocation(SpawnLocation);

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

    // Log the PMC's location info
    UE_LOG(LogTemp, Warning, TEXT("PMC RelativeLocation: %s"), *PMC->GetRelativeLocation().ToString());
    UE_LOG(LogTemp, Warning, TEXT("PMC WorldLocation: %s"), *PMC->GetComponentLocation().ToString());
}
