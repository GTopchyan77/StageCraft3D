// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the AI tool seam (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md). They register the
// offline mock tool and give it its test key, so scripted runs can exercise the Services panel and the acquire dispatcher. They
// never take a real key: a key typed on a command line would end up in the console history. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Services/StageAIToolSubsystem.h"
#include "Services/StageMockAIToolProvider.h"

namespace StageAIToolCommands
{
	UStageAIToolSubsystem* GetTools(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageAIToolSubsystem* Tools = GameInstance ? GameInstance->GetSubsystem<UStageAIToolSubsystem>() : nullptr;
		if (!Tools)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.AITools: no game instance (start PIE or -game first)."));
		}
		return Tools;
	}

	void Mock(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageAIToolSubsystem* Tools = GetTools(World))
		{
			Tools->RegisterProviderForDevelopment(UStageMockAIToolProvider::StaticClass());
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.AITools.Mock: the local test tool is available (key prefix \"%s\")."), UStageMockAIToolProvider::AcceptedKeyPrefix);
		}
	}

	void MockKey(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageAIToolSubsystem* Tools = GetTools(World))
		{
			const bool bValid = !(Args.Num() > 0 && Args[0].Equals(TEXT("wrong"), ESearchCase::IgnoreCase));
			const FStageAIToolResultInfo Result = Tools->StoreKey(UStageMockAIToolProvider::ProviderId, bValid ? TEXT("mock-dev") : TEXT("wrong-dev"));
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.AITools.MockKey: %s"), *Result.Message.ToString());
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (const UStageAIToolSubsystem* Tools = GetTools(World))
		{
			const TArray<FStageAIToolInfo> Infos = Tools->GetTools();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.AITools.Status: %d tool(s); key storage %s; %s."), Infos.Num(),
				Tools->IsKeyStorageAvailable() ? TEXT("available") : TEXT("unavailable"), Tools->IsBusy() ? TEXT("busy") : TEXT("idle"));
			for (const FStageAIToolInfo& Info : Infos)
			{
				UE_LOG(LogStageCraft, Display, TEXT("  %s (%s): key %s, capabilities 0x%02x"), *Info.ProviderId.ToString(), *Info.DisplayName.ToString(),
					*StaticEnum<EStageAIToolKeyState>()->GetNameStringByValue(static_cast<int64>(Info.KeyState)), Info.Capabilities);
			}
		}
	}

	FAutoConsoleCommandWithWorldAndArgs MockCommand(TEXT("StageCraft.AITools.Mock"),
		TEXT("Development only: adds the offline local test tool to the AI tools."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Mock));
	FAutoConsoleCommandWithWorldAndArgs MockKeyCommand(TEXT("StageCraft.AITools.MockKey"),
		TEXT("Development only: saves the local test tool's key. 'wrong' saves a key it refuses."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MockKey));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.AITools.Status"),
		TEXT("Development only: lists the AI tools and their key states (never the keys)."), FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
}

#endif // !UE_BUILD_SHIPPING
