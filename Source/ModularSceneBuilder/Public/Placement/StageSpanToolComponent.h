// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSpanMath.h"
#include "StageSpanToolComponent.generated.h"

/** Where the span tool is in drawing a run. Exactly one phase at a time; transitions are listed on UStageSpanToolComponent. */
UENUM(BlueprintType)
enum class EStageSpanToolPhase : uint8
{
	/** Not in Place mode, or the armed item is not a span. The point placement tool handles clicks. */
	Inactive,
	/** The next click sets where the run starts. */
	PickStart,
	/** The next click places the run from the start point to the cursor. Its end becomes the next start (chained runs). */
	PickEnd,
};

/** What the span tool is doing, for the status bar. Length is in cm; Length and SegmentCount describe the run under the cursor. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSpanToolStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	EStageSpanToolPhase Phase = EStageSpanToolPhase::Inactive;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	FText ItemName;

	/** True while the ghost shows a run (PickEnd with the cursor over a surface). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	bool bHasRun = false;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	double Length = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	int32 SegmentCount = 0;

	/** The rules would refuse this run now (display only; the click is validated again). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	bool bRefused = false;

	bool operator==(const FStageSpanToolStatus& Other) const
	{
		return Phase == Other.Phase && ItemName.IdenticalTo(Other.ItemName) && bHasRun == Other.bHasRun && Length == Other.Length
			&& SegmentCount == Other.SegmentCount && bRefused == Other.bRefused;
	}
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSpanToolStatusChanged, const FStageSpanToolStatus&, Status);

/**
 * Two-point placement of spans (UStageSpanData), like drawing a line in a CAD tool (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §3):
 * click the start, move (the ghost shows the exact run, its length and segment count), click the end. The run is placed and its
 * end becomes the next start, so a truss rectangle is four clicks after the first. Esc drops the start point; a second Esc leaves
 * Place mode (the controller decides).
 *
 * Phases (EStageSpanToolPhase):
 *  - Inactive -> PickStart: Place mode with a span armed (UStagePlacementToolComponent owns the mode and the armed item).
 *  - PickStart -> PickEnd: a click on a surface.
 *  - PickEnd -> PickEnd: a click that places a run (chained), or one the rules refuse (nothing changes).
 *  - PickEnd -> PickStart: CancelStart (Esc), or the armed item changes.
 *  - any -> Inactive: Place mode ends or a non-span item is armed.
 *
 * Like the placement tool it knows nothing about input or tracing: the controller pushes cursor hits (UpdateTarget / ClearTarget)
 * and clicks (HandleClick) while IsToolActive. Spawning goes through USpawnSystemComponent::SpawnConfiguredItem, so the GameMode
 * validates every run with its full segment count. Requires USpawnSystemComponent and UStagePlacementToolComponent on the same actor.
 * No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageSpanToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageSpanToolComponent();

	/** True while Place mode has a span armed: the controller routes clicks and cursor moves here instead of the point tool. */
	bool IsToolActive() const { return Phase != EStageSpanToolPhase::Inactive; }

	EStageSpanToolPhase GetPhase() const { return Phase; }

	const FStageSpanToolStatus& GetStatus() const { return Status; }

	/** Moves the ghost to the run for Hit (PickStart: one segment at the cursor). Hides it when Hit has no surface. */
	void UpdateTarget(const struct FHitResult& Hit);

	/** Hides the ghost (cursor left the viewport, camera navigation). */
	void ClearTarget();

	/**
	 * PickStart: sets the start point. PickEnd: places the run to Hit and chains from its end. Returns the placed run, or null
	 * (start set, no surface, a click on the start point itself, or refused by the rules, which the validator's owner reports).
	 */
	class AModularBaseActor* HandleClick(const struct FHitResult& Hit);

	/** Esc: drops the start point (PickEnd -> PickStart). Returns false, and does nothing, when no start was set. */
	bool CancelStart();

	/** The ghost, so still renders can leave it out. Null until the first run is drawn. */
	class AStageSpanPreview* GetPreviewActor() const { return Preview; }

	/** Display-only check that colours the ghost, with the run's segment count. Bound by the controller without reporting. */
	FStagePlacementValidator PreviewEvaluator;

	/** A run was placed (the undo history records it). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Span")
	FOnStageItemPlaced OnItemPlaced;

	/** The phase, the item or the run under the cursor changed (status bar). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Span")
	FOnStageSpanToolStatusChanged OnStatusChanged;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Span")
	TSubclassOf<class AStageSpanPreview> PreviewClass;

private:
	UFUNCTION()
	void HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode);

	UFUNCTION()
	void HandleArmedItemChanged(class UBaseItemData* ArmedItem);

	/** Re-derives Inactive / PickStart from the placement tool. A run in progress (PickEnd) is kept while the tool stays active. */
	void RefreshActivation();

	class UStageSpanData* GetArmedSpan() const;

	/** The grid-snapped point for Hit, the same snapping the point tool gives this item. */
	FVector ComputeSnappedPoint(const class UStageSpanData& Span, const struct FHitResult& Hit) const;

	/** The run the next click would place (PickEnd) or a one-segment marker at the cursor (PickStart). False without a surface. */
	bool ComputeRun(const class UStageSpanData& Span, const struct FHitResult& Hit, FTransform& OutRunTransform, FStageSpanLayout& OutLayout) const;

	/** Rules verdict for SegmentCount segments of Span; asked again only when the count changes or after a placement. */
	EStagePlacementPreviewState EvaluateRun(const class UStageSpanData& Span, int32 SegmentCount);

	void SetPhase(EStageSpanToolPhase NewPhase);
	void SetStatus(const FStageSpanToolStatus& NewStatus);
	FStageSpanToolStatus MakeIdleStatus() const;
	void HidePreview();
	class AStageSpanPreview* GetOrCreatePreview();

	UPROPERTY(Transient)
	TObjectPtr<class UStagePlacementToolComponent> PlacementTool = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	/** Spawned on first use, so controllers that never draw a run never create one. */
	UPROPERTY(Transient)
	TObjectPtr<class AStageSpanPreview> Preview = nullptr;

	EStageSpanToolPhase Phase = EStageSpanToolPhase::Inactive;
	FStageSpanToolStatus Status;

	/** Valid in PickEnd. */
	FVector StartPoint = FVector::ZeroVector;

	/** Direction of the last run, so the next start marker and a zero-length run point the same way. */
	double LastYawDegrees = 0.0;

	/** Cached rules verdict, keyed by segment count. INDEX_NONE forces a new check. */
	int32 VerdictSegmentCount = INDEX_NONE;
	EStagePlacementPreviewState Verdict = EStagePlacementPreviewState::Valid;
};
