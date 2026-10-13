// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

using UnrealBuildTool;

public class OverCrowdEditor : ModuleRules
{
	public OverCrowdEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core", 
				"MaterialEditor",
				"MessageLog", "ScriptableEditorWidgets",      
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"Projects",
				"InputCore",
				"EditorFramework",
				"OverCrowd",
				"RawMesh",
				"ToolMenus",
				"MeshDescription",
				"StaticMeshDescription",
				"UnrealEd",
				"RenderCore",
				"Json",
				"JsonUtilities",
				"StaticMeshDescription",
				"SkeletalMeshUtilitiesCommon",
				"GameplayTags",
				"GameplayTagsEditor",
				"Blutility",
				"UMGEditor",
				"EditorSubsystem",
				"UMG", 
				"ScriptableEditorWidgets",
				"ApplicationCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
