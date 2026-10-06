// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Placement/StagePlacementTypes.h"
#include "Placement/StageSnapTypes.h"
#include "StagePlacementPreview.generated.h"

/**
 * Visual-only placement preview: a translucent ghost of the armed item at its exact landing transform,
 * plus a decal marker projected onto the surface at the snapped landing point.
 *
 * Pure presentation. It never traces, never ticks and has no collision, so it can never be hit by the
 * placement trace that positions it. UStagePlacementToolComponent owns it and pushes every change.
 *
 * Content: GhostMaterial needs a "GhostColor" vector parameter (translucent, unlit); MarkerMaterial is a
 * deferred decal with a "MarkerColor" vector parameter. Missing materials are logged once and degrade
 * gracefully: the ghost keeps the mesh's own materials, the marker is not shown.
 */
UCLASS(NotBlueprintable)
class MODULARSCENEBUILDER_API AStagePlacementPreview : public AActor
{
	GENERATED_BODY()

public:
	AStagePlacementPreview();

	/** Shows Item's mesh as the ghost. Null hides the ghost mesh (marker only). Call when the armed item changes. */
	void SetItem(const class UBaseItemData* Item);

	/**
	 * Moves the preview. ItemTransform is where the item will spawn; LandingPoint/SurfaceNormal place the
	 * marker on the surface. State must be Valid or Refused; it selects the colour.
	 * SnapGuides are the object-snap alignment lines to draw (none hides them).
	 */
	void ShowAt(const FTransform& ItemTransform, const FVector& LandingPoint, const FVector& SurfaceNormal, EStagePlacementPreviewState State, TConstArrayView<FStageSnapGuide> SnapGuides = {});

	/** Hides ghost and marker. Idempotent. */
	void HidePreview();

	bool IsPreviewVisible() const { return !IsHidden(); }

	/** Object-snap guides currently drawn with the preview (diagnostics). */
	int32 GetVisibleSnapGuideCount() const;

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	//~ End AActor Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	TObjectPtr<class UMaterialInterface> GhostMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	TObjectPtr<class UMaterialInterface> MarkerMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	FLinearColor ValidColor = FLinearColor(0.1f, 0.75f, 1.0f);

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	FLinearColor RefusedColor = FLinearColor(1.0f, 0.15f, 0.1f);

	/** Smallest marker half-size, so thin items (cables, truss) still get a readable landing marker. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement", meta = (ClampMin = "1.0", Units = "cm"))
	double MinMarkerHalfSize = 25.0;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class USceneComponent> PreviewRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> GhostMesh = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UDecalComponent> LandingMarker = nullptr;

	/** Object-snap alignment guides, shown with the ghost. */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UStageSnapGuidesComponent> SnapGuidesComponent = nullptr;

private:
	void ApplyGhostMaterials();
	void ApplyColor(EStagePlacementPreviewState State);
	FVector ComputeMarkerSize() const;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> GhostMaterialInstance = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> MarkerMaterialInstance = nullptr;
};
