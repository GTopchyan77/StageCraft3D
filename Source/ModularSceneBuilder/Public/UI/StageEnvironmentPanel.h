// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Environment/StageEnvironmentTypes.h"
#include "StageEnvironmentPanel.generated.h"

/**
 * The Environment panel (StageCraft.Panel.Environment): one-click atmospheres (Docs/ADR/0024 §3), what lights the stage
 * (level, outdoor sky, studio), one-click weather presets, sliders for time of day, sun height, sun direction, clouds, rain,
 * snow and fog (Docs/ADR/0009-outdoor-environment.md §4), and the camera look (exposure, bloom, white balance).
 *
 * A view in the project's binding pattern:
 *  - Settings are read from UStageEnvironmentSubsystem and written back through SetSettings, its single write path; the
 *    panel shows them again from OnEnvironmentChanged (read-back), never from its own copy. A slider drag writes on every
 *    move, so the sky follows the slider live.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick. Sliders and buttons never take keyboard focus.
 *  - An atmosphere card goes through the controller (RequestApplyAtmosphere): one undo step, blended in. The card whose
 *    settings match the stored ones is lit.
 *  - Without a sky backend the Outdoor choice is disabled and the panel says why; time and weather are live only outdoors.
 *
 * The widget tree is built in code from the theme, like the Render panel; there is no Widget Blueprint.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageEnvironmentPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Environment")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildChoiceRow(const FText& Title, TConstArrayView<FText> Labels, TConstArrayView<FText> Tips,
		TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons, void (UStageEnvironmentPanel::*Handler)(int32));
	class UWidget* BuildSliderSection(const FText& Title, int32 FirstSlider, int32 SliderCount);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;

	void Refresh();
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;

	void RebuildAtmosphereCards();
	void StyleCard(class UStageChoiceButton& Button, bool bSelected) const;
	void HandleAtmospherePicked(int32 Index);
	void HandleLightingPicked(int32 Index);
	void HandleWeatherPicked(int32 Index);
	void HandleSliderMoved(int32 SliderIndex, float Value);

	UFUNCTION()
	void HandleEnvironmentChanged(const FStageEnvironmentSettings& Settings);

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> LightingButtons;

	/** One card per atmosphere preset, in the catalog's order; rebuilt when the presets load. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> AtmosphereButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageAtmospherePreset>> AtmospherePresets;

	UPROPERTY(Transient)
	TObjectPtr<class UUniformGridPanel> AtmosphereGrid = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WeatherButtons;

	/** Indexed by the panel's slider order (StageEnvironmentPanel::ESlider). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageValueSlider>> Sliders;

	/** The value shown next to each slider, same order as Sliders. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> ValueTexts;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> NoticeText = nullptr;

	TWeakObjectPtr<class UStageEnvironmentSubsystem> Environment;
	TWeakObjectPtr<class UStageAtmosphereSubsystem> AtmosphereCatalog;
	FDelegateHandle AtmospheresLoadedHandle;
};
