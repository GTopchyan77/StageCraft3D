// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for scene files and still renders (Docs/ADR/0004-selection-scenes-and-rendering.md).
// They call the same controller requests as the File and Render menus, so saving, loading and rendering can be verified
// from logs without a mouse. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Render/StageRenderSubsystem.h"
#include "Scene/StageSceneComponent.h"
#include "Subsystems/StageSessionSubsystem.h"

namespace StageSceneCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Scene / Render: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	/** Scene names may contain spaces; the console splits arguments on them. */
	FString JoinArgs(const TArray<FString>& Args)
	{
		return FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(World);
		if (!Controller || !Session)
		{
			return;
		}
		const UStageSceneComponent* Scenes = Controller->GetSceneFiles();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Scene.Status: scene \"%s\", %s, %d items, %s; saved scenes: [%s] in %s"),
			*Session->GetSceneName(), Session->IsDirty() ? TEXT("unsaved changes") : TEXT("clean"), Session->GetStats().PlacedItems,
			Scenes->IsBusy() ? TEXT("busy") : TEXT("idle"), *FString::Join(Scenes->GetSceneNames(), TEXT(", ")), *Scenes->GetScenesDirectory());
	}

	void Save(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Scene.Save: %s"), *UEnum::GetValueAsString(Controller->RequestSaveScene(JoinArgs(Args))));
		}
	}

	void Load(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Scene.Load: %s"), *UEnum::GetValueAsString(Controller->RequestLoadScene(JoinArgs(Args))));
		}
	}

	void Delete(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Scene.Delete: %s"), *UEnum::GetValueAsString(Controller->RequestDeleteScene(JoinArgs(Args))));
		}
	}

	/** Render [4K|HD|Square] [Off|FXAA|Temporal|Super] [Clean|Standard|Cinematic] [wm|nowm]: arguments change the saved settings first. */
	void Render(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		UStageRenderSubsystem* RenderSubsystem = UWorld::GetSubsystem<UStageRenderSubsystem>(World);
		if (!Controller || !RenderSubsystem)
		{
			return;
		}

		FStageRenderSettings Settings = RenderSubsystem->GetSettings();
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("4K"), ESearchCase::IgnoreCase)) { Settings.Resolution = EStageRenderResolution::UltraHD4K; }
			else if (Arg.Equals(TEXT("HD"), ESearchCase::IgnoreCase)) { Settings.Resolution = EStageRenderResolution::FullHD; }
			else if (Arg.Equals(TEXT("Square"), ESearchCase::IgnoreCase)) { Settings.Resolution = EStageRenderResolution::Square; }
			else if (Arg.Equals(TEXT("Off"), ESearchCase::IgnoreCase)) { Settings.AntiAliasing = EStageRenderAntiAliasing::Off; }
			else if (Arg.Equals(TEXT("FXAA"), ESearchCase::IgnoreCase)) { Settings.AntiAliasing = EStageRenderAntiAliasing::FXAA; }
			else if (Arg.Equals(TEXT("Temporal"), ESearchCase::IgnoreCase)) { Settings.AntiAliasing = EStageRenderAntiAliasing::Temporal; }
			else if (Arg.Equals(TEXT("Super"), ESearchCase::IgnoreCase)) { Settings.AntiAliasing = EStageRenderAntiAliasing::Supersampled; }
			else if (Arg.Equals(TEXT("Clean"), ESearchCase::IgnoreCase)) { Settings.PostProcess = EStageRenderPostProcess::Clean; }
			else if (Arg.Equals(TEXT("Standard"), ESearchCase::IgnoreCase)) { Settings.PostProcess = EStageRenderPostProcess::Standard; }
			else if (Arg.Equals(TEXT("Cinematic"), ESearchCase::IgnoreCase)) { Settings.PostProcess = EStageRenderPostProcess::Cinematic; }
			else if (Arg.Equals(TEXT("wm"), ESearchCase::IgnoreCase)) { Settings.bWatermark = true; }
			else if (Arg.Equals(TEXT("nowm"), ESearchCase::IgnoreCase)) { Settings.bWatermark = false; }
			else
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Render: unknown argument %s."), *Arg);
				return;
			}
		}
		RenderSubsystem->SetSettings(Settings);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Render: %s"), Controller->RequestRender() ? TEXT("started") : TEXT("refused"));
	}

	void RenderStatus(const TArray<FString>& Args, UWorld* World)
	{
		const UStageRenderSubsystem* RenderSubsystem = UWorld::GetSubsystem<UStageRenderSubsystem>(World);
		if (!RenderSubsystem)
		{
			return;
		}
		const FStageRenderResult& Last = RenderSubsystem->GetLastResult();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Render.Status: %s; last: %s %dx%d in %.2f s, %s"), *UEnum::GetValueAsString(RenderSubsystem->GetState()),
			Last.bSucceeded ? TEXT("ok") : TEXT("failed/none"), Last.Size.X, Last.Size.Y, Last.Seconds, Last.bSucceeded ? *Last.FilePath : *Last.Message.ToString());
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Scene.Status"), TEXT("Logs the current scene name, dirty state, item count and the saved scenes."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SaveCommand(TEXT("StageCraft.Scene.Save"), TEXT("<Name> Same as File > Save Scene As (replaces an existing scene without asking)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Save));
	FAutoConsoleCommandWithWorldAndArgs LoadCommand(TEXT("StageCraft.Scene.Load"), TEXT("<Name> Same as File > Open Scene (discards unsaved changes without asking)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Load));
	FAutoConsoleCommandWithWorldAndArgs DeleteCommand(TEXT("StageCraft.Scene.Delete"), TEXT("<Name> Same as File > Delete Scene."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Delete));
	FAutoConsoleCommandWithWorldAndArgs RenderCommand(TEXT("StageCraft.Render"), TEXT("[4K|HD|Square] [Off|FXAA|Temporal|Super] [Clean|Standard|Cinematic] [wm|nowm] Sets the render settings, then renders like Render > Render Image."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Render));
	FAutoConsoleCommandWithWorldAndArgs RenderStatusCommand(TEXT("StageCraft.Render.Status"), TEXT("Logs the render state and the last result."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RenderStatus));
}

#endif // !UE_BUILD_SHIPPING
