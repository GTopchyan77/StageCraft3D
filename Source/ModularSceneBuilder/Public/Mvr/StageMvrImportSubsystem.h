// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Economy/StageEconomyTypes.h"
#include "Engine/TimerHandle.h"
#include "Mvr/StageMvrImportTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "StageMvrImportSubsystem.generated.h"

/**
 * MVR scene import (Docs/ADR/0034 §6, phases MV2 and MV3): reads an .mvr file, shows what it holds, and on confirmation adds its
 * GDTF fixture types to the library and places its fixtures and trusses on the stage as ONE undo step.
 *
 * Flow, one file at a time (GetState):
 *  1. RequestRead: the file is read and checked on a worker (StageMvrArchive: ZIP, scene XML, every GDTF, truss geometry bounds).
 *     The archive bytes never reach the game thread; only the parsed result does, and it is dropped when the review closes.
 *  2. Review: the user sees GetReview (types, counts, warnings, errors). Nothing has changed yet. CancelReview drops it.
 *  3. StartImport (called by AModularPlayerController::RequestConfirmMvrImport after the GameMode allowed it): the GDTF types are
 *     added through UStageFixtureLibrarySubsystem::AddEmbeddedFile (a type already in the library is reused), the plan is built
 *     (StageMvrImport::Build), the catalog items it needs stream in, then FStageMvrPlacementJob places it a slice per frame
 *     (PlacementBudgetSeconds), all or nothing. The controller's OnPlaced records the result as one undo step.
 *  Adding fixture types to the library is not undone: the library is outside the stage's history, as for a GDTF import.
 *
 * Views read GetState / GetReview / GetProgress and follow OnChanged; the outcome arrives on OnFinished.
 * Lifetime: the whole run (GameInstance). Game-thread API; the worker's result is matched to this instance by weak pointer and a
 * serial, and dropped after Deinitialize or a cancel. The frame-sliced placement runs on the game instance's timer manager and
 * ends (rolled back) when its spawner goes away.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageMvrImportSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Time placing items may take per frame. */
	static constexpr double PlacementBudgetSeconds = 0.008;

	/** True for a file name this imports (.mvr). */
	static bool IsMvrFile(const FString& File);

	/**
	 * Starts reading Path for review. Returns why it cannot start (not .mvr, missing, larger than the MVR cap, another import is
	 * open), or unset when the read started. The review arrives on OnChanged; a file that cannot be read becomes a review with
	 * errors, so the user sees why.
	 */
	TOptional<FText> RequestRead(const FString& Path);

	EStageMvrImportState GetState() const { return State; }

	/** The review in Review and Placing; null otherwise. */
	const FStageMvrImportReview* GetReview() const { return State == EStageMvrImportState::Review || State == EStageMvrImportState::Placing ? &Review : nullptr; }

	/** Items placed and planned while Placing (0 / 0 while the items stream in). */
	void GetProgress(int32& OutDone, int32& OutTotal) const;

	/** Drops the review (Review only). False when ReviewId is not the open review. */
	bool CancelReview(const FGuid& ReviewId);

	/** What the controller hands over for StartImport: where and how to place, and what is on the stage already. */
	struct FPlacementRequest
	{
		TWeakObjectPtr<class USpawnSystemComponent> Spawner;
		/** The GameMode's placement rules without reporting (AModularPlayerController::EvaluatePlacementPreview). */
		TFunction<FStageEconomyResultInfo(const class UBaseItemData&, int32)> Check;
		/** The truss MVR trusses are built with, and the fixture for fixtures without a usable type. Either may be null. */
		TWeakObjectPtr<class UStageSpanData> TrussItem;
		TWeakObjectPtr<class UBaseItemData> GenericFixture;
		TSet<int32> UsedFixtureIds;
		TSet<FGuid> ExistingMvrIds;
		/**
		 * Called once the whole rig is placed: Items in plan order, LayerGroups (layer name, fixture IDs) and the undo step's name.
		 * Returns the number of fixture groups it made. Not called when nothing was placed.
		 */
		TFunction<int32(TConstArrayView<class AModularBaseActor*> Items, const TArray<TPair<FString, TArray<int32>>>& LayerGroups, const FText& StepName)> OnPlaced;
	};

	/** Confirms the open review: Ok when placing started (the outcome arrives on OnFinished), else why not (nothing changed). */
	FStageEconomyResultInfo StartImport(const FGuid& ReviewId, FPlacementRequest Request);

	/** State, review or progress changed (at most once per frame while placing). */
	FOnStageMvrImportChanged OnChanged;

	/** An import ended: placed (one undo step) or refused and rolled back. */
	FOnStageMvrImportFinished OnFinished;

private:
	void FinishRead(uint64 Serial, TSharedPtr<struct FStageMvrArchive> Archive);
	FStageMvrImportReview MakeReview(const struct FStageMvrArchive& Archive) const;
	void BeginPlacing();
	void ScheduleStep();
	void StepPlacement();
	void FinishImport(const FStageMvrImportOutcome& Outcome);
	void Reset();
	void SetState(EStageMvrImportState NewState);

	EStageMvrImportState State = EStageMvrImportState::Idle;
	uint64 ReadSerial = 0;
	FString ReadingFile;
	FStageMvrImportReview Review;
	/** The parsed file behind Review; released when the review closes. */
	TSharedPtr<struct FStageMvrArchive> Archive;

	FPlacementRequest Placement;
	TSharedPtr<class FStageMvrPlacementJob> Job;
	TSharedPtr<struct FStreamableHandle> LoadHandle;
	FTimerHandle StepTimer;
};
