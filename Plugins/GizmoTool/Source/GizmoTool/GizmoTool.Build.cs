// Copyright DamenTools internal. Gizmo Tool plugin.

using UnrealBuildTool;

public class GizmoTool : ModuleRules
{
	public GizmoTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"PreciseTransform"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Runtime module (ships in packaged builds): hosts the engine's UCombinedTransformGizmo.
			"InteractiveToolsFramework"
		});
	}
}
