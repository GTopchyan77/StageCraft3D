// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Remote/StageWebRemoteSubsystem.h"

namespace StageWebRemoteConsoleCommands
{
	AModularPlayerController* FindController(UWorld* World)
	{
		return World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	UStageWebRemoteSubsystem* FindWebRemote(UWorld* World)
	{
		UStageWebRemoteSubsystem* Web = UWorld::GetSubsystem<UStageWebRemoteSubsystem>(World);
		if (!Web)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.WebRemote: no stage in this world."));
		}
		return Web;
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageWebRemoteSubsystem* Web = FindWebRemote(World);
		if (!Web)
		{
			return;
		}
		const FStageWebRemoteStatus& State = Web->GetStatus();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.WebRemote.Status: %s on port %d; %s; %d paired, %d open; pairing %s"),
			State.bListening ? TEXT("listening") : TEXT("off"), Web->GetListeningPort(), *FString::Join(State.Addresses, TEXT(" ")),
			State.PairedDevices, State.ConnectedPages, State.PairingLockedUntil > 0.0 ? TEXT("locked") : TEXT("open"));
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.WebRemote.Status: received %lld, applied %lld, refused %lld, ignored %lld, dropped %lld; last \"%s\"; problem \"%s\"; last refusal \"%s\""),
			State.Received, State.Applied, State.Refused, State.Ignored, State.Dropped, *State.LastInput, *State.LastProblem.ToString(), *State.LastRefusal.ToString());
		if (Args.Contains(TEXT("Pin")))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.WebRemote.Pin=%s"), *State.Pin);
		}
	}

	void Settings(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = FindController(World);
		const UStageWebRemoteSubsystem* Web = FindWebRemote(World);
		if (!Controller || !Web)
		{
			return;
		}
		FStageWebRemoteSettings Settings = Web->GetSettings();
		for (const FString& Arg : Args)
		{
			FString Key;
			FString Value;
			if (Arg.Split(TEXT("="), &Key, &Value))
			{
				if (Key.Equals(TEXT("Port"), ESearchCase::IgnoreCase))
				{
					Settings.Port = FCString::Atoi(*Value);
				}
				continue;
			}
			if (Arg.Equals(TEXT("Off"), ESearchCase::IgnoreCase))
			{
				Settings.Scope = EStageWebRemoteScope::Off;
			}
			else if (Arg.Equals(TEXT("Local"), ESearchCase::IgnoreCase))
			{
				Settings.Scope = EStageWebRemoteScope::ThisComputer;
			}
			else if (Arg.Equals(TEXT("Network"), ESearchCase::IgnoreCase))
			{
				Settings.Scope = EStageWebRemoteScope::Network;
			}
		}
		const FStageEconomyResultInfo Result = Controller->RequestSetWebRemote(Settings);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.WebRemote.Settings: %s %s"), Result.IsSuccess() ? TEXT("applied") : TEXT("refused"), *Result.Message.ToString());
		Status({}, World);
	}

	void NewPin(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = FindController(World);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.WebRemote.NewPin: %s"), Controller && Controller->RequestWebRemoteNewPin() ? TEXT("done") : TEXT("the Web Remote is off"));
	}

	void UnpairAll(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = FindController(World);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.WebRemote.UnpairAll: %s"), Controller && Controller->RequestWebRemoteUnpairAll() ? TEXT("done") : TEXT("the Web Remote is off"));
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.WebRemote.Status"),
		TEXT("Logs the Web Remote: addresses, paired devices, open pages and counters. [Pin] also logs the PIN (development builds only)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SettingsCommand(TEXT("StageCraft.WebRemote.Settings"),
		TEXT("Changes the Web Remote settings through the player's request: Off|Local|Network [Port=n]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Settings));
	FAutoConsoleCommandWithWorldAndArgs NewPinCommand(TEXT("StageCraft.WebRemote.NewPin"),
		TEXT("Makes a new Web Remote PIN through the player's request."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&NewPin));
	FAutoConsoleCommandWithWorldAndArgs UnpairAllCommand(TEXT("StageCraft.WebRemote.UnpairAll"),
		TEXT("Forgets every paired Web Remote device through the player's request."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&UnpairAll));
}

#endif
