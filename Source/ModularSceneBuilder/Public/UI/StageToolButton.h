// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "StageToolButton.generated.h"

/**
 * A button that never takes keyboard focus, for toolbars and item lists in StageCraft panels.
 *
 * Clicking it leaves keyboard focus where it was, normally the game viewport, so viewport shortcuts (Esc, P, Space)
 * keep working right after the click. Focusability can only be set before the Slate widget exists, which is why
 * this is a subclass and not a setter call.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageToolButton : public UButton
{
	GENERATED_BODY()

public:
	UStageToolButton(const FObjectInitializer& ObjectInitializer);
};
