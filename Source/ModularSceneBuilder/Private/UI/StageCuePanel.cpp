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
	constexpr float EffectsWidth = 36.f;
	constexpr float RowButtonWidth = 58.f;
	constexpr float DefaultFadeTime = 2.f;
	constexpr float TimeWidth = 44.f;
	constexpr float TriggerWidth = 62.f;
	constexpr float ToggleWidth = 22.f;
	constexpr float FeatureLabelWidth = 34.f;

	// Transport and source buttons report these choice indices.
	constexpr int32 ChoiceGo = 0;
	constexpr int32 ChoiceBack = 1;
	constexpr int32 ChoiceInternal = 0;
	constexpr int32 ChoiceExternal = 1;

	// Time fields report these ids; a feature time is FieldFeatureBase + its EStagePaletteKind.
	constexpr int32 FieldIn = 0;
	constexpr int32 FieldOut = 1;
	constexpr int32 FieldDelay = 2;
	constexpr int32 FieldWait = 3;
	constexpr int32 FieldFeatureBase = 10;

	FText TriggerLabel(const FStageCueTiming& Timing)
	{
		switch (Timing.Trigger)
		{
		case EStageCueTrigger::Follow:
			return LOCTEXT("TriggerFollow", "FOLLOW");
		case EStageCueTrigger::Wait:
			return FText::Format(LOCTEXT("TriggerWait", "WAIT {0}"), FText::AsNumber(Timing.WaitTime));
		default:
			return LOCTEXT("TriggerGo", "GO");
		}
	}

	FText StoreModeLabel(EStageCueStoreMode Mode)
	{
		switch (Mode)
		{
		case EStageCueStoreMode::Track:
			return LOCTEXT("ModeTrack", "TRACK");
		case EStageCueStoreMode::CueOnly:
			return LOCTEXT("ModeCueOnly", "CUE ONLY");
		default:
			return LOCTEXT("ModeLook", "LOOK");
		}
	}

	FText StoreModeTip(EStageCueStoreMode Mode)
	{
		switch (Mode)
		{
		case EStageCueStoreMode::Track:
			return LOCTEXT("ModeTrackTip", "Store what the programmer holds. It tracks: later cues keep it until they change it.");
		case EStageCueStoreMode::CueOnly:
			return LOCTEXT("ModeCueOnlyTip", "Store what the programmer holds in this cue only: the next cue keeps its old values.");
		default:
			return LOCTEXT("ModeLookTip", "Store every fixture's whole look as a blocked cue that inherits nothing.");
		}
	}

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

void UStageCueFadeField::Configure(float InCueNumber, int32 InFieldId)
{
	CueNumber = InCueNumber;
	FieldId = InFieldId;
	OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleValueCommitted);
	OnEndSliderMovement.AddUniqueDynamic(this, &ThisClass::HandleEndSliderMovement);
}

void UStageCueFadeField::HandleValueCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	OnTimeCommitted.ExecuteIfBound(CueNumber, FieldId, InValue);
}

void UStageCueFadeField::HandleEndSliderMovement(float InValue)
{
	OnTimeCommitted.ExecuteIfBound(CueNumber, FieldId, InValue);
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

	EmptyText = MakeText(LOCTEXT("Empty", "No cues yet. Set up a look with the inspector or the faders (it goes into the programmer), then STORE it as cue 1."), 8, TEXT("Italic"), Palette.TextSecondary);
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
	Title->SetToolTipText(LOCTEXT("TitleTip", "The show's cue list. Cues track: each holds what it changes, earlier cues give the rest. GO plays the next cue with its timing."));
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

	Add(MakeText(LOCTEXT("RecordFade", "IN"), 7, TEXT("Bold"), Palette.TextSecondary), 8.f, false);
	RecordFadeField = MakeNumberField(DefaultFadeTime, 0.f, StageCueRules::MaxFadeTime, 0.1f, 2,
		LOCTEXT("RecordFadeTip", "The cue's In time: seconds its values take to arrive when it plays (a new cue only)."));
	Add(Sized(*RecordFadeField, FadeWidth), 6.f, false);

	StoreModeButtons.Reset();
	for (const EStageCueStoreMode Mode : { EStageCueStoreMode::Look, EStageCueStoreMode::Track, EStageCueStoreMode::CueOnly })
	{
		UStageChoiceButton* ModeButton = MakeButton(StoreModeLabel(Mode), StoreModeTip(Mode), static_cast<int32>(Mode), Mode == StoreMode);
		ModeButton->OnPicked.BindUObject(this, &ThisClass::HandleStoreModePicked);
		Add(ModeButton, StoreModeButtons.IsEmpty() ? 8.f : 2.f, false);
		StoreModeButtons.Add(ModeButton);
	}

	StoreButton = MakeButton(LOCTEXT("Store", "STORE"), LOCTEXT("StoreTip", "Record the cue as the store mode says."), 0, true);
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
	AddTitle(LOCTEXT("InColumn", "IN (s)"), TimeWidth, 6.f);
	AddTitle(LOCTEXT("OutColumn", "OUT"), TimeWidth, 4.f);
	AddTitle(LOCTEXT("DelayColumn", "DELAY"), TimeWidth, 4.f);
	AddTitle(LOCTEXT("TriggerColumn", "TRIGGER"), TriggerWidth, 4.f);
	AddTitle(LOCTEXT("BlockColumn", "BLK"), ToggleWidth, 4.f);
	AddTitle(LOCTEXT("FixturesColumn", "FIXT."), FixturesWidth, 6.f);
	AddTitle(LOCTEXT("EffectsColumn", "FX"), EffectsWidth, 4.f);
	AddTitle(FText::GetEmpty(), ToggleWidth + RowButtonWidth * 2.f + 8.f, 6.f);
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

	const FStageCueTiming& Timing = Cue.Timing;
	AddCell(Sized(*MakeTimeField(Cue.Number, FieldIn, Cue.FadeTime,
		LOCTEXT("InTip", "In time: seconds the cue's values take to arrive (unless a feature time says otherwise).")), TimeWidth), 6.f, false);
	AddCell(Sized(*MakeTimeField(Cue.Number, FieldOut, Timing.bHasOutTime ? Timing.OutTime : Cue.FadeTime,
		LOCTEXT("OutTip", "Out time: seconds for intensity going down. The same as In unless you change it.")), TimeWidth), 4.f, false);
	AddCell(Sized(*MakeTimeField(Cue.Number, FieldDelay, Timing.Delay,
		LOCTEXT("DelayTip", "Delay: seconds after the cue starts before anything in it moves.")), TimeWidth), 4.f, false);

	UStageChoiceButton* Trigger = MakeButton(TriggerLabel(Timing),
		LOCTEXT("TriggerTip", "What starts this cue: GO (your Go), FOLLOW (when the cue before it finishes) or WAIT (seconds after the cue before it starts; set the seconds in the detail line). Click to change."),
		RowIndex, Timing.Trigger != EStageCueTrigger::Go);
	Trigger->OnPicked.BindUObject(this, &ThisClass::HandleTriggerPicked);
	AddCell(MakeCell(*Trigger, TriggerWidth), 4.f, false);

	UStageChoiceButton* Block = MakeButton(LOCTEXT("BlockMark", "B"),
		Cue.bBlock ? LOCTEXT("BlockedTip", "Blocked: this cue inherits nothing from the cues before it. Click to let it track again.")
			: LOCTEXT("BlockTip", "Block: this cue keeps its look but stops inheriting from the cues before it. Click to block."),
		RowIndex, Cue.bBlock);
	Block->OnPicked.BindUObject(this, &ThisClass::HandleBlockPicked);
	AddCell(MakeCell(*Block, ToggleWidth), 4.f, false);

	UTextBlock* Fixtures = MakeText(FText::AsNumber(Cue.Fixtures.Num()), 8, TEXT("Regular"), Palette.TextSecondary);
	Fixtures->SetJustification(ETextJustify::Right);
	Fixtures->SetToolTipText(LOCTEXT("FixturesTip", "Fixtures this cue changes (a tracking cue holds only its changes; earlier cues give the rest)."));
	AddCell(MakeCell(*Fixtures, FixturesWidth), 6.f, false);

	// Effects the cue starts or changes, and stops (Docs/ADR/0027 §5): "2", "-1", "2 -1"; empty when it touches none.
	TArray<FString> EffectNames;
	for (const FStageEffectEntry& Entry : Cue.Effects)
	{
		EffectNames.Add(Entry.Effect.Name.ToString());
	}
	FString EffectsLabel = Cue.Effects.IsEmpty() ? FString() : FString::FromInt(Cue.Effects.Num());
	// ♪: one of them follows the music (Docs/ADR/0029 §4).
	if (Cue.Effects.ContainsByPredicate([](const FStageEffectEntry& Entry) { return Entry.Effect.UsesAudio(); }))
	{
		EffectsLabel += TEXT(" ♪");
	}
	if (!Cue.EffectStops.IsEmpty())
	{
		EffectsLabel += FString::Printf(TEXT("%s-%d"), EffectsLabel.IsEmpty() ? TEXT("") : TEXT(" "), Cue.EffectStops.Num());
	}
	UTextBlock* Effects = MakeText(FText::FromString(EffectsLabel), 8, TEXT("Regular"), Palette.TextSecondary);
	Effects->SetJustification(ETextJustify::Right);
	Effects->SetToolTipText(Cue.Effects.IsEmpty() && Cue.EffectStops.IsEmpty()
		? LOCTEXT("NoEffectsTip", "Effects: this cue starts and stops none (effects from earlier cues carry on).")
		: FText::Format(LOCTEXT("EffectsTip", "Effects this cue starts or changes: {0}. Effects it stops: {1}. They track into the cues that follow."),
			EffectNames.IsEmpty() ? LOCTEXT("EffectsNone", "none") : FText::FromString(FString::Join(EffectNames, TEXT(", "))),
			FText::AsNumber(Cue.EffectStops.Num())));
	AddCell(MakeCell(*Effects, EffectsWidth), 4.f, false);

	const bool bExpanded = ExpandedCues.Contains(Cue.Number);
	UStageChoiceButton* Detail = MakeButton(bExpanded ? LOCTEXT("Collapse", "-") : LOCTEXT("Expand", "+"),
		LOCTEXT("DetailTip", "Show or hide this cue's feature times, wait time and curve."), RowIndex, bExpanded);
	Detail->OnPicked.BindUObject(this, &ThisClass::HandleDetailPicked);
	AddCell(MakeCell(*Detail, ToggleWidth), 6.f, false);

	UStageChoiceButton* GoTo = MakeButton(LOCTEXT("GoTo", "GO TO"), LOCTEXT("GoToTip", "Crossfade to this cue."), RowIndex, false);
	GoTo->OnPicked.BindUObject(this, &ThisClass::HandleGoToPicked);
	AddCell(MakeCell(*GoTo, RowButtonWidth), 6.f, false);

	UStageChoiceButton* Delete = MakeButton(LOCTEXT("Delete", "DELETE"), LOCTEXT("DeleteTip", "Delete this cue (Ctrl+Z brings it back)."), RowIndex, false);
	Delete->OnPicked.BindUObject(this, &ThisClass::HandleDeletePicked);
	AddCell(MakeCell(*Delete, RowButtonWidth), 4.f, false);
	return Row;
}

UStageCueFadeField* UStageCuePanel::MakeTimeField(float CueNumber, int32 FieldId, float Seconds, const FText& ToolTip)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UStageCueFadeField* Field = WidgetTree->ConstructWidget<UStageCueFadeField>(UStageCueFadeField::StaticClass());
	Field->SetWidgetStyle(MakeNumericField(Palette));
	Field->SetFont(MakeFont(8));
	Field->SetForegroundColor(Palette.TextPrimary);
	Field->SetMinDesiredWidth(0.f);
	Field->SetMinFractionalDigits(0);
	Field->SetMaxFractionalDigits(2);
	Field->SetDelta(0.1f);
	Field->SetMinValue(0.f);
	Field->SetMaxValue(StageCueRules::MaxFadeTime);
	Field->SetMinSliderValue(0.f);
	Field->SetMaxSliderValue(30.f);
	Field->SetValue(Seconds);
	Field->SetToolTipText(ToolTip);
	Field->Configure(CueNumber, FieldId);
	Field->OnTimeCommitted.BindUObject(this, &ThisClass::HandleTimeCommitted);
	return Field;
}

UWidget* UStageCuePanel::MakeCueDetailRow(const FShowCue& Cue, int32 RowIndex)
{
	using namespace StageCraftWidgetStyle;
	using namespace StageCuePanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Row->SetBrush(MakeBox(Palette.RowBackground));
	Row->SetPadding(FMargin(10.f + ActiveMarkerWidth + NumberWidth, 2.f, 10.f, 4.f));
	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Cells);
	const auto AddCell = [Cells](UWidget* Widget, float LeftGap)
	{
		if (UHorizontalBoxSlot* CellSlot = Cells->AddChildToHorizontalBox(Widget))
		{
			CellSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			CellSlot->SetVerticalAlignment(VAlign_Center);
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

	// A feature without its own time shows the In time; changing it gives the feature its own, setting it back to In removes it.
	const TPair<EStagePaletteKind, FText> Features[] = {
		{ EStagePaletteKind::Intensity, LOCTEXT("FeatureIntensity", "INT") },
		{ EStagePaletteKind::Position, LOCTEXT("FeaturePosition", "POS") },
		{ EStagePaletteKind::Color, LOCTEXT("FeatureColor", "COL") },
		{ EStagePaletteKind::Beam, LOCTEXT("FeatureBeam", "BEAM") },
		{ EStagePaletteKind::Focus, LOCTEXT("FeatureFocus", "FOC") },
	};
	for (const TPair<EStagePaletteKind, FText>& Feature : Features)
	{
		const float* Own = Cue.Timing.FeatureTimes.Find(Feature.Key);
		AddCell(MakeCell(*MakeText(Feature.Value, 6, TEXT("Bold"), Own ? Palette.AccentColor : Palette.TextSecondary), FeatureLabelWidth), 6.f);
		AddCell(Sized(*MakeTimeField(Cue.Number, FieldFeatureBase + static_cast<int32>(Feature.Key), Own ? *Own : Cue.FadeTime,
			LOCTEXT("FeatureTimeTip", "This feature's own fade time. Set it back to the In time to follow In again.")), TimeWidth), 0.f);
	}
	AddCell(MakeCell(*MakeText(LOCTEXT("WaitLabel", "WAIT"), 6, TEXT("Bold"), Palette.TextSecondary), FeatureLabelWidth), 12.f);
	AddCell(Sized(*MakeTimeField(Cue.Number, FieldWait, Cue.Timing.WaitTime,
		LOCTEXT("WaitTip", "Trigger WAIT: seconds after the cue before this one starts.")), TimeWidth), 0.f);
	UStageChoiceButton* Curve = MakeButton(Cue.Timing.Curve == EStageCueCurve::SCurve ? LOCTEXT("CurveS", "S-CURVE") : LOCTEXT("CurveLinear", "LINEAR"),
		LOCTEXT("CurveTip", "How the cue's values move: evenly (LINEAR) or easing in and out (S-CURVE). Click to change."), RowIndex,
		Cue.Timing.Curve == EStageCueCurve::SCurve);
	Curve->OnPicked.BindUObject(this, &ThisClass::HandleCurvePicked);
	AddCell(MakeCell(*Curve, TriggerWidth), 12.f);
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
		const int32 RowIndex = RowCueNumbers.Num();
		ListBody->AddChildToVerticalBox(MakeCueRow(Cue, bActive, RowIndex));
		if (ExpandedCues.Contains(Cue.Number))
		{
			ListBody->AddChildToVerticalBox(MakeCueDetailRow(Cue, RowIndex));
		}
		RowCueNumbers.Add(Cue.Number);
	}
	// A deleted cue's detail line does not come back if a new cue later takes its number.
	for (auto It = ExpandedCues.CreateIterator(); It; ++It)
	{
		if (!RowCueNumbers.Contains(*It))
		{
			It.RemoveCurrent();
		}
	}
	for (int32 Mode = 0; Mode < StoreModeButtons.Num(); ++Mode)
	{
		SetSegmentActive(*StoreModeButtons[Mode], static_cast<EStageCueStoreMode>(Mode) == StoreMode);
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
	const bool bLook = StoreMode == EStageCueStoreMode::Look;
	if (StoreButtonText)
	{
		StoreButtonText->SetText(!bExists ? LOCTEXT("Store", "STORE") : (bLook ? LOCTEXT("ReRecord", "RE-RECORD") : LOCTEXT("Update", "UPDATE")));
	}
	if (StoreButton)
	{
		StoreButton->SetToolTipText(bLook
			? (bExists ? LOCTEXT("ReRecordTip", "Replace this cue with every fixture's current look, as a blocked cue (Ctrl+Z restores it).")
				: LOCTEXT("StoreLookTip", "Record every fixture's current look as this cue, a blocked cue."))
			: (bExists ? LOCTEXT("UpdateTip", "Add what the programmer holds to this cue (Ctrl+Z restores it).")
				: LOCTEXT("StoreTrackTip", "Record what the programmer holds as this cue; earlier cues give the rest.")));
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
	if (Controller->RequestStoreCue(RecordNumberField->GetValue(), RecordLabelField->GetText(), RecordFadeField->GetValue(), StoreMode))
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

void UStageCuePanel::HandleTimeCommitted(float CueNumber, int32 FieldId, float Seconds)
{
	using namespace StageCuePanel;
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const FShowCue* Cue = Subsystem ? Subsystem->FindCue(CueNumber) : nullptr;
	if (!Controller || !Cue)
	{
		RequestRebuild();
		return;
	}
	if (FieldId == FieldIn)
	{
		if (!FMath::IsNearlyEqual(Cue->FadeTime, Seconds))
		{
			Controller->RequestUpdateCue(CueNumber, Cue->Label, Seconds);
		}
		RequestRebuild();
		return;
	}

	// Out and feature times equal to In mean "follow In": stored as no time of their own, so a later In change carries them along.
	FStageCueTiming Timing = Cue->Timing;
	const bool bSameAsIn = FMath::IsNearlyEqual(Seconds, Cue->FadeTime, 1e-3f);
	if (FieldId == FieldOut)
	{
		Timing.bHasOutTime = !bSameAsIn;
		Timing.OutTime = bSameAsIn ? 0.f : Seconds;
	}
	else if (FieldId == FieldDelay)
	{
		Timing.Delay = Seconds;
	}
	else if (FieldId == FieldWait)
	{
		Timing.WaitTime = Seconds;
	}
	else if (FieldId > FieldFeatureBase && FieldId <= FieldFeatureBase + static_cast<int32>(EStagePaletteKind::Focus))
	{
		const EStagePaletteKind Kind = static_cast<EStagePaletteKind>(FieldId - FieldFeatureBase);
		if (bSameAsIn)
		{
			Timing.FeatureTimes.Remove(Kind);
		}
		else
		{
			Timing.FeatureTimes.Add(Kind, Seconds);
		}
	}
	Controller->RequestUpdateCueTiming(CueNumber, Timing);
	// Read back on the next tick: a refused or clamped time shows what the cue really holds.
	RequestRebuild();
}

void UStageCuePanel::HandleStoreModePicked(int32 ChoiceIndex)
{
	StoreMode = static_cast<EStageCueStoreMode>(FMath::Clamp(ChoiceIndex, 0, static_cast<int32>(EStageCueStoreMode::CueOnly)));
	RequestRebuild();
}

void UStageCuePanel::HandleTriggerPicked(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const FShowCue* Cue = Subsystem && RowCueNumbers.IsValidIndex(RowIndex) ? Subsystem->FindCue(RowCueNumbers[RowIndex]) : nullptr;
	if (Controller && Cue)
	{
		FStageCueTiming Timing = Cue->Timing;
		Timing.Trigger = Timing.Trigger == EStageCueTrigger::Go ? EStageCueTrigger::Follow
			: (Timing.Trigger == EStageCueTrigger::Follow ? EStageCueTrigger::Wait : EStageCueTrigger::Go);
		if (Timing.Trigger == EStageCueTrigger::Wait)
		{
			ExpandedCues.Add(Cue->Number); // The wait time is set in the detail line: show it.
		}
		Controller->RequestUpdateCueTiming(Cue->Number, Timing);
	}
	RequestRebuild();
}

void UStageCuePanel::HandleBlockPicked(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const FShowCue* Cue = Subsystem && RowCueNumbers.IsValidIndex(RowIndex) ? Subsystem->FindCue(RowCueNumbers[RowIndex]) : nullptr;
	if (Controller && Cue)
	{
		Controller->RequestSetCueBlocked(Cue->Number, !Cue->bBlock);
	}
	RequestRebuild();
}

void UStageCuePanel::HandleCurvePicked(int32 RowIndex)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	const FShowCue* Cue = Subsystem && RowCueNumbers.IsValidIndex(RowIndex) ? Subsystem->FindCue(RowCueNumbers[RowIndex]) : nullptr;
	if (Controller && Cue)
	{
		FStageCueTiming Timing = Cue->Timing;
		Timing.Curve = Timing.Curve == EStageCueCurve::Linear ? EStageCueCurve::SCurve : EStageCueCurve::Linear;
		Controller->RequestUpdateCueTiming(Cue->Number, Timing);
	}
	RequestRebuild();
}

void UStageCuePanel::HandleDetailPicked(int32 RowIndex)
{
	if (RowCueNumbers.IsValidIndex(RowIndex))
	{
		const float Number = RowCueNumbers[RowIndex];
		if (ExpandedCues.Remove(Number) == 0)
		{
			ExpandedCues.Add(Number);
		}
	}
	RequestRebuild();
}

#undef LOCTEXT_NAMESPACE
