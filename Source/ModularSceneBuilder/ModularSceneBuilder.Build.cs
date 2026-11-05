// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
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
		// HTTP: the AI assistant's optional online model (Claude Messages API, ADR 0016).
		PrivateDependencyModuleNames.AddRange(new string[] { "ProceduralMeshComponent", "Slate", "SlateCore", "ApplicationCore", "Json", "DeveloperSettings", "RenderCore", "RHI", "ImageCore", "HTTP" });

		// MoviePlayer: StageCraft's loader as the engine's loading screen while the map loads (ADR 0024 §1).
		PrivateDependencyModuleNames.Add("MoviePlayer");

		// ADR 0017: AudioCaptureCore records voice prompts (the AudioCapture plugin registers the platform microphone backend);
		// ImageWrapper decodes the PNG / JPEG drawings imported into Sketch to Stage.
		PrivateDependencyModuleNames.AddRange(new string[] { "AudioCaptureCore", "ImageWrapper" });

		// ADR 0019: the Art-Net / sACN input is a small receiver of our own on the engine's UDP sockets (FUdpSocketReceiver, FIPv4Address).
		PrivateDependencyModuleNames.AddRange(new string[] { "Sockets", "Networking" });

		// ADR 0022: the DCC bridge exports saved scenes as glTF binary with the engine's runtime glTF exporter (GLTFExporter plugin),
		// after turning procedural parts into transient static meshes (MeshDescription, StaticMeshDescription). The bridge's
		// localhost TCP server uses Sockets/Networking above.
		PrivateDependencyModuleNames.AddRange(new string[] { "GLTFExporter", "MeshDescription", "StaticMeshDescription" });

		// The UI font's Armenian sub-font (Noto Sans Armenian, SIL OFL 1.1) is read from these files at runtime, so they ship with the module.
		RuntimeDependencies.Add(Path.Combine(ModuleDirectory, "Resources", "Fonts", "NotoSansArmenian-Regular.ttf"), StagedFileType.UFS);
		RuntimeDependencies.Add(Path.Combine(ModuleDirectory, "Resources", "Fonts", "NotoSansArmenian-Bold.ttf"), StagedFileType.UFS);
		RuntimeDependencies.Add(Path.Combine(ModuleDirectory, "Resources", "Fonts", "OFL.txt"), StagedFileType.UFS);

		// Runtime model import (ADR 0006): Interchange translates .glb/.gltf/.obj (and .fbx in builds with editor code) and builds
		// static meshes at runtime. InterchangePipelines is the Interchange plugin's generic pipeline, configured in code.
		PrivateDependencyModuleNames.AddRange(new string[] { "InterchangeCore", "InterchangeEngine", "InterchangePipelines" });

		// The OS file dialog (IDesktopPlatform) lives in a Developer module, which the engine licence does not allow in shipped
		// builds. It is linked where developer tools are built. Windows builds without it (Shipping) call the same Windows dialog
		// through COM (ADR 0007 §7); on other platforms "Import Model..." is hidden and drag and drop remains.
		if (Target.bBuildDeveloperTools)
		{
			PrivateDependencyModuleNames.Add("DesktopPlatform");
			PrivateDefinitions.Add("WITH_STAGECRAFT_FILE_DIALOG=1");
			PrivateDefinitions.Add("WITH_STAGECRAFT_NATIVE_FILE_DIALOG=0");
		}
		else
		{
			PrivateDefinitions.Add("WITH_STAGECRAFT_FILE_DIALOG=0");
			bool bNativeDialog = Target.Platform == UnrealTargetPlatform.Win64;
			PrivateDefinitions.Add("WITH_STAGECRAFT_NATIVE_FILE_DIALOG=" + (bNativeDialog ? "1" : "0"));
			if (bNativeDialog)
			{
				// CoCreateInstance / CoTaskMemFree, and SHCreateItemFromParsingName.
				PublicSystemLibraries.AddRange(new string[] { "ole32.lib", "shell32.lib" });
			}
		}

		// The credential vault encrypts third-party API keys per Windows user with DPAPI (CryptProtectData; ADR 0023 §2).
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("crypt32.lib");
		}

		// ADR 0029: audio-reactive lighting. SignalProcessing gives the FFT. Music files are decoded by Windows Media Foundation
		// directly (source reader; Phase G0 showed the engine's media players refuse MP3/FLAC or cannot place a beat) and the PC's
		// output is captured with WASAPI loopback; both are Windows system libraries, no plugin.
		// AudioExtensions: the file is played by a USoundWaveProcedural subclass (IAudioProxyDataFactory).
		PrivateDependencyModuleNames.AddRange(new string[] { "SignalProcessing", "AudioExtensions" });
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] { "ole32.lib", "mfplat.lib", "mfreadwrite.lib", "mfuuid.lib" });
		}

		// The DMX Engine plugin is deliberately not a dependency: StageCraft reads Art-Net and sACN itself (ADR 0019 section 3).

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
