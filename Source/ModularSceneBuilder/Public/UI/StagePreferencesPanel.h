// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Input/StageKeyBindings.h"
#include "StagePreferencesPanel.generated.h"

/**
 * The Preferences panel (StageCraft.Panel.Preferences, Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.5): camera
 * look, viewport and audio preferences and the keyboard shortcuts. Opens from Edit › Preferences… or the Window menu.
 *
 * A view in the project's binding pattern:
 *  - Every value is read from its owner and written back through that owner's single write path: the key map and look
 *    preferences through UStageInputPreferencesSubsystem, snapping through UStageSnappingComponent, the grid through
 *    UStageViewportComponent, audio through UStageAudioSubsystem. The panel re-reads after each owner's change event, so a
 *    refused or clamped change shows the stored value. It never writes UStageCraftUserSettings.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick.
 *
 * Capturing a shortcut: clicking a binding button arms that slot and gives the panel keyboard focus, so the keys never reach
 * the viewport. The next key that is not a modifier, with the modifiers held, becomes the binding. CLEAR unbinds the slot;
 * losing focus (a click elsewhere) cancels. A refusal (conflict, unsupported key) is shown and changes nothing.
 *
 * The widget tree is built in code from the theme, like the Environment panel; there is no Widget Blueprint.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePreferencesPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void NativeOnFocusLost(const FFocusEvent& InFocusEvent) override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Preferences")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** The slot being captured. */
	struct FCaptureTarget
	{
		EStageInputCommand Command = EStageInputCommand::Count;
		EStageKeySlot Slot = EStageKeySlot::Primary;
	};

	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildSectionTitle(const FText& Title);
	class UWidget* BuildChoiceRow(const FText& Label, const FText& Tip, TConstArrayView<FText> Choices,
		TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons, void (UStagePreferencesPanel::*Handler)(int32));
	class UWidget* BuildSliderRow(const FText& Label, const FText& Tip, int32 SliderIndex, float Min, float Max);
	class UWidget* BuildShortcutSection();
	class UWidget* BuildCommandRow(const struct FStageInputCommandInfo& Info);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;

	void Refresh();
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;
	void SetNotice(const FText& Text, bool bWarning);

	void HandleLookInvertPicked(int32 Index);
	void HandleLookLinkPicked(int32 Index);
	void HandleSnapItemsPicked(int32 Index);
	void HandleGridPicked(int32 Index);
	void HandleMutePicked(int32 Index);
	void HandleSliderMoved(int32 SliderIndex, float Value);

	/** A binding button: Index = command * 2 + slot. Arms capture for that slot (or disarms it when it is already armed). */
	void HandleBindingPicked(int32 Index);
	void HandleResetPicked(int32 CommandIndex);
	void HandleClearPicked(int32 Unused);
	void HandleResetAllPicked(int32 Unused);

	void BeginCapture(const FCaptureTarget& Target);
	void EndCapture();

	UFUNCTION()
	void HandlePreferencesChanged();

	class UStageInputPreferencesSubsystem* GetInputPreferences() const;
	class UStageAudioSubsystem* GetAudio() const;
	class UStageSnappingComponent* GetSnapping() const;
	class UStageViewportComponent* GetViewport() const;

	TOptional<FCaptureTarget> Capture;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> LookInvertButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> LookLinkButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> SnapItemsButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> GridButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> MuteButtons;

	/** Look sensitivity, master volume, effects volume (StagePreferencesPanel::ESlider order). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageValueSlider>> Sliders;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> SliderValues;

	/** Two per command, indexed command * 2 + slot. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> BindingButtons;

	/** One per command. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ResetButtons;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> ClearButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> NoticeText = nullptr;

	TWeakObjectPtr<class UStageInputPreferencesSubsystem> BoundPreferences;
	TWeakObjectPtr<class UStageAudioSubsystem> BoundAudio;
	TWeakObjectPtr<class UStageSnappingComponent> BoundSnapping;
	TWeakObjectPtr<class UStageViewportComponent> BoundViewport;
};
