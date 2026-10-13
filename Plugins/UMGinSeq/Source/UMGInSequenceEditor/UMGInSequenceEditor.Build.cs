//  Copyright WASD Studio 2025 All Rights Reserved.

using UnrealBuildTool;

public class UMGInSequenceEditor : ModuleRules
{
	public UMGInSequenceEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[]             {
				"Core",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"UMG",
				"UnrealEd",
				"PropertyEditor",
				"MovieScene",
				"Sequencer"
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[]             {
				"InputCore",
				"EditorFramework",
				"Kismet",
				"ApplicationCore",
				"Projects"
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
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
				"UMGInSequence",	
				"PropertyEditor"
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
