using UnrealBuildTool;
using System.IO;

public class ExamplePlugin : ModuleRules
{
    public ExamplePlugin(ReadOnlyTargetRules Target) : base(Target)
    {
        // Use explicit/shared PCHs for faster compile times.
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Public dependencies (exposed to modules that include your plugin).
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

        // Private dependencies (used only within this plugin).
        PrivateDependencyModuleNames.AddRange(new string[] { });

        // Enable exceptions if needed (optional)
        bEnableExceptions = true;
    }
}