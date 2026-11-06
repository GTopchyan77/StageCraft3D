// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "People/StageAudienceMath.h"
#include "StageAudienceActor.generated.h"

/**
 * A placed audience area of a UStageAudienceData (Docs/ADR/0031 §4): a rectangle of floor filled with an animated crowd.
 *
 * - The actor's origin is the middle of the area's front edge; the area runs back along +X and everyone faces -X, so turn the
 *   item to face the stage.
 * - People are instances of the shared figure mesh in one hierarchical instanced component (culled and LOD-switched per cluster),
 *   playing their clips on the GPU (UStageFigureAssetSubsystem). No actors per person, no tick, no shadows.
 * - State is four SaveGame values: width, depth, density and mood, plus the seed that makes the same crowd come back when a scene
 *   is opened again. Restored values are untrusted and sanitized by the item's ranges when the item data is applied; every repair
 *   is counted.
 * - Width, Depth, Density and Mood are inspector parameters: undoable shape edits, refused while the item is locked. People is
 *   read-only.
 * - Picking: a box covering the floor of the area, 2 cm high, answers only the stage item channel. Clicking the crowd selects
 *   it (the ray reaches the floor under a person); placement, camera pivots and fixtures above or beyond the crowd are not in
 *   the way.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AStageAudienceActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	AStageAudienceActor();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audience")
	class UStageAudienceData* GetAudienceData() const;

	/** People shown. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Audience")
	int32 GetPeopleCount() const;

	/** Width (across the stage) by depth, in cm, as built. */
	FVector2D GetAreaSize() const { return AreaSize; }

	double GetDensity() const { return Density; }
	EStageCrowdMood GetMood() const { return Mood; }

	//~ Begin AModularBaseActor Interface
	virtual bool SupportsScaling() const override { return false; }
	virtual void PrepareAsDuplicate() override;
	//~ End AModularBaseActor Interface

protected:
	//~ Begin AModularBaseActor Interface
	virtual void ApplyItemData(const class UBaseItemData& Data) override;
	virtual void GatherParameterSections(TArray<FStageParameterSection>& OutSections) const override;
	virtual bool ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const override;
	virtual bool WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value) override;
	//~ End AModularBaseActor Interface

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UHierarchicalInstancedStaticMeshComponent> Crowd = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UBoxComponent> PickArea = nullptr;

private:
	/** Brings the saved values within the item's rules (its defaults for a fresh actor); reports repairs. */
	void SanitizeState(const class UStageAudienceData& Audience);

	/** Refills the crowd from the current state; also resizes the pick box. */
	void RebuildCrowd();

	/** Width and depth in cm. Zero on a fresh actor until the item gives it its default size. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Audience", meta = (Units = "cm"))
	FVector2D AreaSize = FVector2D::ZeroVector;

	/** People per m². 0 on a fresh actor, which takes the item's default. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Audience")
	double Density = 0.0;

	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Audience")
	EStageCrowdMood Mood = EStageCrowdMood::Count;

	/** Which crowd: chosen once when the area is first placed or duplicated, then saved with it. 0 means not chosen yet. */
	UPROPERTY(VisibleInstanceOnly, SaveGame, Category = "StageCraft|Audience")
	int32 Seed = 0;
};
