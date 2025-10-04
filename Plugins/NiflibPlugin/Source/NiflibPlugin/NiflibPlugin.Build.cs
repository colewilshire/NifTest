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
        PublicDependencyModuleNames.AddRange(new string[] {
            "Core", "CoreUObject", "Engine", "Projects"
        });

        // Editor-only helpers (safe because module Type=Editor)
        PrivateDependencyModuleNames.AddRange(new string[] {
            "UnrealEd", "AssetTools", "AssetRegistry", "ContentBrowser",
            "EditorFramework", "MeshUtilities", "SkeletalMeshUtilitiesCommon",
            "Slate", "SlateCore", "RenderCore"
        });
    }
}
