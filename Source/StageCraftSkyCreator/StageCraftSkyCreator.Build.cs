// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * Sky backend for StageCraft's outdoor environment (Docs/ADR/0009-outdoor-environment.md): drives the Sky Creator plugin's
 * sky actor from FStageEnvironmentSettings. Kept out of the ModularSceneBuilder module so the core never depends on a
 * marketplace plugin; the core finds this backend by class name in DefaultGame.ini.
 */
public class StageCraftSkyCreator : ModuleRules
{
	public StageCraftSkyCreator(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		// SkyCreatorPlugin: ASkyCreator and its settings structs. Niagara: ASkyCreator's header includes Niagara types.
		PrivateDependencyModuleNames.AddRange(new string[] { "ModularSceneBuilder", "SkyCreatorPlugin", "Niagara" });
	}
}
