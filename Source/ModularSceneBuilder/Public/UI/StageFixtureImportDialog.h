// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Fixtures/StageFixtureLibraryTypes.h"
#include "StageFixtureImportDialog.generated.h"

DECLARE_DELEGATE(FOnStageFixtureImportDialogClosed);

/**
 * The GDTF import report (Docs/ADR/0033 §8, §10): shown for each fixture file the library has read, before anything is added. It
 * shows the type, the manufacturer and the file, each DMX mode with its footprint and how many channels are mapped, approximated or
 * kept at their default (with their names), the warnings and the errors that refuse the file, and an editable fixture kind.
 *
 * A view in the project's binding pattern:
 *  - Reads the review it is given (a copy) and StageFixtureLibraryView::DescribeReview for its words. Display only: the library
 *    checks everything again on confirm.
 *  - Writes go through the owning controller: ADD or REPLACE calls RequestConfirmFixtureImport (with the kind when the user changed
 *    it), CANCEL and Esc call RequestCancelFixtureImport. A refused confirm keeps the dialog open with the reason.
 *  - OnClosed fires once the review was confirmed or cancelled; the owner removes the dialog and shows the next review.
 *
 * Built in code from the theme (a Blueprint subclass may restyle it); hosted by UStageWorkspaceSubsystem over the workspace.
 */
UCLASS(Blueprintable, meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageFixtureImportDialog : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows Review. Called by the host right after creation, and again if the same review changed (bReplaces). */
	void ShowReview(const FStageFixtureImportReview& InReview);

	const FGuid& GetReviewId() const { return Review.ReviewId; }

	/** The widget the host focuses: the confirm button, or CANCEL when the file cannot be added. */
	class UWidget* GetInitialFocus() const;

	FOnStageFixtureImportDialogClosed OnClosed;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Fixtures")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	enum class EButton : int32 { Confirm, Cancel, Count };

	void BuildTree();
	void Refresh();
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, bool bWrap = false);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, EButton Index);
	void StyleButton(class UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const;
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void HandleButton(int32 Index);
	void Confirm();
	void Cancel();

	UFUNCTION()
	void HandleKindChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	FStageFixtureImportReview Review;
	/** The kind picked in the dropdown; unset while it shows the inferred kind. */
	TOptional<EStageFixtureKind> CorrectedKind;
	bool bClosed = false;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> TitleText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SourceText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SummaryText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> KindCombo = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> ModeList = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> NotesText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;
	/** Indexed by EButton. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> Buttons;
};
