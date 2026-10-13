// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DynamicDataTableEditor : ModuleRules
{
	public DynamicDataTableEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicIncludePathModuleNames.Add("LevelEditor");

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"Engine",
                "InputCore",
				"Slate",
				"SlateCore",
                "PropertyEditor",
				"EditorFramework",
				"UnrealEd",
				"DesktopPlatform",
				"DataTableEditor",
				
				"DynamicDatatable",
				"Kismet",
				"GraphEditor",
				"BlueprintGraph",
				"AssetTools",
				"AssetRegistry",
				"ToolMenus",
				"KismetCompiler",
				"Projects",
				"Json"
			}
			);

		DynamicallyLoadedModuleNames.Add("WorkspaceMenuStructure");
	}
}
