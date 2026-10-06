// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One alignment guide line in world space, drawn while an item snaps to a neighbour. */
struct FStageSnapGuide
{
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	/** Line thickness in cm, chosen for the current camera distance. */
	double Thickness = 1.0;
};

using FStageSnapGuideList = TArray<FStageSnapGuide, TInlineAllocator<3>>;

/** Result of an object-snap query: where the item goes, and which guides explain why. */
struct FStageSnapOutcome
{
	/** Final world location of the item's pivot. */
	FVector Location = FVector::ZeroVector;

	/** True when at least one axis locked onto a neighbour. False means Location is the caller's own proposal. */
	bool bSnapped = false;

	FStageSnapGuideList Guides;
};

/** Object snapping for the placement preview: returns the landing transform's location from the free (unsnapped) and grid-snapped candidates. */
DECLARE_DELEGATE_RetVal_ThreeParams(FStageSnapOutcome, FStagePlacementSnapper, const class UBaseItemData& /*Item*/, const FTransform& /*FreeTransform*/, const FTransform& /*GridTransform*/);

/** Object snapping for a gizmo translation: ProposedLocation of the dragged target, restricted to the axes in AxisMask (StageSnapMath axis bits). */
DECLARE_DELEGATE_RetVal_TwoParams(FStageSnapOutcome, FStageTranslationSnapper, const FVector& /*ProposedLocation*/, uint8 /*AxisMask*/);
