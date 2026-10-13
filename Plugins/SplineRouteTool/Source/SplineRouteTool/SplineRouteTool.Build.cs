// Copyright DamenTools internal. Spline Route Tool plugin.

using UnrealBuildTool;

public class SplineRouteTool : ModuleRules
{
	public SplineRouteTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"Water",
			"SkyWeatherControls",
			"NGGVideoRecorder"
		});
	}
}
