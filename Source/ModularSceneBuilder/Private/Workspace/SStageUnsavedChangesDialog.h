// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FOnStageDialogSave, const FString& /*SceneName*/);

/**
 * "Save changes before closing?" with Save / Don't Save / Cancel (Docs/ADR/0010-edit-rules-autosave-scene-v2.md §3.2).
 * Shown by UStageWorkspaceSubsystem over the whole workspace (FStageWorkspaceShell::ShowDialog) when the main window is closed
 * with unsaved changes. Pure view: it only reports the choice; the owner saves, quits or dismisses it.
 *
 * A stage that was never saved needs a name, so the dialog then shows a name field (Enter = Save). While the owner saves,
 * SetBusy disables the buttons; a failed save comes back through SetError and the dialog stays open.
 */
class SStageUnsavedChangesDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStageUnsavedChangesDialog) {}
		/** The stage's scene name; empty for a stage that was never saved (a name field is shown). */
		SLATE_ARGUMENT(FString, SceneName)
		SLATE_EVENT(FOnStageDialogSave, OnSave)
		SLATE_EVENT(FSimpleDelegate, OnDiscard)
		SLATE_EVENT(FSimpleDelegate, OnCancel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows Message under the buttons (empty hides it). */
	void SetError(const FText& Message);

	/** While a save runs: buttons disabled, "Saving..." shown. */
	void SetBusy(bool bInBusy);

	/** The widget that should take keyboard focus when the dialog opens: the name field, or the Save button. */
	TSharedPtr<SWidget> GetInitialFocus() const;

	//~ Begin SWidget Interface
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	//~ End SWidget Interface

private:
	FReply HandleSave();
	FReply HandleDiscard();
	FReply HandleCancel();
	void HandleNameCommitted(const FText& Text, ETextCommit::Type CommitType);
	bool AreButtonsEnabled() const { return !bBusy; }

	FString SceneName;
	FOnStageDialogSave OnSave;
	FSimpleDelegate OnDiscard;
	FSimpleDelegate OnCancel;

	TSharedPtr<class SEditableTextBox> NameBox;
	TSharedPtr<class SButton> SaveButton;
	TSharedPtr<class STextBlock> StatusText;
	bool bBusy = false;
};
