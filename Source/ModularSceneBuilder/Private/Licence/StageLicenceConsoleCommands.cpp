// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for StageCraft Pro (Docs/ADR/0034 §17), compiled out of Shipping like the cloud and economy commands.
// They exist so Scripts/Tests/RunLicenceEndToEnd.ps1 can drive the Preferences STAGECRAFT PRO buttons from a scripted run; the
// token itself is never printed.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Economy/StageEconomyTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Licence/StageLicenceSubsystem.h"
#include "ModularSceneBuilder.h"

namespace StageLicenceConsoleCommands
{
	UStageLicenceSubsystem* GetLicence(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UStageLicenceSubsystem>() : nullptr;
	}

	void LogResult(const TCHAR* Command, bool bStarted, const FText& Error)
	{
		UE_LOG(LogStageCraft, Display, TEXT("%s: %s"), Command, bStarted ? TEXT("started.") : *FString::Printf(TEXT("not started: %s"), *Error.ToString()));
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageLicenceSubsystem* Licence = GetLicence(World);
		if (!Licence)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Licence.Status: no game instance."));
			return;
		}
		const FGameplayTag ProTags[] = { StageCraftTags::Entitlement_Pro_DmxUniverses, StageCraftTags::Entitlement_Pro_DmxIn, StageCraftTags::Entitlement_Pro_MidiOsc,
			StageCraftTags::Entitlement_Pro_WebRemote, StageCraftTags::Entitlement_Pro_MvrExport };
		TArray<FString> Granted;
		for (const FGameplayTag& Tag : ProTags)
		{
			if (Licence->HasProEntitlement(Tag))
			{
				Granted.Add(Tag.ToString());
			}
		}
		const FStageSubscriptionInfo& Subscription = Licence->GetSubscription();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Licence.Status: %s; plan '%s'; until %s; entitlements [%s]; subscription %s%s; dev override %s; %s"),
			*UEnum::GetValueAsString(Licence->GetStatus()), *Licence->GetPlan(),
			Licence->GetExpiresUtc() > FDateTime() ? *Licence->GetExpiresUtc().ToIso8601() : TEXT("-"),
			*FString::Join(Granted, TEXT(", ")),
			Subscription.bKnown ? *Subscription.Status : TEXT("unknown"), Subscription.bEntitled ? TEXT(" (entitled)") : TEXT(""),
			UStageLicenceSubsystem::IsDevAssumeProActive() ? TEXT("on") : TEXT("off"), *Licence->GetStatusMessage().ToString());
	}

	void Refresh(const TArray<FString>& Args, UWorld* World)
	{
		UStageLicenceSubsystem* Licence = GetLicence(World);
		FText Error;
		LogResult(TEXT("StageCraft.Licence.Refresh"), Licence && Licence->RequestRefresh(Error), Error);
	}

	void Subscribe(const TArray<FString>& Args, UWorld* World)
	{
		UStageLicenceSubsystem* Licence = GetLicence(World);
		const EStageProPlan Plan = !Args.IsEmpty() && Args[0] == TEXT("Yearly") ? EStageProPlan::Yearly : EStageProPlan::Monthly;
		FText Error;
		LogResult(TEXT("StageCraft.Licence.Subscribe"), Licence && Licence->StartCheckout(Plan, Error), Error);
	}

	void Manage(const TArray<FString>& Args, UWorld* World)
	{
		UStageLicenceSubsystem* Licence = GetLicence(World);
		FText Error;
		LogResult(TEXT("StageCraft.Licence.Manage"), Licence && Licence->OpenManageSubscription(Error), Error);
	}

	void Release(const TArray<FString>& Args, UWorld* World)
	{
		UStageLicenceSubsystem* Licence = GetLicence(World);
		FText Error;
		LogResult(TEXT("StageCraft.Licence.Release"), Licence && Licence->ReleaseThisMachine(Error), Error);
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Licence.Status"), TEXT("Logs the StageCraft Pro licence on this computer and the account's subscription."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs RefreshCommand(TEXT("StageCraft.Licence.Refresh"), TEXT("Asks StageCraft Cloud for a new licence for this computer, like REFRESH."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Refresh));
	FAutoConsoleCommandWithWorldAndArgs SubscribeCommand(TEXT("StageCraft.Licence.Subscribe"), TEXT("Monthly | Yearly: opens the Pro checkout (the address is logged with StageCraft.Cloud.LogBrowserUrls 1)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Subscribe));
	FAutoConsoleCommandWithWorldAndArgs ManageCommand(TEXT("StageCraft.Licence.Manage"), TEXT("Opens the subscription page (billing portal), like MANAGE."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Manage));
	FAutoConsoleCommandWithWorldAndArgs ReleaseCommand(TEXT("StageCraft.Licence.Release"), TEXT("Releases this computer's place on the account, like RELEASE THIS PC."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Release));
}

#endif
