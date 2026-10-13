// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class OverCrowd : ModuleRules
{
	public OverCrowd(ReadOnlyTargetRules Target) : base(Target)
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
				"MeshDescription",
				"GameplayTags",
				"DeveloperSettings", 
				"Niagara", 
				
                 
				// ... add private dependencies that you statically link with here ...	
			}
		);
		
		if(Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("MaterialEditor");
			PrivateDependencyModuleNames.Add("UnrealEd");
			//PrivateDependencyModuleNames.Add("OverCrowdEditor");
		
		}
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
