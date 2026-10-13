// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Placement/StagePlacementTypes.h"
#include "Placement/StageSpanMath.h"
#include "StageSpanPreview.generated.h"

/**
 * Visual-only ghost of the path being drawn by UStageSpanToolComponent: the exact segments it would place, in the
 * placement ghost's colours (valid / refused).
 *
 * Pure presentation, like AStagePlacementPreview: no tick, no collision (so the placement trace never hits it), and nothing
 * happens unless the tool pushes a change. The path is rebuilt only when the item or the layout changes; moving the cursor
 * while the layout stays the same only moves the actor.
 */
UCLASS(NotBlueprintable)
class MODULARSCENEBUILDER_API AStageSpanPreview : public AActor
{
	GENERATED_BODY()

public:
	AStageSpanPreview();

	/** The span whose segments the ghost shows. Null hides the ghost until a span is set. */
	void SetItem(const class UStageSpanData* InItem);

	/** Shows the path Layout at PathTransform (actor transform of the path). State must be Valid or Refused; it selects the colour. */
	void ShowPath(const FTransform& PathTransform, const FStageSpanPathLayout& Layout, EStagePlacementPreviewState State);

	/** Hides the ghost. Idempotent. */
	void HidePreview();

	bool IsPreviewVisible() const { return !IsHidden(); }

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor Interface

	/** Translucent, unlit, with a "GhostColor" vector parameter: the placement ghost's material. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	TObjectPtr<class UMaterialInterface> GhostMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	FLinearColor ValidColor = FLinearColor(0.1f, 0.75f, 1.0f);

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Placement")
	FLinearColor RefusedColor = FLinearColor(1.0f, 0.15f, 0.1f);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UStageSpanGeometryComponent> Geometry = nullptr;

private:
	UPROPERTY(Transient)
	TObjectPtr<const class UStageSpanData> Item = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> GhostMaterialInstance = nullptr;

	/** The layout currently built, so a cursor move that keeps it only moves the actor. */
	FStageSpanPathLayout BuiltLayout;
};
