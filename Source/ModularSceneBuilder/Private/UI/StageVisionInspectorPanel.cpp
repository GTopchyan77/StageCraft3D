// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageVisionInspectorPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Import/StageFileDialog.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "Sketch/StageSketchImageImport.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "Vision/StageVisionBomSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageVisionInspectorPanel)

#define LOCTEXT_NAMESPACE "StageVisionInspectorPanel"

namespace StageVisionInspectorPanel
{
	void SetLabel(UStageChoiceButton* Button, const FText& Label)
	{
		if (UTextBlock* Text = Button ? Cast<UTextBlock>(Button->GetContent()) : nullptr)
		{
			Text->SetText(Label);
		}
	}
}

const UStageCraftUITheme& UStageVisionInspectorPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

UStageVisionBomSubsystem* UStageVisionInspectorPanel::GetVision() const
{
	return UWorld::GetSubsystem<UStageVisionBomSubsystem>(GetWorld());
}

UTextBlock* UStageVisionInspectorPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageVisionInspectorPanel::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageVisionInspectorPanel::*Handler)(int32), bool bAccent)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	FButtonStyle Style = bAccent ? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bAccent)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button->SetStyle(Style);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), bAccent ? Palette.WindowBackground : Palette.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 3.f));
	}
	return Button;
}

void UStageVisionInspectorPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageVisionInspectorPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	WidgetTree->RootWidget = Root;
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->SetContent(Column);

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(Line);
	Column->AddChildToVerticalBox(Header);
	Line->AddChildToHorizontalBox(MakeText(LOCTEXT("Title", "VISION INSPECTOR"), 9, TEXT("Bold"), Palette.TextPrimary))->SetVerticalAlignment(VAlign_Center);
	USizeBox* Spacer = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Line->AddChildToHorizontalBox(Spacer)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	ActionButtons.SetNum(int32(EAction::Count));
	ActionButtons[int32(EAction::OpenPhoto)] = MakeButton(LOCTEXT("OpenPhoto", "OPEN PHOTO..."),
		LOCTEXT("OpenPhotoTip", "Pick a PNG or JPEG photo of a real stage. It is resized and sent to the online model (uses your API key)."), int32(EAction::OpenPhoto), &ThisClass::HandleAction, true);
	ActionButtons[int32(EAction::Cancel)] = MakeButton(LOCTEXT("Cancel", "CANCEL"), LOCTEXT("CancelTip", "Stop reading the photo."), int32(EAction::Cancel), &ThisClass::HandleAction);
	ActionButtons[int32(EAction::Clear)] = MakeButton(LOCTEXT("Clear", "CLEAR"), LOCTEXT("ClearTip", "Empty the bill of materials."), int32(EAction::Clear), &ThisClass::HandleAction);
	for (const EAction Action : { EAction::OpenPhoto, EAction::Cancel, EAction::Clear })
	{
		Line->AddChildToHorizontalBox(ActionButtons[int32(Action)])->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	}

	StatusText = MakeText(LOCTEXT("Intro", "Open a photo of a real stage to list the equipment needed to build it."), 8, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	Column->AddChildToVerticalBox(StatusText)->SetPadding(FMargin(10.f, 6.f, 10.f, 2.f));
	UTextBlock* Honest = MakeText(LOCTEXT("Estimate", "Counts read from one photo are an estimate. Check every line, especially those marked CHECK, before ordering or rigging anything."),
		7, TEXT("Italic"), Palette.WarningColor);
	Honest->SetAutoWrapText(true);
	Column->AddChildToVerticalBox(Honest)->SetPadding(FMargin(10.f, 0.f, 10.f, 4.f));

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(6.f, 2.f));
	}
	List = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(List);

	TotalsText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	TotalsText->SetAutoWrapText(true);
	Column->AddChildToVerticalBox(TotalsText)->SetPadding(FMargin(10.f, 4.f));

	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Column->AddChildToVerticalBox(Footer)->SetPadding(FMargin(10.f, 4.f, 10.f, 8.f));
	ActionButtons[int32(EAction::CopyCsv)] = MakeButton(LOCTEXT("CopyCsv", "COPY CSV"), LOCTEXT("CopyCsvTip", "Copy the bill as CSV, to paste into a spreadsheet or an e-mail."),
		int32(EAction::CopyCsv), &ThisClass::HandleAction);
	ActionButtons[int32(EAction::SaveCsv)] = MakeButton(LOCTEXT("SaveCsv", "SAVE CSV..."), LOCTEXT("SaveCsvTip", "Save the bill as a CSV file."), int32(EAction::SaveCsv), &ThisClass::HandleAction);
	ActionButtons[int32(EAction::Build)] = MakeButton(LOCTEXT("Build", "BUILD IN SCENE"),
		LOCTEXT("BuildTip", "Place the deck, truss runs and every matched item on a stage of the estimated size, as one step that Undo removes. Items not in the catalog are listed only."),
		int32(EAction::Build), &ThisClass::HandleAction, true);
	Footer->AddChildToHorizontalBox(ActionButtons[int32(EAction::CopyCsv)]);
	Footer->AddChildToHorizontalBox(ActionButtons[int32(EAction::SaveCsv)])->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	USizeBox* FooterSpacer = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Footer->AddChildToHorizontalBox(FooterSpacer)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	Footer->AddChildToHorizontalBox(ActionButtons[int32(EAction::Build)]);
}

void UStageVisionInspectorPanel::NativeConstruct()
{
	Super::NativeConstruct();
	if (UStageVisionBomSubsystem* Vision = GetVision())
	{
		ChangedHandle = Vision->OnChanged.AddUObject(this, &ThisClass::HandleChanged);
		BoundVision = Vision;
	}
	Rebuild();
}

void UStageVisionInspectorPanel::NativeDestruct()
{
	if (UStageVisionBomSubsystem* Vision = BoundVision.Get())
	{
		Vision->OnChanged.Remove(ChangedHandle);
	}
	ChangedHandle.Reset();
	BoundVision.Reset();
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	Super::NativeDestruct();
}

void UStageVisionInspectorPanel::HandleChanged()
{
	RequestRebuild();
}

void UStageVisionInspectorPanel::RequestRebuild()
{
	// Structural changes rebuild on the next tick, never inside the callback that caused them (a row button's own click).
	if (const UWorld* World = GetWorld())
	{
		if (!RebuildTimer.IsValid() || !World->GetTimerManager().IsTimerActive(RebuildTimer))
		{
			RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
		}
	}
}

void UStageVisionInspectorPanel::Rebuild()
{
	RebuildTimer.Invalidate();
	const UStageVisionBomSubsystem* Vision = GetVision();
	const UStageCraftUITheme& Palette = GetTheme();
	List->ClearChildren();
	if (!Vision)
	{
		StatusText->SetText(LOCTEXT("Unavailable", "The Vision Inspector is not available in this level."));
		return;
	}
	const bool bBusy = Vision->IsBusy();
	const TArray<FStageVisionBomLine>& Lines = Vision->GetLines();
	FText Reason;
	if (!Vision->GetMessage().IsEmpty())
	{
		StatusText->SetText(Vision->GetModelNotes().IsEmpty() ? Vision->GetMessage()
			: FText::Format(LOCTEXT("WithNotes", "{0}\nThe model noted: {1}"), Vision->GetMessage(), FText::FromString(Vision->GetModelNotes())));
	}
	else if (!Vision->CanInspect(Reason))
	{
		StatusText->SetText(Reason);
	}
	StatusText->SetColorAndOpacity(FSlateColor(Vision->GetState() == EStageVisionBomState::Failed ? Palette.WarningColor : Palette.TextSecondary));

	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const FStageVisionBomLine& Line = Lines[Index];
		UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Row->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.RowBackground, StageCraftWidgetStyle::CornerRadius, Palette.Divider, 1.f));
		Row->SetPadding(FMargin(8.f, 4.f));
		UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->SetContent(Cells);

		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Text->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("RowTitle", "{0} × {1}"), FText::AsNumber(Line.Quantity), StageVisionBom::GetKindDisplayName(Line.Kind)),
			8, TEXT("Bold"), Palette.TextPrimary));
		const FText Spec = StageVisionBom::DescribeSpec(Line);
		const UBaseItemData* Item = Line.Match.Get();
		Text->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("RowDetail", "{0}{1}  ·  {2}"), StageVisionBom::DescribePosition(Line),
			Spec.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("RowSpec", "  ·  {0}"), Spec),
			Item ? FText::Format(LOCTEXT("Matched", "catalog: {0}"), Item->DisplayName) : LOCTEXT("NotMatched", "not in the catalog")),
			7, TEXT("Regular"), Item ? Palette.TextSecondary : Palette.WarningColor));
		if (!Line.Notes.IsEmpty())
		{
			Text->AddChildToVerticalBox(MakeText(FText::FromString(Line.Notes), 7, TEXT("Italic"), Palette.TextSecondary));
		}
		Cells->AddChildToHorizontalBox(Text)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		Cells->AddChildToHorizontalBox(MakeText(Line.NeedsReview()
				? FText::Format(LOCTEXT("Check", "CHECK {0}%"), FText::AsNumber(FMath::RoundToInt(Line.Confidence * 100.f)))
				: FText::Format(LOCTEXT("Confidence", "{0}%"), FText::AsNumber(FMath::RoundToInt(Line.Confidence * 100.f))),
			7, TEXT("Bold"), Line.NeedsReview() ? Palette.WarningColor : Palette.TextSecondary))->SetVerticalAlignment(VAlign_Center);
		const FText RowTips[] = { LOCTEXT("LessTip", "One fewer."), LOCTEXT("MoreTip", "One more."), LOCTEXT("RemoveTip", "Remove this line.") };
		const TCHAR* RowLabels[] = { TEXT("-"), TEXT("+"), TEXT("X") };
		for (int32 Button = 0; Button < 3; ++Button)
		{
			UStageChoiceButton* RowButton = MakeButton(FText::FromString(RowLabels[Button]), RowTips[Button], Index * 3 + Button, &ThisClass::HandleRowAction);
			RowButton->SetIsEnabled(!bBusy);
			Cells->AddChildToHorizontalBox(RowButton)->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
		}
		List->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
	}

	const FStageVisionBomTotals Totals = Vision->GetTotals();
	TotalsText->SetText(Lines.IsEmpty() ? FText::GetEmpty()
		: FText::Format(LOCTEXT("Totals", "{0} lines: {1} in the catalog, {2} not, {3} to check. Matched items: about {4} kg and {5} kW (truss and decks are measured once built)."),
			FText::AsNumber(Lines.Num()), FText::AsNumber(Totals.MatchedLines), FText::AsNumber(Totals.UnmatchedLines), FText::AsNumber(Totals.LinesToReview),
			FText::AsNumber(FMath::RoundToInt(Totals.WeightKg)), FText::AsNumber(FMath::RoundToDouble(Totals.PowerW / 100.0) / 10.0)));

	FText Unavailable;
	ActionButtons[int32(EAction::OpenPhoto)]->SetIsEnabled(!bBusy && Vision->CanInspect(Unavailable));
	ActionButtons[int32(EAction::Cancel)]->SetVisibility(bBusy ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	ActionButtons[int32(EAction::Clear)]->SetIsEnabled(!bBusy && !Lines.IsEmpty());
	for (const EAction Action : { EAction::CopyCsv, EAction::SaveCsv, EAction::Build })
	{
		ActionButtons[int32(Action)]->SetIsEnabled(!bBusy && !Lines.IsEmpty());
	}
}

void UStageVisionInspectorPanel::HandleAction(int32 Index)
{
	UStageVisionBomSubsystem* Vision = GetVision();
	if (!Vision)
	{
		return;
	}
	FText Error;
	switch (static_cast<EAction>(Index))
	{
	case EAction::OpenPhoto:
	{
		TArray<FString> Files;
		if (!StageFileDialog::OpenFiles(LOCTEXT("OpenPhotoTitle", "Open a Photo of a Stage"), LastPhotoDirectory, StageSketchImageImport::GetFileDialogFilter(), Files)
			|| Files.IsEmpty())
		{
			return;
		}
		LastPhotoDirectory = FPaths::GetPath(Files[0]);
		if (!Vision->Inspect(Files[0], Error))
		{
			StatusText->SetText(Error);
			StatusText->SetColorAndOpacity(FSlateColor(GetTheme().WarningColor));
		}
		return;
	}
	case EAction::Cancel:
		Vision->Cancel();
		return;
	case EAction::Clear:
		Vision->Clear();
		return;
	case EAction::CopyCsv:
		FPlatformApplicationMisc::ClipboardCopy(*Vision->MakeCsv());
		StatusText->SetText(LOCTEXT("Copied", "The bill was copied as CSV."));
		return;
	case EAction::SaveCsv:
	{
		FString File;
		if (StageFileDialog::SaveFile(LOCTEXT("SaveCsvTitle", "Save the Bill of Materials"), FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Reports")),
			TEXT("Bill of Materials.csv"), TEXT("CSV file (*.csv)|*.csv"), File))
		{
			StatusText->SetText(Vision->SaveCsv(File, Error) ? FText::Format(LOCTEXT("Saved", "Saved {0}."), FText::FromString(File)) : Error);
		}
		return;
	}
	case EAction::Build:
		if (AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer()))
		{
			// Refusals are reported on the status bar (OnRequestRejected); the outcome arrives on OnChanged.
			Controller->RequestBuildFromVisionBom();
		}
		return;
	default:
		return;
	}
}

void UStageVisionInspectorPanel::HandleRowAction(int32 Index)
{
	UStageVisionBomSubsystem* Vision = GetVision();
	const int32 LineIndex = Index / 3;
	if (!Vision || !Vision->GetLines().IsValidIndex(LineIndex))
	{
		return;
	}
	switch (Index % 3)
	{
	case 0:
		Vision->SetLineQuantity(LineIndex, Vision->GetLines()[LineIndex].Quantity - 1);
		break;
	case 1:
		Vision->SetLineQuantity(LineIndex, Vision->GetLines()[LineIndex].Quantity + 1);
		break;
	default:
		Vision->RemoveLine(LineIndex);
		break;
	}
}

#undef LOCTEXT_NAMESPACE
