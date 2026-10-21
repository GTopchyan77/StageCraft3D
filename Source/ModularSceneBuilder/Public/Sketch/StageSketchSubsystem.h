// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sketch/StageSketchTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageSketchSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageSketchChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSketchGenerationFinished, const FStageSketchGenerateResult&, Result);

/**
 * The 2D-to-3D generator of this stage session (Docs/ADR/0015-sketch-to-stage.md §2.1): the source of truth for the sketch drawn
 * in the Sketch window (strokes, settings, the catalog item used for each element), and the planner that turns it into decks,
 * truss runs and fixtures.
 *
 * Ownership and writes:
 *  - The sketch is tool state, not stage state: the Sketch window edits it through the validated setters here (every stroke and
 *    setting is sanitized; the sketch holds at most StageSketchMath::MaxStrokes strokes). Each change fires OnSketchChanged.
 *  - It never changes the stage. Generating goes through AModularPlayerController::RequestGenerateFromSketch, which asks for the
 *    plan (MakePlan) and its assets (RequestPlanAssets), then spawns it through the player's spawn system and the GameMode rules
 *    (StageSketchBuilder) and records one undo step. ReportGeneration tells the views how it ended.
 *  - One generation is in flight at a time; its asset request is cancelled when the world ends.
 *
 * Per stage session: the sketch is dropped with the world and is not saved in scene files. Game and PIE worlds only. Never ticks
 * and never iterates the world; the catalog is read from UStageItemSubsystem.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageSketchSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	// --- Sketch ---

	const TArray<FStageSketchStroke>& GetStrokes() const { return Strokes; }

	/** Adds a stroke after StageSketchMath::SanitizeStroke. False, and nothing added, when it is unusable or the sketch is full. */
	bool AddStroke(const FStageSketchStroke& Stroke);

	/** False for an invalid index. */
	bool RemoveStroke(int32 Index);

	/** Removes the newest stroke. False when the sketch is empty. */
	bool RemoveLastStroke();

	/** Empties the sketch. No event when it already was. */
	void ClearStrokes();

	/** The newest stroke within Radius (cm) of Point, or INDEX_NONE. */
	int32 FindStrokeAt(const FVector2D& Point, double Radius) const;

	/** Strokes that become Element. */
	int32 CountStrokes(EStageSketchElement Element) const;

	// --- Settings ---

	const FStageSketchSettings& GetSettings() const { return Settings; }

	/** Stores the settings after StageSketchMath::SanitizeSettings. No event when nothing changed. */
	void SetSettings(const FStageSketchSettings& NewSettings);

	// --- Catalog items ---

	/** Catalog items that can build Element, in catalog order: deck items, truss spans, lighting fixtures. */
	TArray<class UBaseItemData*> GetCandidateItems(EStageSketchElement Element) const;

	/** The item that will build Element: the one chosen with SetItem while it is still a candidate, else the first candidate. */
	class UBaseItemData* GetItem(EStageSketchElement Element) const;

	/** Chooses the item for Element. False, and nothing changed, unless Item is one of its candidates. */
	bool SetItem(EStageSketchElement Element, class UBaseItemData* Item);

	// --- Generation ---

	/** The plan for the current sketch, settings and items (StageSketchGenerator::BuildPlan). Floor at world Z 0. */
	FStageSketchBuildPlan MakePlan() const;

	/**
	 * Keeps Plan as the generation in flight and streams its items' Game bundles; OnReady runs once they are in memory, possibly
	 * before this returns. The callback takes the plan with TakeReadyPlan. False, and nothing kept, while another generation is
	 * in flight or for an empty plan.
	 */
	bool RequestPlanAssets(const FStageSketchBuildPlan& Plan, FSimpleDelegate OnReady);

	bool IsGenerationPending() const { return PendingPlan.IsSet(); }

	/** The plan whose assets are ready, handed over once; unset before they are ready or after it was taken. */
	TOptional<FStageSketchBuildPlan> TakeReadyPlan();

	/** Records how a generation ended and broadcasts it (the Sketch window shows the message). */
	void ReportGeneration(const FStageSketchGenerateResult& Result);

	const FStageSketchGenerateResult& GetLastResult() const { return LastResult; }

	/** Any change to the strokes, the settings or the chosen items. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Sketch")
	FOnStageSketchChanged OnSketchChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Sketch")
	FOnStageSketchGenerationFinished OnGenerationFinished;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	void HandlePlanAssetsLoaded();

	class UStageItemSubsystem* GetCatalog() const;

	TArray<FStageSketchStroke> Strokes;
	FStageSketchSettings Settings;

	/** Chosen items, one per EStageSketchElement; null means "the first candidate". Catalog items, kept alive by the catalog too. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UBaseItemData>> ChosenItems;

	TOptional<FStageSketchBuildPlan> PendingPlan;
	bool bPendingPlanReady = false;
	FSimpleDelegate PendingCallback;
	TSharedPtr<struct FStreamableHandle> LoadHandle;

	FStageSketchGenerateResult LastResult;
};
