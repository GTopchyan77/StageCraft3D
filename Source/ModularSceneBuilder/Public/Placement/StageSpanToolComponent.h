// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSpanMath.h"
#include "StageSpanToolComponent.generated.h"

/** Where the span tool is in drawing a path. Exactly one phase at a time; transitions are listed on UStageSpanToolComponent. */
UENUM(BlueprintType)
enum class EStageSpanToolPhase : uint8
{
	/** Not in Place mode, or the armed item is not a span. The point placement tool handles clicks. */
	Inactive,
	/** The next click sets where the path starts. */
	PickStart,
	/** The path has a start; each click adds a vertex, Enter / a double-click / a click on the start point finishes it. */
	PickEnd,
};

/** What the span tool is doing, for the status bar. Lengths are in cm and include the leg under the cursor. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSpanToolStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	EStageSpanToolPhase Phase = EStageSpanToolPhase::Inactive;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	FText ItemName;

	/** True while the ghost shows a path (PickEnd with the cursor over a surface). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	bool bHasRun = false;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	double Length = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	int32 SegmentCount = 0;

	/** Vertices clicked so far (the start included). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	int32 PointCount = 0;

	/** Legs the ghost shows, the one under the cursor included. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	int32 LegCount = 0;

	/** The cursor is on the start point and a click would close the path into a loop. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	bool bClosing = false;

	/** The cursor is on the start point but the loop cannot close (its closing side is not a whole number of segments). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	bool bCloseBlocked = false;

	/** The rules would refuse this path now (display only; finishing is validated again). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Span")
	bool bRefused = false;

	bool operator==(const FStageSpanToolStatus& Other) const
	{
		return Phase == Other.Phase && ItemName.IdenticalTo(Other.ItemName) && bHasRun == Other.bHasRun && Length == Other.Length
			&& SegmentCount == Other.SegmentCount && PointCount == Other.PointCount && LegCount == Other.LegCount
			&& bClosing == Other.bClosing && bCloseBlocked == Other.bCloseBlocked && bRefused == Other.bRefused;
	}
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSpanToolStatusChanged, const FStageSpanToolStatus&, Status);

/**
 * Multi-point drawing of spans (UStageSpanData), like a polyline in a CAD tool (Docs/ADR/0007-lighting-patch-paths-and-pipeline-hardening.md §4.3):
 * click the start, click each bend (the ghost shows the exact path and the leg under the cursor, its length and segment count),
 * then finish with Enter, a double-click on the last point, or a click on the start point, which closes the path into a loop.
 * The whole path becomes one item. Backspace removes the last point; Esc drops the path, and a second Esc leaves Place mode
 * (the controller decides).
 *
 * Phases (EStageSpanToolPhase):
 *  - Inactive -> PickStart: Place mode with a span armed (UStagePlacementToolComponent owns the mode and the armed item).
 *  - PickStart -> PickEnd: a click on a surface (the start point).
 *  - PickEnd -> PickEnd: a click that adds a vertex; RemoveLastPoint while more than one point exists; a finish the rules refuse.
 *  - PickEnd -> PickStart: a finished path is placed; CancelPath (Esc); RemoveLastPoint on the start point; the armed item changes.
 *  - any -> Inactive: Place mode ends or a non-span item is armed.
 *
 * Like the placement tool it knows nothing about input or tracing: the controller pushes cursor hits (UpdateTarget / ClearTarget),
 * clicks (HandleClick) and keys (FinishPath, RemoveLastPoint, CancelPath) while IsToolActive. Spawning goes through
 * USpawnSystemComponent::SpawnConfiguredItem, so the GameMode validates the whole path with its full segment count. Requires
 * USpawnSystemComponent and UStagePlacementToolComponent on the same actor. No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageSpanToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageSpanToolComponent();

	/** True while Place mode has a span armed: the controller routes clicks, cursor moves and keys here instead of the point tool. */
	bool IsToolActive() const { return Phase != EStageSpanToolPhase::Inactive; }

	EStageSpanToolPhase GetPhase() const { return Phase; }

	const FStageSpanToolStatus& GetStatus() const { return Status; }

	/** The clicked vertices in world space, normalised (whole segments per leg). Empty in PickStart. */
	const TArray<FVector>& GetPendingPoints() const { return PendingPoints; }

	/** Moves the ghost to the path for Hit (PickStart: one segment at the cursor). Hides it when Hit has no surface. */
	void UpdateTarget(const struct FHitResult& Hit);

	/** Hides the ghost (cursor left the viewport, camera navigation). */
	void ClearTarget();

	/**
	 * PickStart: sets the start point. PickEnd: adds a vertex, or finishes the path when Hit is on the last point (double-click)
	 * or closes it when Hit is on the start point. Returns the placed span when this click finished one, else null.
	 */
	class AModularBaseActor* HandleClick(const struct FHitResult& Hit);

	/** Enter: places the path drawn so far (two or more points) as one item. Returns it, or null (too few points, refused). */
	class AModularBaseActor* FinishPath();

	/** Backspace: removes the last vertex (the start too, back to PickStart). Returns false when there is nothing to remove. */
	bool RemoveLastPoint();

	/** Esc: drops the path being drawn (PickEnd -> PickStart). Returns false, and does nothing, when no start was set. */
	bool CancelPath();

	/** The ghost, so still renders can leave it out. Null until the first path is drawn. */
	class AStageSpanPreview* GetPreviewActor() const { return Preview; }

	/** Display-only check that colours the ghost, with the path's segment count. Bound by the controller without reporting. */
	FStagePlacementValidator PreviewEvaluator;

	/** A path was placed (the undo history records it). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Span")
	FOnStageItemPlaced OnItemPlaced;

	/** The phase, the item or the path under the cursor changed (status bar). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Span")
	FOnStageSpanToolStatusChanged OnStatusChanged;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Span")
	TSubclassOf<class AStageSpanPreview> PreviewClass;

	/** A click this close (cm, horizontally) to the start point closes the path; this close to the last point finishes it. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Span", meta = (ClampMin = "1.0", Units = "cm"))
	float PointPickRadius = 30.f;

private:
	/** The path the next click would produce, ready to show or place, in the path's own space. */
	struct FCandidatePath
	{
		FTransform Transform;
		FStageSpanPathLayout Layout;

		/** The cursor is on the start point and the path would close. */
		bool bClosing = false;

		/** The cursor is on the start point of a path that cannot close: the ghost shows the open path, and a click does nothing. */
		bool bCloseBlocked = false;
	};

	UFUNCTION()
	void HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode);

	UFUNCTION()
	void HandleArmedItemChanged(class UBaseItemData* ArmedItem);

	/** Re-derives Inactive / PickStart from the placement tool. A path in progress (PickEnd) is kept while the tool stays active. */
	void RefreshActivation();

	class UStageSpanData* GetArmedSpan() const;

	/** The grid-snapped point for Hit, the same snapping the point tool gives this item. */
	FVector ComputeSnappedPoint(const class UStageSpanData& Span, const struct FHitResult& Hit) const;

	/** The next vertex for Hit: polar-snapped from the last point and normalised to whole segments. */
	FVector ComputeNextPoint(const class UStageSpanData& Span, const struct FHitResult& Hit) const;

	/** True when World is within PointPickRadius (horizontally) of Target. */
	bool IsNearPoint(const FVector& World, const FVector& Target) const;

	/**
	 * The path the cursor would make: PickStart shows one segment at the cursor; PickEnd shows the clicked vertices plus the leg
	 * to the cursor (closed when the cursor is on a closable start point). False without a surface.
	 */
	bool ComputeCandidate(const class UStageSpanData& Span, const struct FHitResult& Hit, FCandidatePath& OutCandidate) const;

	/** Lays out world-space vertices as a path placed at the first one, facing along its first leg. */
	FCandidatePath MakePath(const class UStageSpanData& Span, TConstArrayView<FVector> WorldPoints, bool bClosed) const;

	/** Spawns PendingPoints (closed or open) as one span item through the validated path; resets to PickStart on success. */
	class AModularBaseActor* PlacePendingPath(bool bClosed);

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

	/** Spawned on first use, so controllers that never draw a path never create one. */
	UPROPERTY(Transient)
	TObjectPtr<class AStageSpanPreview> Preview = nullptr;

	EStageSpanToolPhase Phase = EStageSpanToolPhase::Inactive;
	FStageSpanToolStatus Status;

	/** Vertices clicked so far, world space; non-empty exactly in PickEnd. */
	TArray<FVector> PendingPoints;

	/** Direction of the last leg drawn, so the start marker of the next path points the same way. */
	double LastYawDegrees = 0.0;

	/** Cached rules verdict, keyed by segment count. INDEX_NONE forces a new check. */
	int32 VerdictSegmentCount = INDEX_NONE;
	EStagePlacementPreviewState Verdict = EStagePlacementPreviewState::Valid;
};
