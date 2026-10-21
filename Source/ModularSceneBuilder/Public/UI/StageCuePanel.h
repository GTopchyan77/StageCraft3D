// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/SpinBox.h"
#include "Show/ShowControlTypes.h"
#include "StageCuePanel.generated.h"

DECLARE_DELEGATE_TwoParams(FOnStageCueLabelCommitted, float /*CueNumber*/, const FText& /*Label*/);
DECLARE_DELEGATE_TwoParams(FOnStageCueFadeCommitted, float /*CueNumber*/, float /*FadeTime*/);

/** A cue row's label field. Commits on Enter or focus loss (not Escape), and says which cue it belongs to. */
UCLASS()
class MODULARSCENEBUILDER_API UStageCueLabelField : public UEditableTextBox
{
	GENERATED_BODY()

public:
	void Configure(float InCueNumber);

	/** Bound by the owning panel. */
	FOnStageCueLabelCommitted OnLabelCommitted;

private:
	UFUNCTION()
	void HandleTextCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	float CueNumber = 0.f;
};

/** A cue row's fade time field. Commits when the user finishes (Enter, focus loss, end of a drag), never while dragging. */
UCLASS()
class MODULARSCENEBUILDER_API UStageCueFadeField : public USpinBox
{
	GENERATED_BODY()

public:
	void Configure(float InCueNumber);

	/** Bound by the owning panel. */
	FOnStageCueFadeCommitted OnFadeCommitted;

private:
	UFUNCTION()
	void HandleValueCommitted(float InValue, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleEndSliderMovement(float InValue);

	float CueNumber = 0.f;
};

/**
 * The cue list dock panel (StageCraft.Panel.Cues, Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.6), laid out like a
 * console's playback and cue list windows:
 *  - transport: GO, BACK, the active cue and the control source (Internal cue list / External DMX);
 *  - record: cue number, label and fade time, and STORE (or RE-RECORD when that number exists);
 *  - the cue list: number, label and fade (edited in place), fixture count, GO TO and DELETE per cue; the active cue is marked.
 *
 * A view in the project's binding pattern:
 *  - Reads UShowControlSubsystem; owns no cue state.
 *  - Push, never poll: rebuilds on the next tick after OnCueListChanged, OnCueStarted, OnCueFinished or OnControlSourceChanged.
 *  - Every write goes through AModularPlayerController (RequestStoreCue, RequestUpdateCue, ...), which asks the GameMode and
 *    records undo; a refusal is reported on OnRequestRejected (status bar), and the rebuild shows the real state again.
 *  - Bindings are made in NativeConstruct and removed in NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageCuePanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Cues")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildTransport();
	class UWidget* BuildRecordRow();
	class UWidget* MakeColumnTitles();
	class UWidget* MakeCueRow(const FShowCue& Cue, bool bActive, int32 RowIndex);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bPrimary);
	/** Outlined, or filled with the accent colour when highlighted (the primary action, or the chosen option of a segment). */
	FButtonStyle MakeButtonStyle(bool bHighlighted) const;
	void SetSegmentActive(class UStageChoiceButton& Button, bool bActive);
	class UWidget* MakeCell(class UWidget& Content, float Width);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class USpinBox* MakeNumberField(float Value, float Min, float Max, float Delta, int32 Digits, const FText& ToolTip);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void RequestRebuild();
	void Rebuild();

	/** The record row's STORE button names what it will do: store a new cue, or re-record an existing one. */
	void RefreshStoreButton();

	/** The next whole cue number after the last cue (1 for an empty list), offered in the record row after each store. */
	float GetSuggestedCueNumber() const;

	UFUNCTION()
	void HandleCueListChanged();

	UFUNCTION()
	void HandleCueEvent(float CueNumber);

	UFUNCTION()
	void HandleControlSourceChanged(EStageControlSource NewSource);

	UFUNCTION()
	void HandleRecordNumberChanged(float InValue);

	void HandleTransportPicked(int32 ChoiceIndex);
	void HandleSourcePicked(int32 ChoiceIndex);
	void HandleStorePicked(int32 ChoiceIndex);
	void HandleGoToPicked(int32 RowIndex);
	void HandleDeletePicked(int32 RowIndex);
	void HandleLabelCommitted(float CueNumber, const FText& Label);
	void HandleFadeCommitted(float CueNumber, float FadeTime);

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> ListBody = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SubtitleText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ActiveCueText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USpinBox> RecordNumberField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> RecordLabelField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USpinBox> RecordFadeField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> StoreButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StoreButtonText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> GoButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> BackButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> InternalButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> ExternalButton = nullptr;

	/** Cue numbers in list order, matched to a clicked row button by index. */
	TArray<float> RowCueNumbers;

	TWeakObjectPtr<class UShowControlSubsystem> ShowControl;
	FTimerHandle RebuildTimer;
};
