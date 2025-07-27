using UnrealBuildTool;
using System.IO;

public class Niflib : ModuleRules
{
    public Niflib(ReadOnlyTargetRules Target) : base(Target)
    {
        string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "niflib");
        string IncludePath = Path.Combine(ThirdPartyPath, "include");
        string LibPath = Path.Combine(ThirdPartyPath, "lib");

        // Use explicit/shared precompiled headers for faster builds
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Enable RTTI for this module
        bUseRTTI = true;

        // Set public include paths:
        // - Public folder contains Nifly's headers (from include/ and external/).
        PublicIncludePaths.Add(IncludePath);

        // Set private include paths:
        // - Private folder contains the source code (from src/).
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        // Add dependencies that your plugin requires.
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RenderCore" });
        PrivateDependencyModuleNames.AddRange(new string[] { });
        PublicAdditionalLibraries.Add(Path.Combine(LibPath, "niflib.lib"));
        RuntimeDependencies.Add("$(PluginDir)/Binaries/Win64/niflib.dll");

        // Enable exceptions if needed by Niflib.
        bEnableExceptions = true;
    }
}
