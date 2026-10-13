// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "StageSpanData.generated.h"

/**
 * How a span fills the length the user drew. Either way the run is made of whole repeated segments; nothing is scaled to
 * length as one mesh (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §2).
 */
UENUM(BlueprintType)
enum class EStageSpanFit : uint8
{
	/** The length snaps to a whole number of segments of exactly SegmentLength, like rental truss sections. */
	WholeSegments	UMETA(DisplayName = "Whole Segments"),
	/** The segment count is the nearest whole number and each segment stretches slightly to hit the exact length (cables, railings). */
	Stretch			UMETA(DisplayName = "Stretch to Length"),
};

/**
 * One mesh of a segment or a joint. A segment can be several parts (a box truss section is four chords and its lacing),
 * which is how a convincing run is built from simple meshes, and how one real segment mesh is used (a single part).
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSpanPart
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Span", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UStaticMesh> Mesh;

	/** Optional; the mesh's own material when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Span", meta = (AssetBundles = "Game"))
	TSoftObjectPtr<class UMaterialInterface> Material;

	/**
	 * Placement within the segment, in cm. Segments run along +X: the segment starts at X = 0 and ends at X = SegmentLength.
	 * Joint parts are placed relative to the joint point instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Span")
	FTransform Transform;
};

/**
 * Catalog entry for a linear element drawn from a start to an end point: truss runs, railings, cable runs
 * (AStageSpanActor, placed with UStageSpanToolComponent).
 *
 * The run repeats SegmentParts once per segment and JointParts at every joint, both ends included. Specs (weight, power) are
 * per SEGMENT: a 6 m run of 2 m truss counts three times, in session totals and in the bill of materials.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UStageSpanData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UStageSpanData();

	/** Upper bound for MaxSegments of any span asset, so a run can never create an unbounded number of instances. */
	static constexpr int32 AbsoluteMaxSegments = 500;

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Span")
	EStageSpanFit Fit = EStageSpanFit::WholeSegments;

	/** Length of one segment along +X; the repeat pitch of the run. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Span", meta = (ClampMin = "1.0", Units = "cm"))
	float SegmentLength = 200.f;

	/** Longest run, in segments. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Span", meta = (ClampMin = "1", ClampMax = "500"))
	int32 MaxSegments = 100;

	/** Placement snaps the run's direction to multiples of this angle (polar tracking). 0 = free direction. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Span", meta = (ClampMin = "0.0", ClampMax = "90.0", Units = "deg"))
	float AngleSnapDegrees = 15.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Span", meta = (TitleProperty = "Mesh"))
	TArray<FStageSpanPart> SegmentParts;

	/** Placed at every joint between segments and at both ends: couplers, railing posts, base plates. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Span", meta = (TitleProperty = "Mesh"))
	TArray<FStageSpanPart> JointParts;
};
