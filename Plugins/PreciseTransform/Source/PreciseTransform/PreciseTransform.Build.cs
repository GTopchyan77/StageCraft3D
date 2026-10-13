// Copyright DamenTools internal. Precise Transform plugin.

using UnrealBuildTool;

public class PreciseTransform : ModuleRules
{
	public PreciseTransform(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"DynamicDatatable"
		});
	}
}
