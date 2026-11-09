// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tests/StageTestGameInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Bridge/StageBridgeSubsystem.h"
#include "Economy/StageCommerceBackend.h"
#include "Economy/Tests/StageTestCommerceBackend.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageAcquireDispatchSubsystem.h"
#include "Marketplace/StageMarketplaceBackend.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Marketplace/Tests/StageTestMarketplaceBackend.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Services/StageAIToolSubsystem.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "Tickable.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

// --- FStageScopedDefault ---

FStageScopedDefault::FStageScopedDefault(UClass* Class, FName PropertyName, const void* Value, int32 ValueSize)
{
	Property = Class ? FindFProperty<FProperty>(Class, PropertyName) : nullptr;
	if (!Property || Property->GetSize() != ValueSize)
	{
		UE_LOG(LogStageCraft, Error, TEXT("Test harness: %s.%s is missing or is not %d bytes; not changed."), *GetNameSafe(Class), *PropertyName.ToString(), ValueSize);
		Property = nullptr;
		return;
	}
	Default = Class->GetDefaultObject();
	void* Current = Property->ContainerPtrToValuePtr<void>(Default);
	Saved = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
	Property->InitializeValue(Saved);
	Property->CopyCompleteValue(Saved, Current);
	Property->CopyCompleteValue(Current, Value);
}

FStageScopedDefault::~FStageScopedDefault()
{
	if (!Saved)
	{
		return;
	}
	Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Default), Saved);
	Property->DestroyValue(Saved);
	FMemory::Free(Saved);
}

// --- FStageTestGameInstance ---

FStageTestGameInstance::FStageTestGameInstance(const FStageTestGameInstanceOptions& Options)
	: Root(Options.Root.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageGameInstance"), FGuid::NewGuid().ToString())) : Options.Root)
	, bOwnsRoot(Options.Root.IsEmpty())
{
	IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);
	DataRoot = MakeUnique<FStageScopedDataRoot>(Root);

	// Never the user's save slot; never a real store.
	Defaults.Add(FStageScopedDefault::Make(UStageProfileSubsystem::StaticClass(), TEXT("bPersistProfile"), false));
	Defaults.Add(FStageScopedDefault::Make(UStageEconomySubsystem::StaticClass(), TEXT("CommerceBackendClass"),
		TSoftClassPtr<UStageCommerceBackend>(UStageTestCommerceBackend::StaticClass())));
	Defaults.Add(FStageScopedDefault::Make(UStageEconomySubsystem::StaticClass(), TEXT("BackendTimeoutSeconds"), Options.BackendTimeoutSeconds));
	// The folder backend with fetches the test can hold (it behaves as the local backend unless told to).
	Defaults.Add(FStageScopedDefault::Make(UStageMarketplaceSubsystem::StaticClass(), TEXT("MarketplaceBackendClass"),
		TSoftClassPtr<UStageMarketplaceBackend>(UStageTestMarketplaceBackend::StaticClass())));
	Defaults.Add(FStageScopedDefault::Make(UStageMarketplaceSubsystem::StaticClass(), TEXT("FetchTimeoutSeconds"), Options.FetchTimeoutSeconds));
	Defaults.Add(FStageScopedDefault::Make(UStageAcquireDispatchSubsystem::StaticClass(), TEXT("OrderTimeoutSeconds"), Options.OrderTimeoutSeconds));
	Defaults.Add(FStageScopedDefault::Make(UStageAIToolSubsystem::StaticClass(), TEXT("OperationTimeoutSeconds"), Options.ToolTimeoutSeconds));
	Defaults.Add(FStageScopedDefault::Make(UStageBridgeSubsystem::StaticClass(), TEXT("SendTimeoutSeconds"), Options.SendTimeoutSeconds));
	for (const TUniquePtr<FStageScopedDefault>& Default : Defaults)
	{
		// Starting with a real save slot or a real backend is worse than not starting.
		if (!Default->IsValid())
		{
			return;
		}
	}

	UGameInstance* Instance = NewObject<UGameInstance>(GEngine);
	GameInstance.Reset(Instance);
	Instance->InitializeStandalone();
	World = Instance->GetWorld();

	// Ready as a session is by the time the user acts: the item and product catalogs load asynchronously at start.
	constexpr double CatalogTimeoutSeconds = 20.0;
	const UStageItemSubsystem* Items = Subsystem<UStageItemSubsystem>();
	const UStageEconomySubsystem* Economy = Subsystem<UStageEconomySubsystem>();
	if (!Pump([Items, Economy]() { return (!Items || Items->IsCatalogLoaded()) && (!Economy || Economy->IsProductCatalogLoaded()); }, CatalogTimeoutSeconds))
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Test harness: the item or product catalog did not load within %.0f s."), CatalogTimeoutSeconds);
	}
}

FStageTestGameInstance::~FStageTestGameInstance()
{
	Shutdown();
	Defaults.Reset();
	DataRoot.Reset();
	if (bOwnsRoot)
	{
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	}
}

void FStageTestGameInstance::Shutdown()
{
	if (!GameInstance.IsValid())
	{
		return;
	}
	// Answers already queued for the game thread (package imports) are delivered first, as they would be in the app.
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	GameInstance->Shutdown();
	// UGameInstance::Shutdown leaves its world and world context behind; the engine would otherwise keep them for the session.
	if (UWorld* OldWorld = World.Get())
	{
		GEngine->DestroyWorldContext(OldWorld);
		OldWorld->DestroyWorld(/*bInformEngineOfWorld*/ false);
	}
	World.Reset();
	GameInstance.Reset();
	// Anything the subsystems' workers still posted is now addressed to dead objects (weak pointers) and is dropped.
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
}

UStageTestCommerceBackend* FStageTestGameInstance::GetCommerce() const
{
	return GameInstance.IsValid() ? UStageTestCommerceBackend::Find(*GameInstance.Get()) : nullptr;
}

void FStageTestGameInstance::TickOnce(float DeltaSeconds)
{
	// What the engine's frame loop would do for these subsystems: queued game-thread tasks, async package loading (the product
	// catalog), deferred streamable callbacks (a tickable object), and the game instance's timers. FTimerManager and the
	// tickables run at most once per engine frame; the harness is their frame loop here.
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	ProcessAsyncLoading(/*bUseTimeLimit*/ true, /*bUseFullTimeLimit*/ false, /*TimeLimit*/ 0.005);
	++GFrameCounter;
	FTickableGameObject::TickObjects(/*World*/ nullptr, ELevelTick::LEVELTICK_All, /*bIsPaused*/ false, DeltaSeconds);
	if (GameInstance.IsValid())
	{
		GameInstance->GetTimerManager().Tick(DeltaSeconds);
	}
}

bool FStageTestGameInstance::Pump(TFunctionRef<bool()> Done, double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	double Last = FPlatformTime::Seconds();
	while (!Done())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now >= Deadline)
		{
			return Done();
		}
		TickOnce(static_cast<float>(Now - Last));
		Last = Now;
		FPlatformProcess::Sleep(0.002f);
	}
	return true;
}

void FStageTestGameInstance::PumpFor(double Seconds)
{
	Pump([]() { return false; }, Seconds);
}

#endif // WITH_DEV_AUTOMATION_TESTS
