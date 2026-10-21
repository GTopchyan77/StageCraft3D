// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Containers/ArrayView.h"
#include "StageDeckActor.generated.h"

/**
 * A placed stage deck or riser of a UStageDeckData: a simple polygon outline extruded to a height, built as a procedural mesh
 * (Docs/ADR/0015-sketch-to-stage.md §2.6). Generated from closed shapes in the Sketch window, or placed from the Library with
 * the item's default footprint.
 *
 * State is two SaveGame values: the outline (actor-local corners in cm around the origin, which stands on the floor) and the
 * height. Undo snapshots and scene files carry them with no format change. Restored values are untrusted and sanitized when the
 * item data is applied (StageDeckMath::SanitizeOutline, the item's height range); every repair is counted.
 *
 * - Quantity: rental panels, ⌈area ÷ panel area⌉ (session totals and the bill of materials count specs per panel).
 * - Height is an inspector parameter: an undoable shape edit, refused while the item is locked. The outline is reshaped by
 *   drawing it again.
 * - Decks do not scale (SupportsScaling is false): their size is their outline.
 * - Collision is query-only on the stage item channel, so decks can be selected and items placed on top of them.
 *
 * Never ticks; the mesh is rebuilt only when the outline, the height or the item changes.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageDeckActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStageDeckActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Deck")
	class UStageDeckData* GetDeckData() const;

	/**
	 * Sets the shape of a deck being spawned (the sketch generator): actor-local corners around the origin and the height in cm.
	 * Both are sanitized when the item is applied. Edits of a placed deck go through its parameters.
	 */
	void InitializeShape(TConstArrayView<FVector2D> LocalOutline, double Height);

	/** The outline as built (sanitized, counter-clockwise). */
	const TArray<FVector2D>& GetOutline() const { return Outline; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Deck")
	double GetDeckHeight() const { return DeckHeight; }

	/** Top area in cm². */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Deck")
	double GetDeckArea() const;

	/** Triangles in the built mesh (diagnostics and tests). */
	int32 GetMeshTriangleCount() const { return BuiltTriangleCount; }

	//~ Begin AModularBaseActor Interface
	virtual int32 GetQuantity() const override;
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
	/** Brings Outline and DeckHeight within the item's rules (default footprint and height for a fresh actor); reports repairs. */
	void SanitizeShape(const class UStageDeckData& Deck);

	void RebuildMesh();

	/** Corners in actor space, counter-clockwise, around the origin. Empty on a fresh actor until the item gives it a footprint. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Deck")
	TArray<FVector2D> Outline;

	/** Deck top above the actor origin, in cm. 0 on a fresh actor, which takes the item's default. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Deck", meta = (Units = "cm"))
	double DeckHeight = 0.0;

	int32 BuiltTriangleCount = 0;
};
