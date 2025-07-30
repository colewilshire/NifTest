// Fill out your copyright notice in the Description page of Project Settings.


#include "NifTestGameMode.h"
#include "niflib.h"
#include "obj/NiTriShape.h"
#include "obj/NiTriStrips.h"
#include "obj/NiTriShapeData.h"
#include "obj/NiTriStripsData.h"

void ANifTestGameMode::BeginPlay()
{
    Super::BeginPlay();

    // this->PrintVersion();
    // this->PrintNifObjectType();
    // this->TraverseNifNodes();

    std::string NifPath = "E:\\Program Files (x86)\\Steam\\steamapps\\common\\The Guild 2 Renaissance\\Objects\\Animals\\bull.nif";
    Niflib::NiObjectRef root = Niflib::ReadNifTree(NifPath);
    this->TraverseNifNodes(root);
}

void ANifTestGameMode::PrintVersion()
{
    // Example absolute path - change to your test file
    FString NifPath = TEXT("E:\\Program Files (x86)\\Steam\\steamapps\\common\\The Guild 2 Renaissance\\Objects\\Animals\\bull.nif");
    std::string NifFilePath = TCHAR_TO_UTF8(*NifPath);

    unsigned int Version = Niflib::GetNifVersion(NifFilePath);
    std::string VerString = Niflib::FormatVersionString(Version);

    UE_LOG(LogTemp, Warning, TEXT("NIF Version: %s (raw: %u)"), *FString(VerString.c_str()), Version);

    if (Version == 0xFFFFFFFE) {
        UE_LOG(LogTemp, Error, TEXT("Failed to open NIF file: %s"), *NifPath);
    }
}

void ANifTestGameMode::PrintNifObjectType()
{
    std::string NifPath = "E:\\Program Files (x86)\\Steam\\steamapps\\common\\The Guild 2 Renaissance\\Objects\\Animals\\bull.nif";
    Niflib::NiObjectRef root = Niflib::ReadNifTree(NifPath);
    std::string rootType = root->GetType().GetTypeName();
    UE_LOG(LogTemp, Warning, TEXT("Niflib loaded root object of type: %s"), *FString(rootType.c_str()));
}


// Helper: Recursively traverse nodes, print mesh info
void ANifTestGameMode::TraverseNifNodes(Niflib::NiObjectRef node, int depth)
{
    if (!node) return;

    // Indent for readability
    std::string indent(depth * 2, ' ');

    // Print node type
    std::string typeName = node->GetType().GetTypeName();
    UE_LOG(LogTemp, Warning, TEXT("%sNode type: %s"), *FString(indent.c_str()), *FString(typeName.c_str()));

    // If this is a mesh, get vertex info
    if (Niflib::NiTriShapeRef triShape = Niflib::DynamicCast<Niflib::NiTriShape>(node))
    {
        // Properly cast GetData() to NiTriShapeDataRef
        Niflib::NiTriShapeDataRef meshData = Niflib::DynamicCast<Niflib::NiTriShapeData>(triShape->GetData());
        if (meshData)
        {
            const std::vector<Niflib::Vector3>& verts = meshData->GetVertices();
            const std::vector<Niflib::Triangle>& tris = meshData->GetTriangles();
            UE_LOG(LogTemp, Warning, TEXT("%s  [Mesh] NumVerts: %d NumTris: %d"), *FString(indent.c_str()), (int)verts.size(), (int)tris.size());
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("%s  [Mesh] No mesh data found!"), *FString(indent.c_str()));
        }
    }
    else if (Niflib::NiTriStripsRef triStrips = Niflib::DynamicCast<Niflib::NiTriStrips>(node))
    {
        // Properly cast GetData() to NiTriStripsDataRef
        Niflib::NiTriStripsDataRef meshData = Niflib::DynamicCast<Niflib::NiTriStripsData>(triStrips->GetData());
        if (meshData)
        {
            const std::vector<Niflib::Vector3>& verts = meshData->GetVertices();
            const std::vector<Niflib::Triangle>& tris = meshData->GetTriangles();
            UE_LOG(LogTemp, Warning, TEXT("%s  [Strips] NumVerts: %d NumTris: %d"), *FString(indent.c_str()), (int)verts.size(), (int)tris.size());
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("%s  [Strips] No mesh data found!"), *FString(indent.c_str()));
        }
    }

    // If this is a NiNode, recurse into children
    if (Niflib::NiNodeRef niNode = Niflib::DynamicCast<Niflib::NiNode>(node))
    {
        const std::vector<Niflib::NiAVObjectRef>& children = niNode->GetChildren();
        for (const auto& child : children)
        {
            TraverseNifNodes(Niflib::StaticCast<Niflib::NiObject>(child), depth + 1);
        }
    }
}