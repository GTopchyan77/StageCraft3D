// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Placement/StageSpanMath.h"
#include "StageSpanActor.generated.h"

/**
 * A placed linear run (truss line, railing, cable run) of a UStageSpanData: whole segments repeated along the actor's +X from
 * its origin (the start point), never one mesh scaled to length (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §2).
 *
 * State is one SaveGame value, the run length, so undo snapshots and scene files carry it with no extra format. Everything
 * else (segment count, pitch, instances) is derived from it and the catalog item, and rebuilt when either changes.
 *
 * - Quantity: the segment count. Session totals and the bill of materials count the item's specs once per segment.
 * - Length is edited through the Length parameter (inspector), validated by the GameMode like any edit: it is refused while the
 *   item is locked, and lengthening must fit the session's power and weight limits.
 * - Spans do not scale (SupportsScaling is false): their size is their length.
 *
 * Never ticks; the run is rebuilt only when the length or the item changes.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageSpanActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStageSpanActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Span")
	class UStageSpanData* GetSpanData() const;

	/** The run as built: segment count, pitch and real length for the current length and catalog item. */
	FStageSpanLayout GetLayout() const;

	/** The run's real length in cm (whole segments: a multiple of the segment length). */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Span")
	double GetSpanLength() const { return GetLayout().Length; }

	/**
	 * Sets the run length in cm; it is normalised by the item's rules (whole segments, limits). For setting up a run before it is
	 * finished spawning (the span tool). Edits of a placed run go through the Length parameter, which the GameMode validates.
	 */
	void InitializeSpanLength(double InLength);

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
	/** The layout SpanLength would have for the current item; invalid without span data. */
	FStageSpanLayout ComputeLayoutFor(double InLength) const;

	void SetSpanLength(double InLength);
	void RebuildGeometry();

	/** Requested run length in cm, stored normalised; 0 means one segment. Loaded files are clamped by the item's rules. */
	UPROPERTY(EditAnywhere, SaveGame, Category = "StageCraft|Span", meta = (ClampMin = "0.0", Units = "cm"))
	double SpanLength = 0.0;
};
