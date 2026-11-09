// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageFixtureImportDialog.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Fixtures/StageFixtureLibraryView.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageFixtureImportDialog)

#define LOCTEXT_NAMESPACE "StageFixtureImportDialog"

namespace StageFixtureImportDialog
{
	constexpr float CardWidth = 520.f;
	constexpr float ModeListHeight = 220.f;
	constexpr float SectionGap = 10.f;

	int32 KindCount()
	{
		return StaticEnum<EStageFixtureKind>()->NumEnums() - 1;
	}

	FText KindName(int32 Index)
	{
		return StaticEnum<EStageFixtureKind>()->GetDisplayNameTextByIndex(Index);
	}
}

void UStageFixtureImportDialog::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageFixtureImportDialog::NativeDestruct()
{
	for (UStageChoiceButton* Button : Buttons)
	{
		if (Button)
		{
			Button->OnPicked.Unbind();
		}
	}
	if (KindCombo)
	{
		KindCombo->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleKindChanged);
	}
	OnClosed.Unbind();
	Super::NativeDestruct();
}

const UStageCraftUITheme& UStageFixtureImportDialog::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageFixtureImportDialog::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStageFixtureImportDialog::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, bool bWrap)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	Block->SetAutoWrapText(bWrap);
	return Block;
}

UStageChoiceButton* UStageFixtureImportDialog::MakeButton(const FText& Label, const FText& Tip, EButton Index)
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(int32(Index));
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, &ThisClass::HandleButton);
	UTextBlock* Text = MakeText(Label, 8, TEXT("Bold"), GetTheme().TextSecondary);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(14.f, 5.f));
	}
	return Button;
}

void UStageFixtureImportDialog::StyleButton(UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	FButtonStyle Style = bPrimary
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bPrimary)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Style.SetDisabled(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	Button.SetStyle(Style);
	Button.SetIsEnabled(bEnabled);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bPrimary && bEnabled ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageFixtureImportDialog::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("DialogSize"));
	Size->SetWidthOverride(StageFixtureImportDialog::CardWidth);
	WidgetTree->RootWidget = Size;

	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("DialogCard"));
	Card->SetBrush(MakeBox(Palette.PanelBackground, CornerRadius, Palette.AccentColor, 1.f));
	Card->SetPadding(FMargin(16.f, 12.f));
	Size->SetContent(Card);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DialogColumn"));
	Card->SetContent(Column);
	const auto Add = [Column](UWidget* Widget, float Top = 3.f) { Column->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, Top, 0.f, 0.f)); };

	Add(MakeText(LOCTEXT("Heading", "IMPORT FIXTURE (GDTF)"), 9, TEXT("Bold"), Palette.AccentColor), 0.f);
	TitleText = MakeText(FText::GetEmpty(), 13, TEXT("Bold"), Palette.TextPrimary);
	TitleText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(TitleText, 6.f);
	SourceText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	SourceText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(SourceText);
	SummaryText = MakeText(FText::GetEmpty(), 9, TEXT("Regular"), Palette.TextPrimary, true);
	Add(SummaryText, 6.f);

	UHorizontalBox* KindRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("KindRow"));
	UTextBlock* KindLabel = MakeText(LOCTEXT("KindLabel", "KIND"), 7, TEXT("Bold"), Palette.TextSecondary);
	KindLabel->SetToolTipText(LOCTEXT("KindTip", "What kind of fixture this is. StageCraft guesses it from the file; correct it if the guess is wrong. It sets the Library group and the fixture's body."));
	if (UHorizontalBoxSlot* LabelSlot = KindRow->AddChildToHorizontalBox(KindLabel))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
		LabelSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
	KindCombo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass(), TEXT("KindCombo"));
	KindCombo->ApplyTheme(Palette);
	KindCombo->SetToolTipText(KindLabel->GetToolTipText());
	for (int32 Index = 0; Index < StageFixtureImportDialog::KindCount(); ++Index)
	{
		KindCombo->AddOption(StageFixtureImportDialog::KindName(Index).ToString());
	}
	KindCombo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleKindChanged);
	if (UHorizontalBoxSlot* ComboSlot = KindRow->AddChildToHorizontalBox(KindCombo))
	{
		ComboSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	Add(KindRow, StageFixtureImportDialog::SectionGap);

	Add(MakeText(LOCTEXT("ModesLabel", "DMX MODES"), 7, TEXT("Bold"), Palette.TextSecondary), StageFixtureImportDialog::SectionGap);
	USizeBox* ModeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ModeBox"));
	ModeBox->SetMaxDesiredHeight(StageFixtureImportDialog::ModeListHeight);
	UScrollBox* ModeScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("ModeScroll"));
	ModeScroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	ModeList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ModeList"));
	ModeScroll->AddChild(ModeList);
	ModeBox->SetContent(ModeScroll);
	Add(ModeBox);

	NotesText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.WarningColor, true);
	Add(NotesText, StageFixtureImportDialog::SectionGap);
	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.WarningColor, true);
	Add(StatusText);

	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Footer"));
	Footer->AddChildToHorizontalBox(WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Buttons.SetNum(int32(EButton::Count));
	Buttons[int32(EButton::Cancel)] = MakeButton(LOCTEXT("Cancel", "CANCEL"), LOCTEXT("CancelTip", "Close without adding anything (Esc)."), EButton::Cancel);
	Buttons[int32(EButton::Confirm)] = MakeButton(LOCTEXT("AddLabel", "ADD"), LOCTEXT("ConfirmTip", "Add the fixture's modes to the Library (Enter). The file is copied into the Library, so it is there at the next start."), EButton::Confirm);
	Footer->AddChildToHorizontalBox(Buttons[int32(EButton::Cancel)]);
	if (UHorizontalBoxSlot* ConfirmSlot = Footer->AddChildToHorizontalBox(Buttons[int32(EButton::Confirm)]))
	{
		ConfirmSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}
	Add(Footer, 12.f);
}

void UStageFixtureImportDialog::ShowReview(const FStageFixtureImportReview& InReview)
{
	if (Review.ReviewId != InReview.ReviewId)
	{
		CorrectedKind.Reset();
		bClosed = false;
		if (StatusText)
		{
			StatusText->SetText(FText::GetEmpty());
		}
	}
	Review = InReview;
	Refresh();
}

void UStageFixtureImportDialog::Refresh()
{
	if (!TitleText)
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	const StageFixtureLibraryView::FReviewText Text = StageFixtureLibraryView::DescribeReview(Review);
	TitleText->SetText(Text.Title);
	TitleText->SetToolTipText(Text.Title);
	SourceText->SetText(Text.Source);
	SourceText->SetToolTipText(FText::FromString(Review.SourceFile));
	SummaryText->SetText(Text.Summary);

	KindCombo->SetSelectedIndex(int32(CorrectedKind.Get(Review.InferredKind)));
	KindCombo->SetIsEnabled(Text.bCanConfirm);

	ModeList->ClearChildren();
	for (const StageFixtureLibraryView::FModeRow& Row : Text.Modes)
	{
		UVerticalBox* Entry = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Entry->AddChildToVerticalBox(MakeText(Row.Name, 9, TEXT("Bold"), Row.bUsable ? Palette.TextPrimary : Palette.TextSecondary));
		Entry->AddChildToVerticalBox(MakeText(Row.Detail, 8, TEXT("Regular"), Row.bUsable ? Palette.TextSecondary : Palette.WarningColor, true));
		for (const FText& Note : Row.Notes)
		{
			Entry->AddChildToVerticalBox(MakeText(Note, 8, TEXT("Regular"), Palette.TextSecondary, true));
		}
		ModeList->AddChildToVerticalBox(Entry)->SetPadding(FMargin(0.f, 3.f, 0.f, 3.f));
	}

	TArray<FString> Notes;
	for (const FText& Error : Text.Errors)
	{
		Notes.Add(FText::Format(LOCTEXT("ErrorLine", "Error: {0}"), Error).ToString());
	}
	for (const FText& Warning : Text.Warnings)
	{
		Notes.Add(FText::Format(LOCTEXT("WarningLine", "Note: {0}"), Warning).ToString());
	}
	NotesText->SetText(FText::FromString(FString::Join(Notes, TEXT("\n"))));
	NotesText->SetVisibility(Notes.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);

	UStageChoiceButton* ConfirmButton = Buttons[int32(EButton::Confirm)];
	if (UTextBlock* Label = Cast<UTextBlock>(ConfirmButton->GetContent()))
	{
		Label->SetText(Text.ConfirmLabel);
	}
	ConfirmButton->SetToolTipText(Review.bReplaces
		? LOCTEXT("ReplaceTip", "Replace the fixture type in the Library with this file (Enter). Placed fixtures move to the mode with the same name; their patch and programming stay.")
		: LOCTEXT("AddTip", "Add the fixture's modes to the Library (Enter). The file is copied into the Library, so it is there at the next start."));
	StyleButton(*ConfirmButton, true, Text.bCanConfirm);
	StyleButton(*Buttons[int32(EButton::Cancel)], false, true);
}

UWidget* UStageFixtureImportDialog::GetInitialFocus() const
{
	const int32 Index = StageFixtureLibraryView::DescribeReview(Review).bCanConfirm ? int32(EButton::Confirm) : int32(EButton::Cancel);
	return Buttons.IsValidIndex(Index) ? Buttons[Index].Get() : nullptr;
}

FReply UStageFixtureImportDialog::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		Cancel();
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::Enter)
	{
		Confirm();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UStageFixtureImportDialog::HandleButton(int32 Index)
{
	if (Index == int32(EButton::Confirm))
	{
		Confirm();
	}
	else
	{
		Cancel();
	}
}

void UStageFixtureImportDialog::HandleKindChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (SelectionType == ESelectInfo::Direct || !KindCombo)
	{
		return;
	}
	const int32 Index = KindCombo->GetSelectedIndex();
	if (Index >= 0 && Index < StageFixtureImportDialog::KindCount())
	{
		const EStageFixtureKind Kind = EStageFixtureKind(Index);
		CorrectedKind = Kind == Review.InferredKind ? TOptional<EStageFixtureKind>() : TOptional<EStageFixtureKind>(Kind);
	}
}

void UStageFixtureImportDialog::Confirm()
{
	AModularPlayerController* Controller = GetStageController();
	if (bClosed || !Controller || !StageFixtureLibraryView::DescribeReview(Review).bCanConfirm)
	{
		return;
	}
	const FStageFixtureImportOutcome Outcome = Controller->RequestConfirmFixtureImport(Review.ReviewId, CorrectedKind.IsSet(), CorrectedKind.Get(Review.InferredKind));
	if (Outcome.IsSuccess() || Outcome.Result == EStageFixtureImportResult::Unknown)
	{
		bClosed = true;
		OnClosed.ExecuteIfBound();
		return;
	}
	StatusText->SetText(Outcome.Message);
}

void UStageFixtureImportDialog::Cancel()
{
	if (bClosed)
	{
		return;
	}
	bClosed = true;
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestCancelFixtureImport(Review.ReviewId);
	}
	OnClosed.ExecuteIfBound();
}

#undef LOCTEXT_NAMESPACE
