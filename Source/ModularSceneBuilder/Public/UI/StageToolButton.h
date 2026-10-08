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

/** Reports which option of a segmented choice was picked. */
DECLARE_DELEGATE_OneParam(FOnStageChoicePicked, int32 /*ChoiceIndex*/);

/**
 * One option of a segmented choice (e.g. a resolution preset in the Render panel). A plain UButton's click does not say
 * which button it was, so each option carries its index and reports it on OnPicked. Never keyboard-focusable, like its parent.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageChoiceButton : public UStageToolButton
{
	GENERATED_BODY()

public:
	UStageChoiceButton(const FObjectInitializer& ObjectInitializer);

	//~ Begin UObject Interface
	virtual void PostInitProperties() override;
	//~ End UObject Interface

	void SetChoiceIndex(int32 InChoiceIndex) { ChoiceIndex = InChoiceIndex; }
	int32 GetChoiceIndex() const { return ChoiceIndex; }

	/** Bound by the panel that owns the choice; unbound when the panel is destroyed with it. */
	FOnStageChoicePicked OnPicked;

private:
	UFUNCTION()
	void HandleClicked();

	int32 ChoiceIndex = INDEX_NONE;
};
