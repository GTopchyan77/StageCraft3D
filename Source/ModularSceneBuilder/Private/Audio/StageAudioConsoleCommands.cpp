// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the audio manager (Docs/ADR/0002-placement-and-audio.md). They go through
// UStageAudioSubsystem's public setters, the same path as the Audio menu. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Audio/StageAudioSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"

namespace StageAudioCommands
{
	UStageAudioSubsystem* GetAudio(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageAudioSubsystem* Audio = GameInstance ? GameInstance->GetSubsystem<UStageAudioSubsystem>() : nullptr;
		if (!Audio)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Audio: no audio subsystem in this world."));
		}
		return Audio;
	}

	void LogStatus(const UStageAudioSubsystem& Audio)
	{
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Audio.Status: master %.2f, muted %s"),
			Audio.GetMasterVolume(), Audio.IsMuted() ? TEXT("yes") : TEXT("no"));
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (const UStageAudioSubsystem* Audio = GetAudio(World))
		{
			LogStatus(*Audio);
		}
	}

	void Master(const TArray<FString>& Args, UWorld* World)
	{
		UStageAudioSubsystem* Audio = GetAudio(World);
		float Volume = 0.f;
		if (!Audio || Args.IsEmpty() || !LexTryParseString(Volume, *Args[0]))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Audio.Master <0-1>"));
			return;
		}
		Audio->SetMasterVolume(Volume);
		LogStatus(*Audio);
	}

	void Mute(const TArray<FString>& Args, UWorld* World)
	{
		UStageAudioSubsystem* Audio = GetAudio(World);
		if (!Audio)
		{
			return;
		}

		bool bMute = false;
		if (Args.IsEmpty())
		{
			Audio->ToggleMute();
		}
		else if (LexTryParseString(bMute, *Args[0]))
		{
			Audio->SetMuted(bMute);
		}
		LogStatus(*Audio);
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Audio.Status"), TEXT("Logs master volume and mute."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs MasterCommand(TEXT("StageCraft.Audio.Master"), TEXT("<0-1> Sets the master volume (all sounds)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Master));
	FAutoConsoleCommandWithWorldAndArgs MuteCommand(TEXT("StageCraft.Audio.Mute"), TEXT("[0|1] Mutes/unmutes everything (no argument toggles)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Mute));
}

#endif // !UE_BUILD_SHIPPING
