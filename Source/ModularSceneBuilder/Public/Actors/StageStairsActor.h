// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Placement/StageProceduralShapes.h"
#include "StageStairsActor.generated.h"

/**
 * Placed stage stairs of a UStageStairsData (Docs/ADR/0018 §5.2): equal steps climbing toward the actor's +X up to Rise, Width
 * wide, with optional handrails on both sides, built as a procedural mesh (StageProceduralShapes::BuildStairsMesh).
 *
 * State is three SaveGame values (rise, width, handrails), so undo snapshots and scene files carry them with no format change.
 * Restored values are untrusted and sanitized when the item data is applied; every repair is counted.
 *
 * - Rise, width and handrails are inspector parameters: undoable shape edits, refused while the item is locked.
 * - One stairs unit is one item (GetQuantity 1). Stairs do not scale: their size is their parameters.
 * - Collision is query-only on the stage item channel (complex as simple), so stairs can be selected and items placed on them.
 *
 * Never ticks; the mesh is rebuilt only when a parameter or the item changes.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageStairsActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStageStairsActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Stairs")
	class UStageStairsData* GetStairsData() const;

	/**
	 * Sets the shape of stairs being spawned (the assistant's build plan): rise and width in cm. Sanitized when the item is
	 * applied. Edits of placed stairs go through their parameters.
	 */
	void InitializeStairs(double InRise, double InWidth, bool bInHandrails);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Stairs")
	double GetRise() const { return Rise; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Stairs")
	double GetWidth() const { return Width; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Stairs")
	bool HasHandrails() const { return bHandrails; }

	/** The steps as built (count, step rise, run). */
	const FStageStairsLayout& GetLayout() const { return BuiltLayout; }

	/** Triangles in the built mesh (diagnostics and tests). */
	int32 GetMeshTriangleCount() const { return BuiltTriangleCount; }

	//~ Begin AModularBaseActor Interface
	virtual bool SupportsScaling() const override { return false; }
	//~ End AModularBaseActor Interface

protected:
	//~ Begin AModularBaseActor Interface
	virtual void ApplyItemData(const class UBaseItemData& Data) override;
	virtual void UpdateHighlight() override;
	virtual void GatherParameterSections(TArray<FStageParameterSection>& OutSections) const override;
	virtual bool ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const override;
	virtual bool WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value) override;
	//~ End AModularBaseActor Interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UProceduralMeshComponent> Surface = nullptr;

private:
	/** Brings Rise and Width within the item's ranges (the item's defaults for a fresh actor); reports repairs. */
	void SanitizeShape(const class UStageStairsData& Stairs);

	void RebuildMesh();

	/** Height the stairs climb, in cm. 0 on a fresh actor, which takes the item's default. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Stairs", meta = (Units = "cm"))
	double Rise = 0.0;

	/** In cm. 0 on a fresh actor, which takes the item's default. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Stairs", meta = (Units = "cm"))
	double Width = 0.0;

	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Stairs")
	bool bHandrails = true;

	/** Derived from the three values above and the item on every rebuild; never saved. */
	FStageStairsLayout BuiltLayout;

	int32 BuiltTriangleCount = 0;
};
