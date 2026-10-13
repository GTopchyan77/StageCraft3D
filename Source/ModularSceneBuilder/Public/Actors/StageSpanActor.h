// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Placement/StageSpanMath.h"
#include "StageSpanActor.generated.h"

/**
 * A placed linear item (truss line, railing, cable run) of a UStageSpanData, drawn as a path of straight legs: whole segments
 * repeated along each leg, corner parts at the bends, never one mesh scaled to length (Docs/ADR/0005 §2, Docs/ADR/0007 §4).
 *
 * State is SaveGame values: the path's vertices (actor-local, the first at the origin), whether it is closed, and the sag of a
 * hanging run. Undo snapshots and scene files carry them with no extra format. Everything else (legs, segment counts,
 * instances) is derived from them and the catalog item, and rebuilt when either changes. Scene files written before paths
 * existed hold only SpanLength; they load as a one-leg path.
 *
 * - Quantity: the total segment count. Session totals and the bill of materials count the item's specs once per segment.
 * - A one-leg path is resized through the Length parameter (inspector), validated by the GameMode like any edit: refused while
 *   the item is locked, and lengthening must fit the session's power and weight limits. A multi-leg path shows its total length
 *   read-only: it is reshaped by drawing it again.
 * - Sag (cables): an undoable parameter, refused while locked; only stretched items whose data allows sag have it.
 * - Spans do not scale (SupportsScaling is false): their size is their path.
 *
 * Never ticks; the path is rebuilt only when its state or the item changes.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageSpanActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStageSpanActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Span")
	class UStageSpanData* GetSpanData() const;

	/** The path as built: normalised vertices and one run per leg, for the current state and catalog item. */
	FStageSpanPathLayout GetPathLayout() const;

	/** Total length along the legs in cm. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Span")
	double GetSpanLength() const { return GetPathLayout().GetLength(); }

	/**
	 * Sets the path for a span being spawned (the span tool): actor-local points (the first should be the origin) and whether the
	 * path closes on itself. They are normalised by the item's rules. Edits of a placed span go through its parameters.
	 */
	void InitializePath(TConstArrayView<FVector> LocalPoints, bool bClosed);

	/** Instances shown, over all parts (diagnostics). */
	int32 GetGeometryInstanceCount() const;

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
	TObjectPtr<class UStageSpanGeometryComponent> Geometry = nullptr;

private:
	/** The layout of Points for the current item; invalid without span data. */
	FStageSpanPathLayout ComputePathFor(TConstArrayView<FVector> Points, bool bClosed, double Sag) const;

	/** The points a one-leg path of InLength has (its direction kept). */
	TArray<FVector> MakeSingleLegPoints(double InLength) const;

	/** True when the stored path is a single straight leg, so its Length is editable. */
	bool IsSingleLeg() const;

	/** Stores the normalised layout as the new state (so the saved state is always what was built) and rebuilds. */
	void ApplyPath(const FStageSpanPathLayout& Layout);

	/** Brings legacy (SpanLength only) and empty state to a path, then normalises it for the current item. */
	void NormalisePath();

	void RebuildGeometry();
	void NotifyShapeChanged();

	/** Path vertices in actor space, normalised; PathPoints[0] is the origin. Loaded files are re-normalised by the item's rules. */
	UPROPERTY(EditAnywhere, SaveGame, Category = "StageCraft|Span")
	TArray<FVector> PathPoints;

	UPROPERTY(EditAnywhere, SaveGame, Category = "StageCraft|Span")
	bool bClosedPath = false;

	/** Mid-span sag of each leg in cm (limited per leg by the item); 0 for rigid items. */
	UPROPERTY(EditAnywhere, SaveGame, Category = "StageCraft|Span", meta = (ClampMin = "0.0", Units = "cm"))
	double SagDepth = 0.0;

	/** Run length of a straight span from scene files written before paths (STATE.md #27). Read once to migrate; then 0. */
	UPROPERTY(SaveGame)
	double SpanLength = 0.0;

	/** The layout currently built, kept so reads do not re-run the path math. */
	FStageSpanPathLayout BuiltLayout;
};
