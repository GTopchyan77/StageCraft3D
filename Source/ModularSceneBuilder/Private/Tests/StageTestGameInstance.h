// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "IO/StageCraftPaths.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "UObject/StrongObjectPtr.h"

/**
 * Tests only: sets one property of a class default object for its lifetime and restores the old value after. Subsystems are
 * created from their class default object, so this is how a test chooses a config value (a backend class, a timeout) for the
 * subsystems it starts, without touching any .ini. The value type must match the property exactly.
 */
class FStageScopedDefault
{
public:
	template <typename TValue>
	static TUniquePtr<FStageScopedDefault> Make(UClass* Class, FName PropertyName, const TValue& Value)
	{
		return TUniquePtr<FStageScopedDefault>(new FStageScopedDefault(Class, PropertyName, &Value, sizeof(TValue)));
	}

	~FStageScopedDefault();
	FStageScopedDefault(const FStageScopedDefault&) = delete;
	FStageScopedDefault& operator=(const FStageScopedDefault&) = delete;

	/** False when the property does not exist or its size does not match the value given (nothing was changed). */
	bool IsValid() const { return Saved != nullptr; }

private:
	FStageScopedDefault(UClass* Class, FName PropertyName, const void* Value, int32 ValueSize);

	UObject* Default = nullptr;
	FProperty* Property = nullptr;
	void* Saved = nullptr;
};

/** What a test game instance starts with. Timeouts are the subsystems' config values (seconds). */
struct FStageTestGameInstanceOptions
{
	float BackendTimeoutSeconds = 30.f;
	float FetchTimeoutSeconds = 60.f;
	float OrderTimeoutSeconds = 300.f;
	float ToolTimeoutSeconds = 120.f;
	float SendTimeoutSeconds = 120.f;
};

/**
 * Tests only: a real, isolated game instance for integration tests (Docs/ADR/0021 §14, STATE.md #55).
 *
 * All the game-instance subsystems start as in the app (profile, economy, marketplace, bridge, AI tools, acquire dispatcher), with:
 *  - every StageCraft folder (scenes, marketplace, outboxes, credential vault, bridge session file) in a fresh temporary
 *    directory (FStageScopedDataRoot), deleted afterwards;
 *  - the profile not persisted, so the real save slot is never read or written (Economy security 10);
 *  - UStageTestCommerceBackend as the commerce backend, so the test decides every answer;
 *  - UStageTestMarketplaceBackend as the marketplace backend (the folder backend, with package fetches the test can hold);
 *  - the timeouts of FStageTestGameInstanceOptions.
 *
 * Time does not pass by itself: Pump runs the game thread's queued tasks and ticks the game instance's timers until a condition
 * holds, so a test is deterministic and needs no latent commands. Game thread only. Everything is undone in the destructor, in
 * reverse order (game instance, worker pipes, class defaults, data root, folder).
 */
class FStageTestGameInstance
{
public:
	explicit FStageTestGameInstance(const FStageTestGameInstanceOptions& Options = FStageTestGameInstanceOptions());
	~FStageTestGameInstance();

	FStageTestGameInstance(const FStageTestGameInstance&) = delete;
	FStageTestGameInstance& operator=(const FStageTestGameInstance&) = delete;

	/** False when the game instance could not be started (a class default could not be set); the test should stop. */
	bool IsValid() const { return GameInstance.IsValid(); }

	class UGameInstance& Get() const { return *GameInstance.Get(); }

	template <typename TSubsystem>
	TSubsystem* Subsystem() const { return GameInstance.IsValid() ? GameInstance->GetSubsystem<TSubsystem>() : nullptr; }

	/** The commerce backend the economy created. */
	class UStageTestCommerceBackend* GetCommerce() const;

	/** The temporary folder every StageCraft path points into. */
	const FString& GetRoot() const { return Root; }

	/**
	 * Runs queued game-thread tasks and ticks timers (in real time) until Done returns true or TimeoutSeconds passed.
	 * Returns Done().
	 */
	bool Pump(TFunctionRef<bool()> Done, double TimeoutSeconds = 5.0);

	/** Pump for a fixed time: lets timers due within Seconds fire. */
	void PumpFor(double Seconds);

	/** Stops the game instance now (Deinitialize of every subsystem). Idempotent; the destructor calls it too. */
	void Shutdown();

private:
	void TickOnce(float DeltaSeconds);

	FString Root;
	TUniquePtr<FStageScopedDataRoot> DataRoot;
	TArray<TUniquePtr<FStageScopedDefault>> Defaults;
	TStrongObjectPtr<class UGameInstance> GameInstance;
	TWeakObjectPtr<class UWorld> World;
};

#endif // WITH_DEV_AUTOMATION_TESTS
