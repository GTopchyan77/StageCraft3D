// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ModularSceneBuilder : ModuleRules
{
	public ModularSceneBuilder(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "GameplayTags", "UMG" });

		PrivateDependencyModuleNames.AddRange(new string[] { "ProceduralMeshComponent", "Slate", "SlateCore" });

		// DMX Engine (Art-Net/sACN) is deliberately not a dependency of this module. A separate
		// module that depends on "DMXProtocol" implements UStageDMXBridge (see STATE.md #9).

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
