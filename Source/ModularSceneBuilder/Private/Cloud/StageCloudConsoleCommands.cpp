// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for StageCraft Cloud (Docs/ADR/0032): compiled out of Shipping. The session token is never printed.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Cloud/StageCloudSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"

namespace StageCloudConsoleCommands
{
	UStageCloudSubsystem* GetCloud(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UStageCloudSubsystem>() : nullptr;
	}

	const TCHAR* StateName(EStageCloudState State)
	{
		switch (State)
		{
		case EStageCloudState::SignedOut: return TEXT("SignedOut");
		case EStageCloudState::SigningIn: return TEXT("SigningIn");
		case EStageCloudState::SignedIn: return TEXT("SignedIn");
		default: return TEXT("Offline");
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageCloudSubsystem* Cloud = GetCloud(World);
		if (!Cloud)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Cloud.Status: no game instance."));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Cloud.Status: %s at '%s'; account '%s' (%s, seller %s); %s"), StateName(Cloud->GetState()), *Cloud->GetBaseUrl(),
			*Cloud->GetAccount().DisplayName, *Cloud->GetAccount().AccountId, *Cloud->GetAccount().SellerStatus, *Cloud->GetStatusMessage().ToString());
	}

	void SignIn(const TArray<FString>& Args, UWorld* World)
	{
		UStageCloudSubsystem* Cloud = GetCloud(World);
		FText Error;
		if (!Cloud || !Cloud->StartSignIn(Error))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Cloud.SignIn: not started: %s"), *Error.ToString());
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Cloud.SignIn: started."));
	}

	void SignOut(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageCloudSubsystem* Cloud = GetCloud(World))
		{
			Cloud->SignOut();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Cloud.SignOut: %s."), StateName(Cloud->GetState()));
		}
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Cloud.Status"), TEXT("Logs the StageCraft Cloud connection and account."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SignInCommand(TEXT("StageCraft.Cloud.SignIn"), TEXT("Starts a device sign-in (the browser opens, or the address is logged with StageCraft.Cloud.LogBrowserUrls 1)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SignIn));
	FAutoConsoleCommandWithWorldAndArgs SignOutCommand(TEXT("StageCraft.Cloud.SignOut"), TEXT("Signs out of StageCraft Cloud and removes the session from the vault."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SignOut));
}

#endif
