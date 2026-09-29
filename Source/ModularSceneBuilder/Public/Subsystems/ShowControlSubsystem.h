// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Show/ShowControlTypes.h"
#include "ShowControlSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnShowFixturesChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnShowCueListChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnShowCueEvent, float, CueNumber);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnShowControlSourceChanged, EStageControlSource, NewSource);

/**
 * Show control for one world: fixture registry and patch, cue list with crossfade playback,
 * and the DMX in/out boundary.
 *
 * A world subsystem because fixtures are world actors and a show belongs to a venue (level).
 * Fixtures register themselves on BeginPlay, so nothing iterates the world. It ticks only while
 * a fade is running or DMX output is active (IsTickable), never at idle.
 *
 * Signal flow (Internal):  cue list -> fixture attributes -> visuals, and -> DMX bridge -> wire
 * Signal flow (External):  wire -> DMX bridge -> ReceiveDMXUniverse -> fixture attributes -> visuals
 */
UCLASS()
class MODULARSCENEBUILDER_API UShowControlSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem / UWorldSubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem / UWorldSubsystem Interface

	//~ Begin FTickableGameObject Interface
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject Interface

	// --- Fixtures & patch ---

	/** Called by ALightingFixtureActor::BeginPlay. Assigns a free fixture ID and, if enabled, a free DMX address. */
	void RegisterFixture(class ALightingFixtureActor* Fixture);
	void UnregisterFixture(class ALightingFixtureActor* Fixture);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	TArray<class ALightingFixtureActor*> GetFixtures() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	class ALightingFixtureActor* FindFixture(int32 FixtureId) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	bool IsFixtureIdFree(int32 FixtureId, const class ALightingFixtureActor* Ignore = nullptr) const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	int32 GetNextFreeFixtureId() const;

	/** First address range of Footprint channels, from StartUniverse upwards, that no patched fixture occupies. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	FStageDMXPatch FindNextFreePatch(int32 Footprint, int32 StartUniverse = 1) const;

	/** Fixture IDs whose DMX ranges overlap another fixture's. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	TArray<int32> FindPatchConflicts() const;

	/** New fixtures without a patch get the next free address. Off = they stay unpatched until the user patches them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Show")
	bool bAutoPatchNewFixtures = true;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowFixturesChanged OnFixturesChanged;

	// --- Cues ---

	/** Records every fixture's current attributes as a cue. Overwrites an existing cue with the same number. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	void StoreCue(float CueNumber, const FText& Label, float FadeTime = 2.f);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool DeleteCue(float CueNumber);

	/** Crossfades to the cue. Fails in External control mode or for an unknown number. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool GoToCue(float CueNumber);

	/** Plays the cue after the active one (the first cue if none is active). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	bool Go();

	/** Snaps a running fade to its end state. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	void FinishFade();

	/** Sorted by cue number. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	const TArray<FShowCue>& GetCues() const { return Cues; }

	/** Replaces the cue list, e.g. when loading a show file. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Show")
	void SetCues(const TArray<FShowCue>& InCues);

	/** Number of the last cue started, or a negative value if none. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	float GetActiveCueNumber() const { return ActiveCueNumber; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Show")
	bool IsFading() const { return !Fades.IsEmpty(); }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowCueListChanged OnCueListChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowCueEvent OnCueStarted;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Show")
	FOnShowCueEvent OnCueFinished;

	// --- DMX ---

	/** Installs (or with nullptr removes) the transport. The previous bridge is stopped. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void SetDMXBridge(class UStageDMXBridge* NewBridge);

	UFUNCTION(BlueprintPure, Category = "StageCraft|DMX")
	class UStageDMXBridge* GetDMXBridge() const { return DMXBridge; }

	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void SetControlSource(EStageControlSource NewSource);

	UFUNCTION(BlueprintPure, Category = "StageCraft|DMX")
	EStageControlSource GetControlSource() const { return ControlSource; }

	/** Inbound DMX frame (from the bridge). Applied to fixtures only in External control mode. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void ReceiveDMXUniverse(int32 Universe, const TArray<uint8>& Channels);

	/** Encodes every fixture patched in Universe into a 512-slot buffer. Also useful for debugging patches. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	TArray<uint8> RenderDMXUniverse(int32 Universe) const;

	/** Universes that have at least one patched fixture, ascending. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	TArray<int32> GetPatchedUniverses() const;

	/** DMX refresh rate for output. 44 Hz is the practical maximum of a full 512-slot DMX512 line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|DMX", meta = (ClampMin = "1.0", ClampMax = "60.0", Units = "Hz"))
	float DMXOutputRateHz = 44.f;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|DMX")
	FOnShowControlSourceChanged OnControlSourceChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	struct FAttributeFade
	{
		TWeakObjectPtr<class ALightingFixtureActor> Fixture;
		TMap<FGameplayTag, float> From;
		TMap<FGameplayTag, float> To;
	};

	bool IsDMXOutputActive() const;
	void TickFades(float DeltaTime);
	void TickDMXOutput(float DeltaTime);
	void CompleteFade();
	void SortCues();
	int32 FindCueIndex(float CueNumber) const;

	TArray<TWeakObjectPtr<class ALightingFixtureActor>> Fixtures;

	UPROPERTY(Transient)
	TArray<FShowCue> Cues;

	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXBridge> DMXBridge = nullptr;

	TArray<FAttributeFade> Fades;
	float FadeDuration = 0.f;
	float FadeElapsed = 0.f;
	float ActiveCueNumber = -1.f;
	float DMXOutputAccumulator = 0.f;
	EStageControlSource ControlSource = EStageControlSource::Internal;
};
