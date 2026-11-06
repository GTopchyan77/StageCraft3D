// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "StagePropActor.generated.h"

/**
 * A placed item of a UStagePropData (Docs/ADR/0031 §5): its parts as instanced meshes and its stand-in people as instances of the
 * shared figure mesh, each playing its clip on the GPU (per-instance custom data, UStageFigureAssetSubsystem). A drum kit with
 * its drummer, a lighting desk with its operator, an LED backdrop.
 *
 * - No state of its own: everything comes from the item, so undo, scenes and duplicates need nothing new.
 * - Never ticks; the people move in the material.
 * - Parts block cursor traces like any stage item (selectable, a surface for placement). People answer only the stage item
 *   channel: clicking one selects the item, but placement, camera pivots and fixture picking look through them.
 * - The hover and selection overlay is drawn on the parts only: an overlay on the people would show their rest pose, not the
 *   moving one.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStagePropActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStagePropActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Prop")
	class UStagePropData* GetPropData() const;

	/** People shown (diagnostics and tests). */
	int32 GetFigureCount() const;

	/** Part instances shown (diagnostics and tests). */
	int32 GetPartInstanceCount() const;

protected:
	//~ Begin AModularBaseActor Interface
	virtual void ApplyItemData(const class UBaseItemData& Data) override;
	virtual void UpdateHighlight() override;
	//~ End AModularBaseActor Interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UStageSpanGeometryComponent> Parts = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UInstancedStaticMeshComponent> Figures = nullptr;

private:
	void BuildFigures(const class UStagePropData& Prop);
};
