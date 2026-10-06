// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StagePlacementTypes.generated.h"

/**
 * What the primary mouse button does in the stage viewport. Exactly one mode is active per local player.
 * Select: gizmo handle drag > select item > deselect. Never places and never moves an item on a plain click.
 * Place: one click places exactly one instance of the armed catalog item, then the mode returns to Select.
 */
UENUM(BlueprintType)
enum class EStageEditMode : uint8
{
	Select,
	Place,
};

/** What the placement preview currently shows. Display only: every placement is still validated on commit. */
UENUM(BlueprintType)
enum class EStagePlacementPreviewState : uint8
{
	/** Not in Place mode, no item armed, or the cursor is not over a surface. */
	Hidden,
	/** Ghost and landing marker shown; the rules currently allow the item. */
	Valid,
	/** Ghost and landing marker shown in the refusal colour (e.g. the item is locked or a session limit is reached). */
	Refused,
};
