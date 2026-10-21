// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/SStageUnsavedChangesDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "StageUnsavedChangesDialog"

namespace StageUnsavedChangesDialog
{
	constexpr float Width = 420.f;
	constexpr float Padding = 16.f;
	constexpr float ButtonGap = 8.f;
}

void SStageUnsavedChangesDialog::Construct(const FArguments& InArgs)
{
	using namespace StageUnsavedChangesDialog;

	SceneName = InArgs._SceneName;
	OnSave = InArgs._OnSave;
	OnDiscard = InArgs._OnDiscard;
	OnCancel = InArgs._OnCancel;

	const bool bUntitled = SceneName.IsEmpty();
	const FText Message = bUntitled
		? LOCTEXT("UntitledMessage", "This stage has never been saved. Name it to save it, or close without saving.")
		: FText::Format(LOCTEXT("NamedMessage", "\"{0}\" has unsaved changes."), FText::FromString(SceneName));

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Save changes before closing?"))
			.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(Message)
			.AutoWrapText(true)
		];

	if (bUntitled)
	{
		Body->AddSlot()
			.AutoHeight()
			.Padding(0.f, 12.f, 0.f, 0.f)
			[
				SAssignNew(NameBox, SEditableTextBox)
				.HintText(LOCTEXT("NameHint", "Scene name"))
				.IsEnabled(this, &SStageUnsavedChangesDialog::AreButtonsEnabled)
				.OnTextCommitted(this, &SStageUnsavedChangesDialog::HandleNameCommitted)
			];
	}

	Body->AddSlot()
		.AutoHeight()
		.Padding(0.f, 16.f, 0.f, 0.f)
		.HAlign(HAlign_Right)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SAssignNew(SaveButton, SButton)
				.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
				.Text(LOCTEXT("Save", "Save"))
				.IsEnabled(this, &SStageUnsavedChangesDialog::AreButtonsEnabled)
				.OnClicked(this, &SStageUnsavedChangesDialog::HandleSave)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(ButtonGap, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Discard", "Don't Save"))
				.ToolTipText(LOCTEXT("DiscardTip", "Close StageCraft and lose the unsaved changes."))
				.IsEnabled(this, &SStageUnsavedChangesDialog::AreButtonsEnabled)
				.OnClicked(this, &SStageUnsavedChangesDialog::HandleDiscard)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(ButtonGap, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.ToolTipText(LOCTEXT("CancelTip", "Keep StageCraft open."))
				.IsEnabled(this, &SStageUnsavedChangesDialog::AreButtonsEnabled)
				.OnClicked(this, &SStageUnsavedChangesDialog::HandleCancel)
			]
		];

	Body->AddSlot()
		.AutoHeight()
		.Padding(0.f, 8.f, 0.f, 0.f)
		[
			SAssignNew(StatusText, STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FAppStyle::Get().GetSlateColor("Colors.Warning"))
			.Visibility(EVisibility::Collapsed)
		];

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(Width)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
			.Padding(Padding)
			[
				Body
			]
		]
	];
}

void SStageUnsavedChangesDialog::SetError(const FText& Message)
{
	StatusText->SetText(Message);
	StatusText->SetColorAndOpacity(FAppStyle::Get().GetSlateColor("Colors.Warning"));
	StatusText->SetVisibility(Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
}

void SStageUnsavedChangesDialog::SetBusy(bool bInBusy)
{
	bBusy = bInBusy;
	if (bBusy)
	{
		StatusText->SetText(LOCTEXT("Saving", "Saving..."));
		StatusText->SetColorAndOpacity(FAppStyle::Get().GetSlateColor("Colors.Foreground"));
		StatusText->SetVisibility(EVisibility::Visible);
	}
}

TSharedPtr<SWidget> SStageUnsavedChangesDialog::GetInitialFocus() const
{
	return NameBox.IsValid() ? StaticCastSharedPtr<SWidget>(NameBox) : StaticCastSharedPtr<SWidget>(SaveButton);
}

FReply SStageUnsavedChangesDialog::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Escape is Cancel, as in every dialog. Other keys stop here: nothing behind the dialog may act while it is open.
	if (InKeyEvent.GetKey() == EKeys::Escape && !bBusy)
	{
		return HandleCancel();
	}
	return FReply::Handled();
}

FReply SStageUnsavedChangesDialog::HandleSave()
{
	OnSave.ExecuteIfBound(NameBox.IsValid() ? NameBox->GetText().ToString().TrimStartAndEnd() : SceneName);
	return FReply::Handled();
}

FReply SStageUnsavedChangesDialog::HandleDiscard()
{
	OnDiscard.ExecuteIfBound();
	return FReply::Handled();
}

FReply SStageUnsavedChangesDialog::HandleCancel()
{
	OnCancel.ExecuteIfBound();
	return FReply::Handled();
}

void SStageUnsavedChangesDialog::HandleNameCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter && !bBusy)
	{
		HandleSave();
	}
}

#undef LOCTEXT_NAMESPACE
