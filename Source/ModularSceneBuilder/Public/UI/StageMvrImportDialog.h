// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Mvr/StageMvrImportTypes.h"
#include "StageMvrImportDialog.generated.h"

DECLARE_DELEGATE(FOnStageMvrImportDialogClosed);

/**
 * The MVR import review (Docs/ADR/0034 §6, MV3): what an .mvr file holds (layers, fixtures, trusses, focus points, other objects),
 * each GDTF type it uses (new to the Library, already in it, or unusable and placed as a generic fixture), the parser's warnings and
 * the errors that refuse it. For a plot already on the stage (§18, MV5) it shows what is new, changed by the file, unchanged and no
 * longer in the file, with a toggle to remove or keep the latter, and its confirm reads UPDATE. While the rig is placed it shows the
 * progress; it closes when the import ends.
 *
 * A view in the project's binding pattern:
 *  - Reads UStageMvrImportSubsystem (ShowState, called by the host when the subsystem changes). Display only: the controller and
 *    the subsystem check everything again on confirm.
 *  - Writes go through the owning controller: IMPORT calls RequestConfirmMvrImport (with the remove / keep choice), CANCEL and Esc
 *    RequestCancelMvrImport. A refused confirm keeps the review open with the reason. The remove / keep toggle is view state until then.
 *  - OnClosed fires once the review was cancelled; the host closes the dialog when the subsystem has no review.
 *
 * Built in code from the theme; hosted by UStageWorkspaceSubsystem over the workspace. No tick.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageMvrImportDialog : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows the importer's review and state (Review: the buttons; Placing: the progress). */
	void ShowState(const class UStageMvrImportSubsystem& Importer);

	const FGuid& GetReviewId() const { return ReviewId; }

	/** The widget the host focuses: IMPORT, or CANCEL when the file cannot be imported. */
	class UWidget* GetInitialFocus() const;

	FOnStageMvrImportDialogClosed OnClosed;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Import")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	enum class EButton : int32 { Confirm, Cancel, Missing, Count };

	void BuildTree();
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, bool bWrap = false);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, EButton Index);
	void StyleButton(class UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const;
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void HandleButton(int32 Index);
	void Confirm();
	void Cancel();
	void ToggleMissing();
	/** IMPORT / UPDATE and the remove / keep toggle, from the review and the choice. */
	void RefreshButtons();

	FGuid ReviewId;
	bool bCanImport = false;
	bool bReimport = false;
	int32 MissingItems = 0;
	/** Items of the plot the file no longer has: removed unless the user switches the toggle (Docs/ADR/0034 §18). */
	EStageMvrMissingItems Missing = EStageMvrMissingItems::Remove;
	/** The review's own verdict and whether confirming would change anything with the current choice. */
	bool bReviewCanImport = false;
	bool bHasChangesRemoving = false;
	bool bHasChangesKeeping = false;
	bool bPlacing = false;
	bool bClosed = false;
	/** Notes the review had when it was last drawn: the review is redrawn only when they change (the plan adds its own), never per frame. */
	int32 ShownNotes = INDEX_NONE;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> TitleText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SourceText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SummaryText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> TypeList = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> NotesText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;
	/** Indexed by EButton. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> Buttons;
};
