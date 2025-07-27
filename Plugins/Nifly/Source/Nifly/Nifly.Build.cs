using UnrealBuildTool;
using System.IO;

public class Nifly : ModuleRules
{
    public Nifly(ReadOnlyTargetRules Target) : base(Target)
    {
        // Use explicit/shared precompiled headers for faster builds
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        
        // Enable RTTI for this module
        bUseRTTI = true;
        
        // Set public include paths:
        // - Public folder contains Nifly's headers (from include/ and external/).
        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));
        
        // Set private include paths:
        // - Private folder contains the source code (from src/).
        PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));
        
        // Add dependencies that your plugin requires.
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "RenderCore" });
        PrivateDependencyModuleNames.AddRange(new string[] { });
        
        // Enable exceptions if needed by Nifly.
        bEnableExceptions = true;
    }
}
