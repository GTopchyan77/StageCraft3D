// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ModularSceneBuilder : ModuleRules
{
	public ModularSceneBuilder(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "GameplayTags", "UMG" });

		// ApplicationCore: FDisplayMetrics and DPI scale (monitor work areas). Json: workspace layout files.
		// DeveloperSettings: StageCraft Audio project settings (feedback cue table).
		// RenderCore, RHI: render-thread GPU readback of still renders. ImageCore: resizing supersampled renders.
		PrivateDependencyModuleNames.AddRange(new string[] { "ProceduralMeshComponent", "Slate", "SlateCore", "ApplicationCore", "Json", "DeveloperSettings", "RenderCore", "RHI", "ImageCore" });

		// Runtime model import (ADR 0006): Interchange translates .glb/.gltf/.obj (and .fbx in builds with editor code) and builds
		// static meshes at runtime. InterchangePipelines is the Interchange plugin's generic pipeline, configured in code.
		PrivateDependencyModuleNames.AddRange(new string[] { "InterchangeCore", "InterchangeEngine", "InterchangePipelines" });

		// The OS file dialog (IDesktopPlatform) lives in a Developer module, which the engine licence does not allow in shipped
		// builds. It is linked where developer tools are built; elsewhere "Import Model..." is hidden and drag and drop remains.
		if (Target.bBuildDeveloperTools)
		{
			PrivateDependencyModuleNames.Add("DesktopPlatform");
			PrivateDefinitions.Add("WITH_STAGECRAFT_FILE_DIALOG=1");
		}
		else
		{
			PrivateDefinitions.Add("WITH_STAGECRAFT_FILE_DIALOG=0");
		}

		// DMX Engine (Art-Net/sACN) is deliberately not a dependency of this module. A separate
		// module that depends on "DMXProtocol" implements UStageDMXBridge (see STATE.md #9).

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
