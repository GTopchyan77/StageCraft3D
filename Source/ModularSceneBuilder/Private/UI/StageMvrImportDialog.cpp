// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageMvrImportDialog.h"

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
#include "Misc/Paths.h"
#include "Mvr/StageMvrImportSubsystem.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMvrImportDialog)

#define LOCTEXT_NAMESPACE "StageMvrImportDialog"

namespace StageMvrImportDialog
{
	constexpr float CardWidth = 560.f;
	constexpr float TypeListHeight = 200.f;
	constexpr float NotesHeight = 140.f;
	constexpr float SectionGap = 10.f;
	/** Notes shown in the dialog; the rest are counted (all of them are in the log). */
	constexpr int32 MaxNotesShown = 30;
}

void UStageMvrImportDialog::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageMvrImportDialog::NativeDestruct()
{
	for (UStageChoiceButton* Button : Buttons)
	{
		if (Button)
		{
			Button->OnPicked.Unbind();
		}
	}
	OnClosed.Unbind();
	Super::NativeDestruct();
}

const UStageCraftUITheme& UStageMvrImportDialog::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageMvrImportDialog::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStageMvrImportDialog::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color, bool bWrap)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	Block->SetAutoWrapText(bWrap);
	return Block;
}

UStageChoiceButton* UStageMvrImportDialog::MakeButton(const FText& Label, const FText& Tip, EButton Index)
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

void UStageMvrImportDialog::StyleButton(UStageChoiceButton& Button, bool bPrimary, bool bEnabled) const
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

void UStageMvrImportDialog::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("DialogSize"));
	Size->SetWidthOverride(StageMvrImportDialog::CardWidth);
	WidgetTree->RootWidget = Size;

	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("DialogCard"));
	Card->SetBrush(MakeBox(Palette.PanelBackground, CornerRadius, Palette.AccentColor, 1.f));
	Card->SetPadding(FMargin(16.f, 12.f));
	Size->SetContent(Card);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DialogColumn"));
	Card->SetContent(Column);
	const auto Add = [Column](UWidget* Widget, float Top = 3.f) { Column->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, Top, 0.f, 0.f)); };
	const auto AddScrolled = [this, &Palette, &Add](UWidget* Content, float MaxHeight, const TCHAR* Name)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), Name);
		Box->SetMaxDesiredHeight(MaxHeight);
		UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
		Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
		Scroll->AddChild(Content);
		Box->SetContent(Scroll);
		Add(Box);
	};

	Add(MakeText(LOCTEXT("Heading", "IMPORT MVR SCENE"), 9, TEXT("Bold"), Palette.AccentColor), 0.f);
	TitleText = MakeText(FText::GetEmpty(), 13, TEXT("Bold"), Palette.TextPrimary);
	TitleText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(TitleText, 6.f);
	SourceText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary, true);
	Add(SourceText);
	SummaryText = MakeText(FText::GetEmpty(), 9, TEXT("Regular"), Palette.TextPrimary, true);
	Add(SummaryText, 6.f);

	Add(MakeText(LOCTEXT("TypesLabel", "FIXTURE TYPES (GDTF)"), 7, TEXT("Bold"), Palette.TextSecondary), StageMvrImportDialog::SectionGap);
	TypeList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("TypeList"));
	AddScrolled(TypeList, StageMvrImportDialog::TypeListHeight, TEXT("TypeBox"));

	Add(MakeText(LOCTEXT("NotesLabel", "NOTES"), 7, TEXT("Bold"), Palette.TextSecondary), StageMvrImportDialog::SectionGap);
	NotesText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.WarningColor, true);
	AddScrolled(NotesText, StageMvrImportDialog::NotesHeight, TEXT("NotesBox"));
	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.WarningColor, true);
	Add(StatusText, 6.f);

	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Footer"));
	Footer->AddChildToHorizontalBox(WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()))->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Buttons.SetNum(int32(EButton::Count));
	Buttons[int32(EButton::Cancel)] = MakeButton(LOCTEXT("Cancel", "CANCEL"), LOCTEXT("CancelTip", "Close without changing anything (Esc)."), EButton::Cancel);
	Buttons[int32(EButton::Confirm)] = MakeButton(LOCTEXT("Import", "IMPORT"),
		LOCTEXT("ImportTip", "Add the file's fixture types to the Library and place its fixtures and trusses on the stage (Enter). One undo step takes it all back; the Library keeps the types."),
		EButton::Confirm);
	Footer->AddChildToHorizontalBox(Buttons[int32(EButton::Cancel)]);
	if (UHorizontalBoxSlot* ConfirmSlot = Footer->AddChildToHorizontalBox(Buttons[int32(EButton::Confirm)]))
	{
		ConfirmSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}
	Add(Footer, 12.f);
}

void UStageMvrImportDialog::ShowState(const UStageMvrImportSubsystem& Importer)
{
	const FStageMvrImportReview* Review = Importer.GetReview();
	if (!Review || !TitleText)
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	const bool bNewReview = Review->ReviewId != ReviewId;
	ReviewId = Review->ReviewId;
	bCanImport = Review->CanImport();
	bPlacing = Importer.GetState() == EStageMvrImportState::Placing;
	if (bNewReview)
	{
		bClosed = false;
		StatusText->SetText(FText::GetEmpty());
	}

	const int32 NoteCount = Review->Warnings.Num() + Review->Errors.Num();
	if (bNewReview || NoteCount != ShownNotes)
	{
		ShownNotes = NoteCount;
		TitleText->SetText(FText::FromString(FPaths::GetCleanFilename(Review->SourceFile)));
		TitleText->SetToolTipText(FText::FromString(Review->SourceFile));
		SourceText->SetText(FText::FromString(Review->Description));

		TArray<FText> Parts;
		Parts.Add(FText::Format(LOCTEXT("FixturesCount", "{0} {0}|plural(one=fixture,other=fixtures)"), FText::AsNumber(Review->Fixtures)));
		Parts.Add(FText::Format(LOCTEXT("TrussCount", "{0} {0}|plural(one=truss,other=trusses)"), FText::AsNumber(Review->Trusses)));
		Parts.Add(FText::Format(LOCTEXT("LayerCount", "{0} {0}|plural(one=layer,other=layers) (a fixture group each)"), FText::AsNumber(Review->Layers)));
		if (Review->FocusPoints > 0)
		{
			Parts.Add(FText::Format(LOCTEXT("FocusCount", "{0} focus {0}|plural(one=point,other=points) (fixtures aim at them)"), FText::AsNumber(Review->FocusPoints)));
		}
		FText Summary = FText::Join(FText::FromString(TEXT(" · ")), Parts);
		if (Review->GenericFixtures > 0)
		{
			Summary = FText::Format(LOCTEXT("GenericLine", "{0}\n{1} {1}|plural(one=fixture is,other=fixtures are) placed as a generic fixture: no usable GDTF type or mode."),
				Summary, FText::AsNumber(Review->GenericFixtures));
		}
		if (Review->OtherObjects > 0)
		{
			Summary = FText::Format(LOCTEXT("OtherLine", "{0}\n{1} other {1}|plural(one=object,other=objects) (set pieces, supports, screens) {1}|plural(one=is,other=are) not placed in this version."),
				Summary, FText::AsNumber(Review->OtherObjects));
		}
		SummaryText->SetText(Summary);

		TypeList->ClearChildren();
		for (const FStageMvrTypeLine& Line : Review->Types)
		{
			const FText State = !Line.bUsable ? LOCTEXT("TypeUnusable", "cannot be used")
				: (Line.bInLibrary ? LOCTEXT("TypeKnown", "in the Library") : LOCTEXT("TypeNew", "new: added to the Library"));
			FText Detail = FText::Format(LOCTEXT("TypeDetail", "{0} {0}|plural(one=fixture,other=fixtures) · {1} · {2}"), FText::AsNumber(Line.Fixtures), State, FText::FromString(Line.File));
			if (!Line.Problem.IsEmpty())
			{
				Detail = FText::Format(LOCTEXT("TypeProblem", "{0}\n{1}"), Detail, FText::FromString(Line.Problem));
			}
			UVerticalBox* Entry = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
			Entry->AddChildToVerticalBox(MakeText(FText::FromString(Line.Label), 9, TEXT("Bold"), Line.bUsable ? Palette.TextPrimary : Palette.TextSecondary));
			Entry->AddChildToVerticalBox(MakeText(Detail, 8, TEXT("Regular"), Line.bUsable && Line.Problem.IsEmpty() ? Palette.TextSecondary : Palette.WarningColor, true));
			TypeList->AddChildToVerticalBox(Entry)->SetPadding(FMargin(0.f, 3.f, 0.f, 3.f));
		}
		if (Review->Types.IsEmpty())
		{
			TypeList->AddChildToVerticalBox(MakeText(LOCTEXT("NoTypes", "The file names no GDTF fixture types."), 8, TEXT("Italic"), Palette.TextSecondary));
		}

		TArray<FString> Notes;
		for (const FString& Error : Review->Errors)
		{
			Notes.Add(FText::Format(LOCTEXT("ErrorLine", "Error: {0}"), FText::FromString(Error)).ToString());
		}
		for (int32 Index = 0; Index < Review->Warnings.Num() && Notes.Num() < StageMvrImportDialog::MaxNotesShown; ++Index)
		{
			Notes.Add(FText::Format(LOCTEXT("WarningLine", "Note: {0}"), FText::FromString(Review->Warnings[Index])).ToString());
		}
		if (Review->Warnings.Num() + Review->Errors.Num() > Notes.Num())
		{
			Notes.Add(FText::Format(LOCTEXT("MoreNotes", "... and {0} more notes in the log."), FText::AsNumber(Review->Warnings.Num() + Review->Errors.Num() - Notes.Num())).ToString());
		}
		NotesText->SetText(Notes.IsEmpty() ? LOCTEXT("NoNotes", "Nothing to report.") : FText::FromString(FString::Join(Notes, TEXT("\n"))));
		NotesText->SetColorAndOpacity(FSlateColor(Notes.IsEmpty() ? Palette.TextSecondary : Palette.WarningColor));
	}

	if (bPlacing)
	{
		int32 Done = 0;
		int32 Total = 0;
		Importer.GetProgress(Done, Total);
		StatusText->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		StatusText->SetText(Total > 0
			? FText::Format(LOCTEXT("Placing", "Placing {0} of {1}..."), FText::AsNumber(Done), FText::AsNumber(Total))
			: LOCTEXT("Preparing", "Adding fixture types and loading items..."));
	}
	StyleButton(*Buttons[int32(EButton::Confirm)], true, bCanImport && !bPlacing && !bClosed);
	StyleButton(*Buttons[int32(EButton::Cancel)], false, !bPlacing);
}

UWidget* UStageMvrImportDialog::GetInitialFocus() const
{
	const int32 Index = bCanImport ? int32(EButton::Confirm) : int32(EButton::Cancel);
	return Buttons.IsValidIndex(Index) ? Buttons[Index].Get() : nullptr;
}

FReply UStageMvrImportDialog::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
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

void UStageMvrImportDialog::HandleButton(int32 Index)
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

void UStageMvrImportDialog::Confirm()
{
	AModularPlayerController* Controller = GetStageController();
	if (bClosed || bPlacing || !bCanImport || !Controller)
	{
		return;
	}
	const FStageEconomyResultInfo Started = Controller->RequestConfirmMvrImport(ReviewId);
	if (!Started.IsSuccess())
	{
		StatusText->SetColorAndOpacity(FSlateColor(GetTheme().WarningColor));
		StatusText->SetText(Started.Message);
	}
}

void UStageMvrImportDialog::Cancel()
{
	if (bClosed || bPlacing)
	{
		return;
	}
	bClosed = true;
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestCancelMvrImport(ReviewId);
	}
	OnClosed.ExecuteIfBound();
}

#undef LOCTEXT_NAMESPACE
