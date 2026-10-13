// Copyright NGG. All Rights Reserved.

using UnrealBuildTool;

public class NGGVideoRecorder : ModuleRules
{
	public NGGVideoRecorder(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"DeveloperSettings",
			"InputCore",
			"Projects",
			"RHI",
			"RenderCore",
			"Renderer",
			"Slate",
			"SlateCore",
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Media Foundation: H.264 encoding + MP4 muxing. Ships with Windows, no redistributables.
			PublicSystemLibraries.AddRange(new string[]
			{
				"mfplat.lib",
				"mfreadwrite.lib",
				"mfuuid.lib",
				"ole32.lib",
			});
		}
	}
}
