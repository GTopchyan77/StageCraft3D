// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/StageParameterTypes.h"
#include "StageParameterControlWidget.generated.h"

DECLARE_DELEGATE_TwoParams(FOnStageParameterControlCommitted, const FGameplayTag& /*ParameterId*/, const FStageParameterValue& /*Value*/);

/**
 * Base of every widget that shows and edits one parameter: inspector rows, faders, encoders.
 *
 * Owns the two-way binding contract so each control only maps a value to its widgets:
 *  - RefreshValue pushes a value from the inspected object in. Widgets the user is interacting with
 *    (keyboard focus inside, or mouse capture: typing, dragging) are skipped, so a cue or DMX update
 *    never yanks a fader or overwrites half-typed text. The value is still stored.
 *  - CommitValue sends a user edit out (never while the control is writing its own widgets).
 *  - FinishInteraction re-applies the latest stored value to every widget when an edit ends, so a
 *    clamped or rejected edit always settles on what the object really holds.
 *
 * Owners (UStageParameterViewWidget) bind OnCommitted and call RefreshValue; controls never talk to
 * the inspected object themselves.
 */
UCLASS(Abstract, Blueprintable)
class MODULARSCENEBUILDER_API UStageParameterControlWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Configures the control for a parameter and shows its current value. */
	void InitializeControl(const FStageParameterDescriptor& InDescriptor, const FLinearColor& InGroupColor);

	/** Pushes a value from the inspected object into the control without echoing it back as a commit. */
	void RefreshValue(const FStageParameterValue& InValue);

	/** Sends a user edit to the inspected object. Values are in stored units (not display units). */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Inspector")
	void CommitValue(const FStageParameterValue& InValue);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	const FStageParameterDescriptor& GetDescriptor() const { return Descriptor; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	FGameplayTag GetParameterId() const { return Descriptor.Id; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	FLinearColor GetGroupColor() const { return GroupColor; }

	/** Human-readable value in display units, e.g. "75.0 %", "(120, 0, 350) cm", or the chosen option. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Inspector")
	static FText FormatValue(const FStageParameterDescriptor& InDescriptor, const FStageParameterValue& InValue);

	FOnStageParameterControlCommitted OnCommitted;

protected:
	/** Builds/configures widgets for Descriptor. Called once, before the first ApplyValue. */
	virtual void OnInitializeControl() {}

	/**
	 * Maps a value onto the control's widgets. Unless bForce, skip any widget for which
	 * IsUserInteracting is true. Runs with commits suppressed, so widget change events are not echoed.
	 */
	virtual void ApplyValue(const FStageParameterValue& InValue, bool bForce) {}

	/** Call when the user ends an edit (drag released, text committed). */
	void FinishInteraction();

	/** True while the user is typing into or dragging the widget (keyboard focus within it, or mouse capture). */
	static bool IsUserInteracting(const class UWidget* Widget);

	/** Display value (stored * DisplayScale) of a numeric value; 0 for non-numeric types. */
	double ToDisplay(const FStageParameterValue& InValue) const;

	/** Numeric value of the right type from display units, rounded for Integer parameters. */
	FStageParameterValue FromDisplay(double DisplayValue) const;

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Control Initialized"))
	void BP_OnControlInitialized(const FStageParameterDescriptor& InDescriptor, FLinearColor InGroupColor);

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|Inspector", meta = (DisplayName = "On Value Refreshed"))
	void BP_OnValueRefreshed(const FStageParameterValue& InValue);

	FStageParameterDescriptor Descriptor;
	FLinearColor GroupColor = FLinearColor::White;

private:
	void ApplyValueGuarded(bool bForce);

	/** Set while the control writes widget values itself, so widget change events are not sent back as edits. */
	bool bSuppressCommit = false;
};
