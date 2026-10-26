// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Placement/StageProceduralShapes.h"
#include "StageArcTrussActor.generated.h"

/**
 * A placed curved box truss of a UStageArcTrussData (Docs/ADR/0018 §5.3): an arc of Radius (to the centre line) spanning Angle
 * degrees, up to a full circle, with chords and bracing generated along the true curve (StageProceduralShapes::BuildArcTrussMesh).
 * The actor origin is the arc's apex on the centre line; the arc bends toward the actor's -Y and the profile is centred on the
 * origin's height, like a straight truss hung at that height.
 *
 * State is two SaveGame values (radius, angle), so undo snapshots and scene files carry them with no format change. Restored
 * values are untrusted and sanitized when the item data is applied; every repair is counted.
 *
 * - Quantity: rental curved sections, ⌈arc length ÷ section length⌉; specs and the bill of materials count per section.
 *   Radius and angle are quantity parameters, so an edit that adds sections goes through the session's limits.
 * - Curved truss does not scale: its size is its parameters.
 * - Collision is one convex hull per section on the stage item channel (query only): the tubes are too thin to aim at, and item
 *   traces are simple-collision queries.
 *
 * Never ticks; the mesh is rebuilt only when a parameter or the item changes.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageArcTrussActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStageArcTrussActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Truss")
	class UStageArcTrussData* GetArcData() const;

	/**
	 * Sets the shape of an arc being spawned (the assistant's build plan): radius in cm and angle in degrees. Sanitized when the
	 * item is applied. Edits of a placed arc go through its parameters.
	 */
	void InitializeArc(double InRadius, double InAngleDegrees);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Truss")
	double GetRadius() const { return Radius; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Truss")
	double GetAngleDegrees() const { return AngleDegrees; }

	/** The arc as built (length, sections, bays). */
	const FStageArcTrussLayout& GetLayout() const { return BuiltLayout; }

	/** Triangles in the built mesh (diagnostics and tests). */
	int32 GetMeshTriangleCount() const { return BuiltTriangleCount; }

	//~ Begin AModularBaseActor Interface
	virtual int32 GetQuantity() const override;
	virtual int32 PredictQuantity(const FGameplayTag& ParameterId, const FStageParameterValue& Value) const override;
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
	/** Brings Radius and AngleDegrees within the item's ranges (the item's defaults for a fresh actor); reports repairs. */
	void SanitizeShape(const class UStageArcTrussData& Arc);

	/** The layout these values would build with this item (radius and angle clamped to its ranges). */
	FStageArcTrussLayout ComputeLayout(const class UStageArcTrussData& Arc, double InRadius, double InAngleDegrees) const;

	void RebuildMesh();

	/** To the centre line, in cm. 0 on a fresh actor, which takes the item's default. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Truss", meta = (Units = "cm"))
	double Radius = 0.0;

	/** 360 is a full circle. 0 on a fresh actor, which takes the item's default. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Truss", meta = (Units = "deg"))
	double AngleDegrees = 0.0;

	/** Derived from the values above and the item on every rebuild; never saved. */
	FStageArcTrussLayout BuiltLayout;

	int32 BuiltTriangleCount = 0;
};
