// Copyright DamenTools internal. Crane Control plugin.

using UnrealBuildTool;

public class CraneControl : ModuleRules
{
	public CraneControl(ReadOnlyTargetRules Target) : base(Target)
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

		// Public: FDynamicDataTableRowHandle is in MeshListCraneLibrary.h.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"DynamicDatatable"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// GetClassDefaultStaticMesh: a catalog entry's mesh = the one its ghost shows.
			"PreciseTransform"
		});
	}
}
