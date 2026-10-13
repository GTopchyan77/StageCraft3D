// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Placement/StageSpanMath.h"
#include "StageSpanGeometryComponent.generated.h"

/** What the meshes of a span geometry are for. Chosen once by the owner; it decides collision and materials. */
enum class EStageSpanGeometryUsage : uint8
{
	/** A placed run: blocks cursor traces like any stage item, and uses the parts' own materials. */
	PlacedItem,
	/** The placement ghost: no collision, no shadows, every slot drawn with the ghost material. */
	Preview,
};

/**
 * Builds the visible run of a span: every segment part and joint part of a UStageSpanData, repeated along the run's +X as
 * instances (StageSpanMath). Parts that share a mesh and material share one instanced component, so a run of any length costs
 * one draw batch per distinct mesh, not one per segment.
 *
 * Owns the instanced components it creates (transient, attached below itself, destroyed when no longer needed). Rebuilds only
 * when asked; it never ticks. Used by AStageSpanActor (placed runs) and AStageSpanPreview (the placement ghost).
 */
UCLASS(ClassGroup = (StageCraft))
class MODULARSCENEBUILDER_API UStageSpanGeometryComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UStageSpanGeometryComponent();

	/** Set once by the owner before the first Build. PreviewMaterial is required for Preview usage and ignored otherwise. */
	void SetUsage(EStageSpanGeometryUsage InUsage, class UMaterialInterface* InPreviewMaterial = nullptr);

	/**
	 * Shows Layout of Data. Null data or an invalid layout clears the run. Ignored while unregistered (a class default object);
	 * spawned actors register their components before construction, so a deferred spawn can build before it finishes. Part meshes
	 * are normally resident (they stream with the item's Game bundle when it is armed); otherwise they load synchronously once.
	 */
	void Build(const class UStageSpanData* Data, const FStageSpanLayout& Layout);

	/** Removes every instance and instanced component. Idempotent. */
	void Clear();

	/** Hover / selection overlay on every part (null removes it). Kept for components created by later builds. */
	void SetOverlayMaterial(class UMaterialInterface* InOverlayMaterial);

	/** Instances currently shown, over all parts (diagnostics and tests). */
	int32 GetInstanceCount() const;

	/** Instanced components currently used, one per distinct mesh and material (diagnostics). */
	int32 GetBatchCount() const { return Batches.Num(); }

private:
	/** The instanced component for batch Index, created on first use. */
	class UInstancedStaticMeshComponent* AcquireBatch(int32 Index);
	void ConfigureBatch(class UInstancedStaticMeshComponent& Batch) const;
	void ApplyMaterials(class UInstancedStaticMeshComponent& Batch, class UMaterialInterface* PartMaterial) const;
	void TrimBatches(int32 Count);

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UInstancedStaticMeshComponent>> Batches;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> PreviewMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> OverlayMaterial = nullptr;

	EStageSpanGeometryUsage Usage = EStageSpanGeometryUsage::PlacedItem;
};
