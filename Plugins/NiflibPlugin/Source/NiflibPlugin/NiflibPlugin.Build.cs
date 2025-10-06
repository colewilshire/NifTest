// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class NiflibPlugin : ModuleRules
{
    public NiflibPlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDefinitions.Add("NIFLIB_STATIC_LINK=1");
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "inc"));
        PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "lib", "niflib_static.lib"));

        // PublicDependencyModuleNames.AddRange(new string[] { "Core" });
        // PrivateDependencyModuleNames.AddRange(new string[] { "CoreUObject", "Engine", "Slate", "SlateCore" });

        // Core deps
        //PublicDependencyModuleNames.AddRange(new string[] {
        //    "Core", "CoreUObject", "Engine", "Projects"
        //});

        //// Editor-only helpers (safe because module Type=Editor)
        //PrivateDependencyModuleNames.AddRange(new string[] {
        //    "UnrealEd", "AssetTools", "AssetRegistry", "ContentBrowser",
        //    "EditorFramework", "MeshUtilities", "SkeletalMeshUtilitiesCommon",
        //    "Slate", "SlateCore", "RenderCore"
        //});

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine",
            "AssetTools", "MeshUtilities", "RenderCore"
        });

        PrivateDependencyModuleNames.AddRange(new string[] {
            "UnrealEd",                 // factories / editor-only helpers
            "MeshDescription",          // FMeshDescription & CreateVertexInstance_Internal
            "SkeletalMeshDescription",  // FSkeletalMeshAttributes
            "StaticMeshDescription",    // (often pulled in transitively; safe to add)
            "Slate", "SlateCore"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] {
                "EditorFramework", "EditorSubsystem", "Persona"
            });
        }
    }
}
