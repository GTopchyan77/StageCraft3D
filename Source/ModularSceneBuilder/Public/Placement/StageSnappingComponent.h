// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Placement/StageSnapMath.h"
#include "Placement/StageSnapTypes.h"
#include "StageSnappingComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageSnapEngaged);

/**
 * Object-to-object snapping for one local player's tools (Docs/ADR/0003-undo-redo-and-object-snapping.md):
 * an item being placed or moved locks onto the bounding boxes of nearby placed items, flush side by
 * side, stacked, or with edges and centres aligned. The geometry is StageSnapMath (pure, tested); this
 * component supplies the boxes, the user preference and the guide lines.
 *
 * Consumers stay snapping-agnostic and receive it through delegates bound by the controller:
 *  - UStagePlacementToolComponent::PlacementSnapper -> SnapPlacement (sideways only; the surface trace decides height).
 *  - AModularTransformGizmo::TranslationSnapper -> SnapMove (along the dragged axis), between BeginMove and EndMove.
 *
 * Neighbour boxes come from UStageSessionSubsystem's registry, never from a world search. A move caches
 * them once at BeginMove (the scene is static while the user drags); placement gathers them per query,
 * which runs only on cursor or camera events, at most once per frame, and only in Place mode.
 *
 * The on/off preference is UStageCraftUserSettings::IsSnapToItemsEnabled; this component is its only writer.
 * No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageSnappingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageSnappingComponent();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Snapping")
	bool IsSnapToItemsEnabled() const;

	/** Stores and saves the preference. Takes effect on the next snap query. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Snapping")
	void SetSnapToItemsEnabled(bool bEnabled);

	/**
	 * Landing location for Item: X/Y lock onto neighbours measured at FreeTransform (no grid), other axes
	 * keep GridTransform. Not snapped (GridTransform's location) when snapping is off or Item has no Mesh
	 * to measure.
	 */
	FStageSnapOutcome SnapPlacement(const class UBaseItemData& Item, const FTransform& FreeTransform, const FTransform& GridTransform);

	/**
	 * Starts a move of Moving: caches its bounds and its neighbours'. Call when a drag begins. MovingWith are items
	 * that move along with it (the rest of a multi-selection); they are not neighbours, because they travel too.
	 */
	void BeginMove(const AActor& Moving, TConstArrayView<const AActor*> MovingWith = {});

	/** Snaps the moving item's ProposedLocation along the axes in AxisMask. Returns ProposedLocation outside a move. */
	FStageSnapOutcome SnapMove(const FVector& ProposedLocation, uint8 AxisMask);

	/** Ends the move and drops the cache. Idempotent. */
	void EndMove();

	/** A snap onto a different face than before has just engaged (for the snap cue). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Snapping")
	FOnStageSnapEngaged OnSnapEngaged;

protected:
	//~ Begin UActorComponent Interface
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	/** How close a face must come before it snaps. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Snapping", meta = (ClampMin = "0.0", Units = "cm"))
	double SnapDistance = 20.0;

	/** Guide line thickness per cm of camera distance, so guides keep a similar on-screen width. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Snapping", meta = (ClampMin = "0.0"))
	double GuideThicknessPerCm = 0.004;

private:
	void GatherNeighbourBounds(TConstArrayView<const AActor*> Exclude, TArray<FBox>& OutBounds) const;
	void AddGuides(const StageSnapMath::FSnapResult& Snap, const FBox& SnappedBox, TConstArrayView<FBox> Neighbours, FStageSnapGuideList& OutGuides) const;
	void NoteEngagement(const StageSnapMath::FSnapResult& Snap);
	double ComputeGuideThickness(const FVector& At) const;

	/** Bounds of a placed item's colliding components (what the cursor can hit). */
	static FBox GetItemBounds(const AActor& Item);

	TWeakObjectPtr<const AActor> MovingActor;
	FVector MoveStartLocation = FVector::ZeroVector;
	FBox MoveStartBounds = FBox(ForceInit);
	TArray<FBox> MoveNeighbours;

	/** Reused between placement queries to avoid reallocating per cursor move. */
	TArray<FBox> PlacementNeighbours;

	uint32 LastSnapSignature = 0;
};
