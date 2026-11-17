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
 *  2. Review: the user sees GetReview (types, counts, warnings, errors, and for a plot already on the stage what is new, changed,
 *     unchanged and gone: §18). Nothing has changed yet. CancelReview drops it.
 *  3. StartImport (called by AModularPlayerController::RequestConfirmMvrImport after the GameMode allowed it): the GDTF types are
 *     added through UStageFixtureLibrarySubsystem::AddEmbeddedFile (a type already in the library is reused), the plan is built
 *     (StageMvrImport::Build), the catalog items it needs stream in, then FStageMvrPlacementJob places it a slice per frame
 *     (PlacementBudgetSeconds), all or nothing: removals, updates (the same items, changed only where the file changed) and new
 *     items. The controller's OnPlaced records the result as one undo step.
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
	 * errors, so the user sees why. Stage is the stage as it is now: the review compares the file with it (re-import).
	 */
	TOptional<FText> RequestRead(const FString& Path, FStageMvrStage Stage);

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
		/** The stage now (StageMvrStageReader): items from MVR, fixture IDs in use, the truss and generic fixture to build with. */
		FStageMvrStage Stage;
		/** Items of the same plot the file no longer has: removed or kept (the review's choice). */
		EStageMvrMissingItems Missing = EStageMvrMissingItems::Keep;
		/**
		 * Called once the whole plan is applied: Removed (snapshots of the items taken off: removed ones and the old state of
		 * updated ones), Items (placed: new and updated, in plan order), LayerGroups (layer name, fixture IDs of NEW fixtures) and the
		 * undo step's name. Returns the number of fixture groups it made or added to. Not called when the job failed.
		 */
		TFunction<int32(TConstArrayView<struct FStageItemSnapshot> Removed, TConstArrayView<class AModularBaseActor*> Items,
			const TArray<TPair<FString, TArray<int32>>>& LayerGroups, const FText& StepName)> OnPlaced;
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
	/** The stage the open read is compared with; dropped once the review is made. */
	FStageMvrStage ReadingStage;
	FStageMvrImportReview Review;
	/** The parsed file behind Review; released when the review closes. */
	TSharedPtr<struct FStageMvrArchive> Archive;

	FPlacementRequest Placement;
	TSharedPtr<class FStageMvrPlacementJob> Job;
	TSharedPtr<struct FStreamableHandle> LoadHandle;
	FTimerHandle StepTimer;
};
