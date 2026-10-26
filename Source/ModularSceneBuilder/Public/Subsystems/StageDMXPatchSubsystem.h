// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageDMXPatching.h"
#include "StageDMXPatchSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageDMXPatchChanged);

/** A patch planned for one fixture. Applied through AModularPlayerController::RequestPatchFixtures, never directly. */
struct FStageDMXPatchAssignment
{
	TWeakObjectPtr<class ALightingFixtureActor> Fixture;
	FStageDMXPatch Patch;
};

/**
 * DMX addressing for one world (Docs/ADR/0019-dmx-patch-and-network-input.md §2): which fixture sits on which universe and
 * channels, automatic patching of new fixtures, conflicts, universe usage and patch plans (patch the unpatched, patch a
 * selection from an address, repair conflicts).
 *
 * The fixture's own Patch (SaveGame, undoable) stays the source of truth. This subsystem keeps a derived per-universe index
 * for the DMX input and output paths, rebuilt lazily after a fixture reports a change (NotifyPatchChanged), so a received
 * frame touches only the fixtures of its universe. It never iterates the world and never ticks: fixtures register on
 * BeginPlay and unregister on EndPlay.
 *
 * Read access is open; it changes no patch itself except the automatic patch of a newly registered fixture. Every planned
 * change is applied by the controller through the rules (GameMode::EvaluateParameterChange) and recorded for undo.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageDMXPatchSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Called by ALightingFixtureActor::BeginPlay after its fixture ID is assigned. An unpatched fixture is auto-patched if enabled. */
	void RegisterFixture(class ALightingFixtureActor* Fixture);
	void UnregisterFixture(class ALightingFixtureActor* Fixture);

	/** Called by a registered fixture whenever its patch or its footprint (item data) changed. */
	void NotifyPatchChanged(class ALightingFixtureActor& Fixture);

	/** First range of Footprint free channels at or after StartUniverse.StartAddress; unpatched if there is none. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|DMX")
	FStageDMXPatch FindNextFreePatch(int32 Footprint, int32 StartUniverse = 1, int32 StartAddress = 1) const;

	/** Fixture IDs whose ranges overlap another fixture's or run past channel 512, ascending. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	TArray<int32> FindPatchConflicts() const;

	/** Universes that have at least one patched fixture, ascending. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	TArray<int32> GetPatchedUniverses() const;

	/** Fixtures patched in Universe (entries may have died since). The view is valid until the patch next changes. */
	TConstArrayView<TWeakObjectPtr<class ALightingFixtureActor>> GetFixturesInUniverse(int32 Universe) const;

	/** Channels, fixtures and conflicts per patched universe, ascending. */
	TArray<FStageDMXUniverseUsage> GetUniverseUsage() const;

	/** Every unpatched fixture with a footprint, by fixture ID, each at the next free fit from 1.001. */
	TArray<FStageDMXPatchAssignment> PlanUnpatched() const;

	/**
	 * Fixtures (in fixture-ID order, footprint-less ones skipped) patched one after another from Start, around the fixtures
	 * that keep their patch. All or nothing: empty when they do not all fit.
	 */
	TArray<FStageDMXPatchAssignment> PlanConsecutive(TConstArrayView<class ALightingFixtureActor*> ToPatch, const FStageDMXPatch& Start) const;

	/** New patches that remove every conflict (StageDMXPatching::PlanConflictRepair). Empty when there is none to repair. */
	TArray<FStageDMXPatchAssignment> PlanConflictRepair() const;

	/** Changes on every patch change, so a consumer can tell its derived state is stale without binding a delegate. */
	uint32 GetPatchRevision() const { return PatchRevision; }

	/** New fixtures without a patch get the next free address. Off = they stay unpatched until the user patches them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|DMX")
	bool bAutoPatchNewFixtures = true;

	/** A fixture was patched, re-patched, registered or removed. Broadcast once per change. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|DMX")
	FOnStageDMXPatchChanged OnPatchChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	/** Live fixtures as patch slots, keyed by their index in Fixtures; Skip are left out (the ones being re-planned). */
	TArray<FStageDMXPatchSlot> MakeSlots(TConstArrayView<class ALightingFixtureActor*> Skip = {}) const;
	TArray<FStageDMXPatchAssignment> ToAssignments(TConstArrayView<FStageDMXPatchPlanEntry> Plan) const;
	void RebuildIndexIfStale() const;
	void HandlePatchChanged();

	TArray<TWeakObjectPtr<class ALightingFixtureActor>> Fixtures;

	/** Derived from the fixtures' patches; rebuilt on the first read after a change. */
	mutable TMap<int32, TArray<TWeakObjectPtr<class ALightingFixtureActor>>> UniverseIndex;
	mutable bool bIndexStale = true;

	uint32 PatchRevision = 0;
};
