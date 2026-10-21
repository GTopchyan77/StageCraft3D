// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageCuePanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCuePanel)

#define LOCTEXT_NAMESPACE "StageCuePanel"

namespace StageCuePanel
{
	constexpr float ActiveMarkerWidth = 14.f;
	constexpr float NumberWidth = 64.f;
	constexpr float FadeWidth = 58.f;
	constexpr float FixturesWidth = 44.f;
	constexpr float RowButtonWidth = 58.f;
	constexpr float DefaultFadeTime = 2.f;

	// Transport and source buttons report these choice indices.
	constexpr int32 ChoiceGo = 0;
	constexpr int32 ChoiceBack = 1;
	constexpr int32 ChoiceInternal = 0;
	constexpr int32 ChoiceExternal = 1;

	FText FormatCueNumber(float CueNumber)
	{
		// "1", "2.5", "3.125": as many decimals as the number has, like a console's cue list.
		FNumberFormattingOptions Options;
		Options.SetUseGrouping(false).SetMinimumFractionalDigits(0).SetMaximumFractionalDigits(3);
		return FText::AsNumber(CueNumber, &Options);
	}
}

// --- Row fields ---

void UStageCueLabelField::Configure(float InCueNumber)
{
	CueNumber = InCueNumber;
	OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleTextCommitted);
}

void UStageCueLabelField::HandleTextCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	// Escape reverts the text (SetRevertTextOnEscape) and must not write.
	if (CommitMethod != ETextCommit::OnCleared)
	{
		OnLabelCommitted.ExecuteIfBound(CueNumber, InText);
	}
}

void UStageCueFadeField::Configure(float InCueNumber)
{
	CueNumber = InCueNumber;
	OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleValueCommitted);
	OnEndSliderMovement.AddUniqueDynamic(this, &ThisClass::HandleEndSliderMovement);
}

void UStageCueFadeField::HandleValueCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	OnFadeCommitted.ExecuteIfBound(CueNumber, InValue);
}

void UStageCueFadeField::HandleEndSliderMovement(float InValue)
{
	OnFadeCommitted.ExecuteIfBound(CueNumber, InValue);
}

// --- UStageCuePanel ---

const UStageCraftUITheme& UStageCuePanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageCuePanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStageCuePanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UWidget* UStageCuePanel::MakeCell(UWidget& Content, float Width)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Width);
	Box->SetContent(&Content);
	return Box;
}

FButtonStyle UStageCuePanel::MakeButtonStyle(bool bHighlighted) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const FLinearColor Fill = bHighlighted ? WithAlpha(Palette.AccentColor, 0.35f) : FLinearColor::Transparent;
	FButtonStyle Style = MakeFlatButton(Fill, bHighlighted ? WithAlpha(Palette.AccentColor, 0.5f) : Palette.RowHover, Palette.Divider);
	Style.SetNormal(MakeBox(Fill, CornerRadius, Palette.Divider, 1.f));
	return Style;
}

void UStageCuePanel::SetSegmentActive(UStageChoiceButton& Button, bool bActive)
{
	// The chosen option of a segmented choice is lit, like the viewport toolbar's segments; both stay clickable.
	Button.SetStyle(MakeButtonStyle(bActive));
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetChildAt(0)))
	{
		Label->SetColorAndOpacity(FSlateColor(bActive ? GetTheme().TextPrimary : GetTheme().TextSecondary));
	}
}

UStageChoiceButton* UStageCuePanel::MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bPrimary)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetStyle(MakeButtonStyle(bPrimary));
	Button->SetToolTipText(ToolTip);
	Button->SetChoiceIndex(ChoiceIndex);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), bPrimary ? Palette.TextPrimary : Palette.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 4.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return Button;
}

USpinBox* UStageCuePanel::MakeNumberField(float Value, float Min, float Max, float Delta, int32 Digits, const FText& ToolTip)
{
	const UStageCraftUITheme& Palette = GetTheme();
	USpinBox* Field = WidgetTree->ConstructWidget<USpinBox>(USpinBox::StaticClass());
	Field->SetWidgetStyle(StageCraftWidgetStyle::MakeNumericField(Palette));
	Field->SetFont(StageCraftWidgetStyle::MakeFont(8));
	Field->SetForegroundColor(Palette.TextPrimary);
	Field->SetMinDesiredWidth(0.f);
	Field->SetMinFractionalDigits(0);
	Field->SetMaxFractionalDigits(Digits);
	Field->SetDelta(Delta);
	Field->SetMinValue(Min);
	Field->SetMaxValue(Max);
	Field->SetMinSliderValue(Min);
	Field->SetMaxSliderValue(Max);
	Field->SetValue(Value);
	Field->SetToolTipText(ToolTip);
	return Field;
}

void UStageCuePanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageCuePanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildTransport());
	Column->AddChildToVerticalBox(BuildRecordRow());
	Column->AddChildToVerticalBox(MakeColumnTitles());

	EmptyText = MakeText(LOCTEXT("Empty", "No cues yet. Set up a look with the inspector or the faders, then STORE it as cue 1."), 8, TEXT("Italic"), Palette.TextSecondary);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* ListScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("ListScroll"));
	ListScroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	ListScroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ListSlot = Column->AddChildToVerticalBox(ListScroll))
	{
		ListSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	ListBody = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ListBody"));
	ListScroll->AddChild(ListBody);
}

UWidget* UStageCuePanel::BuildTransport()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageCuePanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(Line);

	const auto Add = [Line](UWidget* Widget, float LeftGap, bool bFill)
	{
		if (UHorizontalBoxSlot* WidgetSlot = Line->AddChildToHorizontalBox(Widget))
		{
			WidgetSlot->SetVerticalAlignment(VAlign_Center);
			WidgetSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			if (bFill)
			{
				WidgetSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};

	UTextBlock* Title = MakeText(LOCTEXT("Title", "CUES"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "The show's cue list. Each cue records every fixture's look and crossfades to it on GO."));
	Add(Title, 0.f, false);

	GoButton = MakeButton(LOCTEXT("Go", "GO"), LOCTEXT("GoTip", "Play the next cue."), ChoiceGo, true);
	GoButton->OnPicked.BindUObject(this, &ThisClass::HandleTransportPicked);
	Add(GoButton, 12.f, false);
	BackButton = MakeButton(LOCTEXT("Back", "BACK"), LOCTEXT("BackTip", "Play the previous cue with its own fade time."), ChoiceBack, false);
	BackButton->OnPicked.BindUObject(this, &ThisClass::HandleTransportPicked);
	Add(BackButton, 4.f, false);

	ActiveCueText = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.AccentColor);
	ActiveCueText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(ActiveCueText, 10.f, true);

	SubtitleText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	Add(SubtitleText, 10.f, false);

	InternalButton = MakeButton(LOCTEXT("Internal", "INTERNAL"), LOCTEXT("InternalTip", "The cue list drives the fixtures (StageCraft is the console)."), ChoiceInternal, false);
	InternalButton->OnPicked.BindUObject(this, &ThisClass::HandleSourcePicked);
	Add(InternalButton, 10.f, false);
	ExternalButton = MakeButton(LOCTEXT("External", "EXTERNAL DMX"), LOCTEXT("ExternalTip", "Incoming DMX drives the fixtures (StageCraft visualises an external console). Cues cannot play; STORE records the incoming look."), ChoiceExternal, false);
	ExternalButton->OnPicked.BindUObject(this, &ThisClass::HandleSourcePicked);
	Add(ExternalButton, 2.f, false);
	return Header;
}

UWidget* UStageCuePanel::BuildRecordRow()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageCuePanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("RecordRow"));
	Row->SetBrush(MakeBox(Palette.PanelBackground));
	Row->SetPadding(FMargin(10.f, 5.f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Line);

	const auto Add = [Line](UWidget* Widget, float LeftGap, bool bFill)
	{
		if (UHorizontalBoxSlot* WidgetSlot = Line->AddChildToHorizontalBox(Widget))
		{
			WidgetSlot->SetVerticalAlignment(VAlign_Center);
			WidgetSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			if (bFill)
			{
				WidgetSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};
	const auto Sized = [this](UWidget& Content, float Width)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->SetHeightOverride(FieldHeight);
		Box->SetContent(&Content);
		return Box;
	};

	Add(MakeText(LOCTEXT("RecordCue", "CUE"), 7, TEXT("Bold"), Palette.TextSecondary), 0.f, false);
	RecordNumberField = MakeNumberField(1.f, StageCueRules::MinCueNumber, StageCueRules::MaxCueNumber, 1.f, 3,
		LOCTEXT("RecordNumberTip", "Number of the cue to record. Decimals insert a cue between two others (2.5)."));
	RecordNumberField->OnValueChanged.AddUniqueDynamic(this, &ThisClass::HandleRecordNumberChanged);
	Add(Sized(*RecordNumberField, NumberWidth), 6.f, false);

	RecordLabelField = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("RecordLabel"));
	FEditableTextBoxStyle TextStyle = MakeInputField(Palette);
	TextStyle.SetPadding(FMargin(FieldTextInset, 2.f));
	RecordLabelField->SetWidgetStyle(TextStyle);
	RecordLabelField->SetHintText(LOCTEXT("RecordLabelHint", "Label (optional)"));
	RecordLabelField->SetRevertTextOnEscape(true);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetHeightOverride(FieldHeight);
	LabelBox->SetContent(RecordLabelField);
	Add(LabelBox, 6.f, true);

	Add(MakeText(LOCTEXT("RecordFade", "FADE"), 7, TEXT("Bold"), Palette.TextSecondary), 8.f, false);
	RecordFadeField = MakeNumberField(DefaultFadeTime, 0.f, StageCueRules::MaxFadeTime, 0.1f, 2,
		LOCTEXT("RecordFadeTip", "Crossfade time in seconds when the cue plays."));
	Add(Sized(*RecordFadeField, FadeWidth), 6.f, false);

	StoreButton = MakeButton(LOCTEXT("Store", "STORE"), LOCTEXT("StoreTip", "Record every fixture's current look as this cue."), 0, true);
	StoreButton->OnPicked.BindUObject(this, &ThisClass::HandleStorePicked);
	StoreButtonText = Cast<UTextBlock>(StoreButton->GetChildAt(0));
	Add(StoreButton, 8.f, false);
	return Row;
}

UWidget* UStageCuePanel::MakeColumnTitles()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageCuePanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ColumnTitles"));
	Row->SetBrush(MakeBox(Palette.PanelHeader));
	Row->SetPadding(FMargin(10.f, 3.f));
	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Cells);

	const auto AddTitle = [this, Cells, &Palette](const FText& Title, float Width, float LeftGap)
	{
		UTextBlock* Text = MakeText(Title, 6, TEXT("Bold"), Palette.TextSecondary);
		UHorizontalBoxSlot* TitleSlot = Width > 0.f ? Cells->AddChildToHorizontalBox(MakeCell(*Text, Width)) : Cells->AddChildToHorizontalBox(Text);
		if (TitleSlot)
		{
			TitleSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			TitleSlot->SetVerticalAlignment(VAlign_Center);
			if (Width <= 0.f)
			{
				TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};
	AddTitle(FText::GetEmpty(), ActiveMarkerWidth, 0.f);
	AddTitle(LOCTEXT("NumberColumn", "CUE"), NumberWidth, 0.f);
	AddTitle(LOCTEXT("LabelColumn", "LABEL"), 0.f, 6.f);
	AddTitle(LOCTEXT("FadeColumn", "FADE (s)"), FadeWidth, 6.f);
	AddTitle(LOCTEXT("FixturesColumn", "FIXT."), FixturesWidth, 6.f);
	AddTitle(FText::GetEmpty(), RowButtonWidth * 2.f + 4.f, 6.f);
	return Row;
}

UWidget* UStageCuePanel::MakeCueRow(const FShowCue& Cue, bool bActive, int32 RowIndex)
{
	using namespace StageCraftWidgetStyle;
	using namespace StageCuePanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Row->SetBrush(MakeBox(bActive ? WithAlpha(Palette.AccentColor, 0.18f) : (RowIndex % 2 == 0 ? FLinearColor::Transparent : Palette.RowBackground)));
	Row->SetPadding(FMargin(10.f, 2.f));
	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Cells);

	const auto AddCell = [Cells](UWidget* Widget, float LeftGap, bool bFill)
	{
		if (UHorizontalBoxSlot* CellSlot = Cells->AddChildToHorizontalBox(Widget))
		{
			CellSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			CellSlot->SetVerticalAlignment(VAlign_Center);
			if (bFill)
			{
				CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};
	const auto Sized = [this](UWidget& Content, float Width)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		if (Width > 0.f)
		{
			Box->SetWidthOverride(Width);
		}
		Box->SetHeightOverride(FieldHeight);
		Box->SetContent(&Content);
		return Box;
	};

	UTextBlock* Marker = MakeText(bActive ? LOCTEXT("ActiveMarker", ">") : FText::GetEmpty(), 9, TEXT("Bold"), Palette.AccentColor);
	AddCell(MakeCell(*Marker, ActiveMarkerWidth), 0.f, false);

	UTextBlock* Number = MakeText(FormatCueNumber(Cue.Number), 9, TEXT("Bold"), bActive ? Palette.AccentColor : Palette.TextPrimary);
	AddCell(MakeCell(*Number, NumberWidth), 0.f, false);

	UStageCueLabelField* Label = WidgetTree->ConstructWidget<UStageCueLabelField>(UStageCueLabelField::StaticClass());
	FEditableTextBoxStyle TextStyle = MakeInputField(Palette);
	TextStyle.SetPadding(FMargin(FieldTextInset, 2.f));
	Label->SetWidgetStyle(TextStyle);
	Label->SetText(Cue.Label);
	Label->SetHintText(LOCTEXT("LabelHint", "No label"));
	Label->SetRevertTextOnEscape(true);
	Label->SetClearKeyboardFocusOnCommit(true);
	Label->Configure(Cue.Number);
	Label->OnLabelCommitted.BindUObject(this, &ThisClass::HandleLabelCommitted);
	AddCell(Sized(*Label, 0.f), 6.f, true);

	UStageCueFadeField* Fade = WidgetTree->ConstructWidget<UStageCueFadeField>(UStageCueFadeField::StaticClass());
	Fade->SetWidgetStyle(MakeNumericField(Palette));
	Fade->SetFont(MakeFont(8));
	Fade->SetForegroundColor(Palette.TextPrimary);
	Fade->SetMinDesiredWidth(0.f);
	Fade->SetMinFractionalDigits(0);
	Fade->SetMaxFractionalDigits(2);
	Fade->SetDelta(0.1f);
	Fade->SetMinValue(0.f);
	Fade->SetMaxValue(StageCueRules::MaxFadeTime);
	Fade->SetMinSliderValue(0.f);
	Fade->SetMaxSliderValue(30.f);
	Fade->SetValue(Cue.FadeTime);
	Fade->SetToolTipText(LOCTEXT("FadeTip", "Crossfade time in seconds."));
	Fade->Configure(Cue.Number);
	Fade->OnFadeCommitted.BindUObject(this, &ThisClass::HandleFadeCommitted);
	AddCell(Sized(*Fade, FadeWidth), 6.f, false);

	UTextBlock* Fixtures = MakeText(FText::AsNumber(Cue.Fixtures.Num()), 8, TEXT("Regular"), Palette.TextSecondary);
	Fixtures->SetJustification(ETextJustify::Right);
	Fixtures->SetToolTipText(LOCTEXT("FixturesTip", "Fixtures recorded in this cue."));
	AddCell(MakeCell(*Fixtures, FixturesWidth), 6.f, false);

	UStageChoiceButton* GoTo = MakeButton(LOCTEXT("GoTo", "GO TO"), LOCTEXT("GoToTip", "Crossfade to this cue."), RowIndex, false);
	GoTo->OnPicked.BindUObject(this, &ThisClass::HandleGoToPicked);
	AddCell(MakeCell(*GoTo, RowButtonWidth), 6.f, false);

	UStageChoiceButton* Delete = MakeButton(LOCTEXT("Delete", "DELETE"), LOCTEXT("DeleteTip", "Delete this cue (Ctrl+Z brings it back)."), RowIndex, false);
	Delete->OnPicked.BindUObject(this, &ThisClass::HandleDeletePicked);
	AddCell(MakeCell(*Delete, RowButtonWidth), 4.f, false);
	return Row;
}

void UStageCuePanel::NativeConstruct()
{
	Super::NativeConstruct();

	UShowControlSubsystem* Subsystem = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	ShowControl = Subsystem;
	if (Subsystem)
	{
		Subsystem->OnCueListChanged.AddUniqueDynamic(this, &ThisClass::HandleCueListChanged);
		Subsystem->OnCueStarted.AddUniqueDynamic(this, &ThisClass::HandleCueEvent);
		Subsystem->OnCueFinished.AddUniqueDynamic(this, &ThisClass::HandleCueEvent);
		Subsystem->OnControlSourceChanged.AddUniqueDynamic(this, &ThisClass::HandleControlSourceChanged);
	}
	if (RecordNumberField)
	{
		RecordNumberField->SetValue(GetSuggestedCueNumber());
	}
	Rebuild();
}

void UStageCuePanel::NativeDestruct()
{
	if (UShowControlSubsystem* Subsystem = ShowControl.Get())
	{
		Subsystem->OnCueListChanged.RemoveDynamic(this, &ThisClass::HandleCueListChanged);
		Subsystem->OnCueStarted.RemoveDynamic(this, &ThisClass::HandleCueEvent);
		Subsystem->OnCueFinished.RemoveDynamic(this, &ThisClass::HandleCueEvent);
		Subsystem->OnControlSourceChanged.RemoveDynamic(this, &ThisClass::HandleControlSourceChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	ShowControl.Reset();
	Super::NativeDestruct();
}

void UStageCuePanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	// Next tick: never inside the callback that caused the change (a row's own commit, a cue starting), and a burst is one rebuild.
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStageCuePanel::Rebuild()
{
	using namespace StageCuePanel;
	RebuildTimer.Invalidate();
	if (!ListBody)
	{
		return;
	}
	ListBody->ClearChildren();
	RowCueNumbers.Reset();

	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const TArray<FShowCue> NoCues;
	const TArray<FShowCue>& Cues = Subsystem ? Subsystem->GetCues() : NoCues;
	const float Active = Subsystem ? Subsystem->GetActiveCueNumber() : -1.f;
	for (const FShowCue& Cue : Cues)
	{
		const bool bActive = Active >= 0.f && FMath::IsNearlyEqual(Cue.Number, Active, StageCueRules::NumberResolution * 0.5f);
		ListBody->AddChildToVerticalBox(MakeCueRow(Cue, bActive, RowCueNumbers.Num()));
		RowCueNumbers.Add(Cue.Number);
	}

	const UStageCraftUITheme& Palette = GetTheme();
	const bool bExternal = Subsystem && Subsystem->GetControlSource() == EStageControlSource::External;
	EmptyText->SetVisibility(Cues.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

	const FShowCue* ActiveCue = Subsystem && Active >= 0.f ? Subsystem->FindCue(Active) : nullptr;
	if (bExternal)
	{
		ActiveCueText->SetText(LOCTEXT("ExternalActive", "External DMX is driving the fixtures"));
		ActiveCueText->SetColorAndOpacity(FSlateColor(Palette.WarningColor));
	}
	else
	{
		ActiveCueText->SetText(ActiveCue
			? FText::Format(LOCTEXT("ActiveCue", "Cue {0}{1}{2}"), FormatCueNumber(ActiveCue->Number),
				ActiveCue->Label.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("ActiveLabel", "  {0}"), ActiveCue->Label),
				Subsystem->IsFading() ? LOCTEXT("Fading", "  (fading)") : FText::GetEmpty())
			: LOCTEXT("NoActiveCue", "No cue playing"));
		ActiveCueText->SetColorAndOpacity(FSlateColor(Palette.AccentColor));
	}
	SubtitleText->SetText(FText::Format(LOCTEXT("Subtitle", "{0} {0}|plural(one=cue,other=cues)"), Cues.Num()));

	GoButton->SetIsEnabled(!bExternal && !Cues.IsEmpty());
	BackButton->SetIsEnabled(!bExternal && ActiveCue != nullptr);
	SetSegmentActive(*InternalButton, !bExternal);
	SetSegmentActive(*ExternalButton, bExternal);
	RefreshStoreButton();
}

float UStageCuePanel::GetSuggestedCueNumber() const
{
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	if (!Subsystem || Subsystem->GetCues().IsEmpty())
	{
		return 1.f;
	}
	return FMath::Min(FMath::FloorToFloat(Subsystem->GetCues().Last().Number) + 1.f, FMath::FloorToFloat(StageCueRules::MaxCueNumber));
}

void UStageCuePanel::RefreshStoreButton()
{
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const bool bExists = Subsystem && RecordNumberField && Subsystem->FindCue(RecordNumberField->GetValue()) != nullptr;
	if (StoreButtonText)
	{
		StoreButtonText->SetText(bExists ? LOCTEXT("ReRecord", "RE-RECORD") : LOCTEXT("Store", "STORE"));
	}
	if (StoreButton)
	{
		StoreButton->SetToolTipText(bExists
			? LOCTEXT("ReRecordTip", "Replace this cue's recorded look with every fixture's current look (Ctrl+Z restores it).")
			: LOCTEXT("StoreTip", "Record every fixture's current look as this cue."));
	}
}

void UStageCuePanel::HandleCueListChanged()
{
	RequestRebuild();
}

void UStageCuePanel::HandleCueEvent(float CueNumber)
{
	RequestRebuild();
}

void UStageCuePanel::HandleControlSourceChanged(EStageControlSource NewSource)
{
	RequestRebuild();
}

void UStageCuePanel::HandleRecordNumberChanged(float InValue)
{
	RefreshStoreButton();
}

void UStageCuePanel::HandleTransportPicked(int32 ChoiceIndex)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		if (ChoiceIndex == StageCuePanel::ChoiceGo)
		{
			Controller->RequestCueGo();
		}
		else
		{
			Controller->RequestCueBack();
		}
	}
}

void UStageCuePanel::HandleSourcePicked(int32 ChoiceIndex)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestSetControlSource(ChoiceIndex == StageCuePanel::ChoiceExternal ? EStageControlSource::External : EStageControlSource::Internal);
	}
}

void UStageCuePanel::HandleStorePicked(int32 ChoiceIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller || !RecordNumberField || !RecordFadeField || !RecordLabelField)
	{
		return;
	}
	if (Controller->RequestStoreCue(RecordNumberField->GetValue(), RecordLabelField->GetText(), RecordFadeField->GetValue()))
	{
		// Ready for the next cue, like a console after Record.
		RecordLabelField->SetText(FText::GetEmpty());
		RecordNumberField->SetValue(GetSuggestedCueNumber());
	}
	RefreshStoreButton();
}

void UStageCuePanel::HandleGoToPicked(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && RowCueNumbers.IsValidIndex(RowIndex))
	{
		Controller->RequestGoToCue(RowCueNumbers[RowIndex]);
	}
}

void UStageCuePanel::HandleDeletePicked(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && RowCueNumbers.IsValidIndex(RowIndex))
	{
		Controller->RequestDeleteCue(RowCueNumbers[RowIndex]);
	}
}

void UStageCuePanel::HandleLabelCommitted(float CueNumber, const FText& Label)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const FShowCue* Cue = Subsystem ? Subsystem->FindCue(CueNumber) : nullptr;
	if (Controller && Cue && !Cue->Label.EqualTo(Label))
	{
		Controller->RequestUpdateCue(CueNumber, Label, Cue->FadeTime);
	}
	// Read back on the next tick: a refused or trimmed label shows what the cue really holds.
	RequestRebuild();
}

void UStageCuePanel::HandleFadeCommitted(float CueNumber, float FadeTime)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const FShowCue* Cue = Subsystem ? Subsystem->FindCue(CueNumber) : nullptr;
	if (Controller && Cue && !FMath::IsNearlyEqual(Cue->FadeTime, FadeTime))
	{
		Controller->RequestUpdateCue(CueNumber, Cue->Label, FadeTime);
	}
	RequestRebuild();
}

#undef LOCTEXT_NAMESPACE
