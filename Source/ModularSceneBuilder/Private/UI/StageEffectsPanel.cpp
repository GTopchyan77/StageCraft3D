// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageEffectsPanel.h"

#include "Algo/Compare.h"
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
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Data/StageParameterTypes.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageEffectPreview.h"
#include "UI/StageToolButton.h"
#include "UI/StageValueSlider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEffectsPanel)

#define LOCTEXT_NAMESPACE "StageEffectsPanel"

namespace StageEffectsPanel
{
	/** A choice button's index is Row * ChoiceStride + option. */
	constexpr int32 ChoiceStride = 100;
	constexpr float LabelWidth = 58.f;
	constexpr float ValueWidth = 104.f;
	constexpr float TapWidth = 40.f;
	constexpr float SideWidth = 168.f;
	/** The running list: wider, as each line has a source and EDIT and STOP buttons. */
	constexpr float RunningWidth = 220.f;
	/** The rate slider is quadratic up to this, for fine control of slow effects; faster rates come from TAP or the console. */
	constexpr float SliderMaxRateHz = 10.f;
	constexpr float MaxSliderSize = 180.f;
	constexpr float MaxSliderSpread = 720.f;
	constexpr int32 MaxSliderWings = 8;
	constexpr int32 MaxSliderBlocks = 16;
	/** Taps further apart than this start a new tempo. */
	constexpr double TapResetSeconds = 2.0;
	constexpr int32 MaxTaps = 5;

	const FLinearColor ColorChoices[] = { FLinearColor(1.f, 0.f, 0.f), FLinearColor(1.f, 0.5f, 0.f), FLinearColor(1.f, 1.f, 0.f), FLinearColor(0.f, 1.f, 0.f),
		FLinearColor(0.f, 1.f, 1.f), FLinearColor(0.f, 0.f, 1.f), FLinearColor(1.f, 0.f, 1.f), FLinearColor(1.f, 1.f, 1.f) };

	FGameplayTag BeamChoice(int32 Option)
	{
		switch (Option)
		{
		case 1:		return StageCraftTags::Attribute_Iris;
		case 2:		return StageCraftTags::Attribute_Frost;
		case 3:		return StageCraftTags::Attribute_Shutter;
		case 4:		return StageCraftTags::Attribute_GoboRotation;
		default:	return StageCraftTags::Attribute_Zoom;
		}
	}
	constexpr int32 BeamChoiceCount = 5;

	/** Rates with up to two decimals (at least MinDigits), no grouping. */
	const FNumberFormattingOptions& RateFormat(int32 MinDigits = 0)
	{
		static const FNumberFormattingOptions Loose = FNumberFormattingOptions().SetUseGrouping(false).SetMaximumFractionalDigits(2);
		static const FNumberFormattingOptions Fixed = FNumberFormattingOptions().SetUseGrouping(false).SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
		return MinDigits > 0 ? Fixed : Loose;
	}

	FText FormatPercent(float Fraction)
	{
		return FText::Format(LOCTEXT("Percent", "{0}%"), FText::AsNumber(FMath::RoundToInt(Fraction * 100.f)));
	}

	FText FormatDegrees(float Degrees)
	{
		return FText::Format(LOCTEXT("Degrees", "{0}°"), FText::AsNumber(FMath::RoundToInt(Degrees)));
	}
}

const UStageCraftUITheme& UStageEffectsPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageEffectsPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStageEffectsPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

FButtonStyle UStageEffectsPanel::MakeButtonStyle(bool bLit, const FLinearColor& LitColor) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	const FLinearColor Fill = bLit ? WithAlpha(LitColor, 0.45f) : FLinearColor::Transparent;
	FButtonStyle Style = MakeFlatButton(Fill, bLit ? WithAlpha(LitColor, 0.6f) : UITheme.RowHover, UITheme.Divider);
	Style.SetNormal(MakeBox(Fill, CornerRadius, UITheme.Divider, 1.f));
	return Style;
}

UStageChoiceButton* UStageEffectsPanel::MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetStyle(MakeButtonStyle(bLit, UITheme.AccentColor));
	Button->SetToolTipText(ToolTip);
	Button->SetChoiceIndex(ChoiceIndex);
	UTextBlock* Text = MakeText(Label, 7, TEXT("Bold"), bLit ? UITheme.TextPrimary : UITheme.TextSecondary);
	Text->SetJustification(ETextJustify::Center);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(5.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return Button;
}

void UStageEffectsPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	QuickEffects = StageEffectRules::MakeQuickEffects();
	AudioQuickStart = QuickEffects.Num();
	QuickEffects.Append(StageEffectRules::MakeAudioQuickEffects());
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageEffectsPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(UITheme.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(UITheme.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UHorizontalBox* HeaderLine = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(HeaderLine);
	HeaderLine->AddChildToHorizontalBox(MakeText(LOCTEXT("Title", "EFFECTS"), 9, TEXT("Bold"), UITheme.TextPrimary));
	StatusText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextSecondary);
	if (UHorizontalBoxSlot* StatusSlot = HeaderLine->AddChildToHorizontalBox(StatusText))
	{
		StatusSlot->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
		StatusSlot->SetVerticalAlignment(VAlign_Center);
	}
	Column->AddChildToVerticalBox(Header);

	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Body"));
	if (UVerticalBoxSlot* BodySlot = Column->AddChildToVerticalBox(Body))
	{
		BodySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		BodySlot->SetPadding(FMargin(8.f, 6.f));
	}
	if (UHorizontalBoxSlot* QuickSlot = Body->AddChildToHorizontalBox(BuildQuickColumn()))
	{
		QuickSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
	if (UHorizontalBoxSlot* EditorSlot = Body->AddChildToHorizontalBox(BuildEditor()))
	{
		EditorSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	if (UHorizontalBoxSlot* RunningSlot = Body->AddChildToHorizontalBox(BuildRunningColumn()))
	{
		RunningSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}
}

UWidget* UStageEffectsPanel::BuildQuickColumn()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Width->SetWidthOverride(StageEffectsPanel::SideWidth);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Width->SetContent(Column);
	Column->AddChildToVerticalBox(MakeText(LOCTEXT("QuickTitle", "QUICK EFFECTS"), 7, TEXT("Bold"), UITheme.TextSecondary));
	UTextBlock* Hint = MakeText(LOCTEXT("QuickHint", "Start on the selected fixtures, in selection order."), 7, TEXT("Regular"), UITheme.TextDisabled);
	Hint->SetAutoWrapText(true);
	Column->AddChildToVerticalBox(Hint);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(StageCraftWidgetStyle::MakeScrollBar(UITheme));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	UVerticalBox* Groups = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(Groups);
	Groups->AddChildToVerticalBox(BuildQuickGrid(0, AudioQuickStart));
	// The audio quick effects follow the music (Docs/ADR/0029 §5): they move only while the Audio panel has a source playing.
	UTextBlock* AudioTitle = MakeText(LOCTEXT("QuickAudioTitle", "AUDIO"), 7, TEXT("Bold"), UITheme.TextSecondary);
	AudioTitle->SetToolTipText(LOCTEXT("QuickAudioTip", "Effects that follow the music of the Audio panel's source: on the beat, or with a band's level. Without music they hold, or stay fully lit."));
	if (UVerticalBoxSlot* TitleSlot = Groups->AddChildToVerticalBox(AudioTitle))
	{
		TitleSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 3.f));
	}
	if (UVerticalBoxSlot* MasterSlot = Groups->AddChildToVerticalBox(BuildAudioMasterRow()))
	{
		MasterSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}
	Groups->AddChildToVerticalBox(BuildQuickGrid(AudioQuickStart, QuickEffects.Num() - AudioQuickStart));
	return Width;
}

UWidget* UStageEffectsPanel::BuildAudioMasterRow()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetToolTipText(LOCTEXT("AudioMasterTip", "AUDIO master: how much effects that follow the music follow it. Down, a pulse breathes less; at 0 they run as without music. A stage always opens at full."));
	UStageValueSlider* Slider = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Slider->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(UITheme));
	Slider->SetSliderHandleColor(FLinearColor::White);
	Slider->SetMinValue(0.f);
	Slider->SetMaxValue(1.f);
	Slider->SetStepSize(0.01f);
	Slider->OnMoved.BindUObject(this, &ThisClass::HandleAudioMasterMoved);
	AudioMasterSlider = Slider;
	if (UHorizontalBoxSlot* SliderSlot = Row->AddChildToHorizontalBox(Slider))
	{
		SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SliderSlot->SetVerticalAlignment(VAlign_Center);
	}
	USizeBox* ValueBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ValueBox->SetWidthOverride(32.f);
	UTextBlock* Value = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextPrimary);
	Value->SetJustification(ETextJustify::Right);
	ValueBox->SetContent(Value);
	AudioMasterText = Value;
	if (UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(ValueBox))
	{
		ValueSlot->SetVerticalAlignment(VAlign_Center);
		ValueSlot->SetPadding(FMargin(4.f, 0.f));
	}
	UStageChoiceButton* Kill = MakeButton(LOCTEXT("AudioKill", "KILL"), LOCTEXT("AudioKillTip", "Stop every effect from following the music at once: pulses stay open, chases hold, synced effects keep their last tempo. Click again to follow the music."), 0, false);
	Kill->OnPicked.BindUObject(this, &ThisClass::HandleAudioKillPicked);
	AudioKillButton = Kill;
	if (UHorizontalBoxSlot* KillSlot = Row->AddChildToHorizontalBox(Kill))
	{
		KillSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Row;
}

UWidget* UStageEffectsPanel::BuildQuickGrid(int32 First, int32 Count)
{
	UWrapBox* Grid = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	Grid->SetInnerSlotPadding(FVector2D(3.f, 3.f));
	for (int32 Index = First; Index < First + Count && Index < QuickEffects.Num(); ++Index)
	{
		UStageChoiceButton* Button = MakeButton(QuickEffects[Index].Name,
			FText::Format(LOCTEXT("QuickTip", "Start {0} on the selected fixtures."), QuickEffects[Index].Name), Index, false);
		Button->OnPicked.BindUObject(this, &ThisClass::HandleQuickPicked);
		USizeBox* ButtonWidth = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		ButtonWidth->SetWidthOverride((StageEffectsPanel::SideWidth - 10.f) * 0.5f);
		ButtonWidth->SetContent(Button);
		Grid->AddChildToWrapBox(ButtonWidth);
	}
	return Grid;
}

UWidget* UStageEffectsPanel::BuildSettingRow(ESetting Setting, const FText& Label, const FText& ToolTip)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetToolTipText(ToolTip);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(StageEffectsPanel::LabelWidth);
	LabelBox->SetContent(MakeText(Label, 7, TEXT("Bold"), UITheme.TextSecondary));
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}

	UStageValueSlider* Slider = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Slider->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(UITheme));
	Slider->SetSliderHandleColor(FLinearColor::White);
	Slider->SetMinValue(0.f);
	Slider->SetMaxValue(1.f);
	Slider->SetStepSize(0.001f);
	Slider->SetSliderIndex(static_cast<int32>(Setting));
	Slider->OnMoved.BindUObject(this, &ThisClass::HandleSliderMoved);
	Slider->OnMouseCaptureEnd.AddUniqueDynamic(this, &ThisClass::HandleSliderReleased);
	if (UHorizontalBoxSlot* SliderSlot = Row->AddChildToHorizontalBox(Slider))
	{
		SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SliderSlot->SetVerticalAlignment(VAlign_Center);
	}

	USizeBox* ValueBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ValueBox->SetWidthOverride(StageEffectsPanel::ValueWidth);
	UTextBlock* Value = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextPrimary);
	Value->SetJustification(ETextJustify::Right);
	ValueBox->SetContent(Value);
	if (UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(ValueBox))
	{
		ValueSlot->SetVerticalAlignment(VAlign_Center);
		ValueSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
	}
	// Every row ends in the same column, so the sliders line up; the rate row holds TAP there.
	USizeBox* TapBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	TapBox->SetWidthOverride(StageEffectsPanel::TapWidth);
	if (Setting == ESetting::Rate)
	{
		UStageChoiceButton* Tap = MakeButton(LOCTEXT("Tap", "TAP"), LOCTEXT("TapTip", "Tap in time: the rate follows your beat (one cycle per tap)."), 0, false);
		Tap->OnPicked.BindUObject(this, &ThisClass::HandleTapPicked);
		TapBox->SetContent(Tap);
	}
	if (UHorizontalBoxSlot* TapSlot = Row->AddChildToHorizontalBox(TapBox))
	{
		TapSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
		TapSlot->SetVerticalAlignment(VAlign_Center);
	}

	FSettingRow& Entry = SettingRows[static_cast<int32>(Setting)];
	Entry.Row = Row;
	Entry.Slider = Slider;
	Entry.Value = Value;
	return Row;
}

UWidget* UStageEffectsPanel::BuildChoiceRow(EChoiceRow RowKind, const FText& Label, TConstArrayView<FText> Options, const FText& ToolTip)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetToolTipText(ToolTip);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(StageEffectsPanel::LabelWidth);
	LabelBox->SetContent(MakeText(Label, 7, TEXT("Bold"), UITheme.TextSecondary));
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}
	UWrapBox* Buttons = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	Buttons->SetInnerSlotPadding(FVector2D(2.f, 2.f));
	if (UHorizontalBoxSlot* ButtonsSlot = Row->AddChildToHorizontalBox(Buttons))
	{
		ButtonsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	FChoiceRow& Entry = ChoiceRows[static_cast<int32>(RowKind)];
	Entry.Row = Row;
	for (int32 Option = 0; Option < Options.Num(); ++Option)
	{
		UStageChoiceButton* Button = MakeButton(Options[Option], ToolTip, static_cast<int32>(RowKind) * StageEffectsPanel::ChoiceStride + Option, false);
		Button->OnPicked.BindUObject(this, &ThisClass::HandleChoicePicked);
		Buttons->AddChildToWrapBox(Button);
		Entry.Buttons.Add(Button);
	}
	return Row;
}

UWidget* UStageEffectsPanel::BuildEditor()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	EditorTitle = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), UITheme.TextPrimary);
	Column->AddChildToVerticalBox(EditorTitle);
	EditorHint = MakeText(LOCTEXT("EditorHint", "Pick a quick effect to start one on the selected fixtures, or a running effect to change it."),
		7, TEXT("Regular"), UITheme.TextDisabled);
	EditorHint->SetAutoWrapText(true);
	Column->AddChildToVerticalBox(EditorHint);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(StageCraftWidgetStyle::MakeScrollBar(UITheme));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	EditorBody = Scroll;
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(Rows);
	const auto Add = [Rows](UWidget* Widget)
	{
		if (UVerticalBoxSlot* RowSlot = Rows->AddChildToVerticalBox(Widget))
		{
			RowSlot->SetPadding(FMargin(0.f, 2.f, 6.f, 2.f));
		}
	};

	Preview = WidgetTree->ConstructWidget<UStageEffectPreview>(UStageEffectPreview::StaticClass());
	Preview->SetPreviewColors(StageCraftWidgetStyle::WithAlpha(UITheme.WindowBackground, 1.f), StageCraftWidgetStyle::WithAlpha(UITheme.TextSecondary, 0.15f),
		UITheme.AccentColor, UITheme.TextPrimary);
	Preview->SetToolTipText(LOCTEXT("PreviewTip", "One cycle of the wave, and where each fixture is on it now (the first is brightest)."));
	TWeakObjectPtr<UStageEffectsPanel> WeakThis(this);
	Preview->SetStateSource([WeakThis](FStageEffect& OutEffect, int32& OutCount, double& OutCycles)
	{
		const UStageEffectsPanel* Panel = WeakThis.Get();
		return Panel && Panel->GetPreviewState(OutEffect, OutCount, OutCycles);
	});
	USizeBox* PreviewBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	PreviewBox->SetHeightOverride(104.f);
	PreviewBox->SetContent(Preview);
	Add(PreviewBox);

	SettingRows.SetNum(static_cast<int32>(ESetting::Count));
	ChoiceRows.SetNum(static_cast<int32>(EChoiceRow::Count));
	const FText Waveforms[] = { LOCTEXT("Sine", "SINE"), LOCTEXT("Cosine", "COS"), LOCTEXT("Square", "SQUARE"), LOCTEXT("RampUp", "RAMP UP"), LOCTEXT("RampDown", "RAMP DN"),
		LOCTEXT("Triangle", "TRI"), LOCTEXT("Random", "RANDOM"), LOCTEXT("Circle", "CIRCLE"), LOCTEXT("Figure8", "FIG 8") };
	Add(BuildChoiceRow(EChoiceRow::Waveform, LOCTEXT("Wave", "WAVE"), Waveforms, LOCTEXT("WaveTip", "The shape repeated every cycle. Circle and Figure 8 move pan and tilt together.")));
	const FText Directions[] = { LOCTEXT("Forward", "FWD"), LOCTEXT("Reverse", "REV"), LOCTEXT("Bounce", "BOUNCE") };
	Add(BuildChoiceRow(EChoiceRow::Direction, LOCTEXT("Direction", "DIR"), Directions, LOCTEXT("DirectionTip", "Which way the pattern runs along the fixtures: first to last, last to first, or out and back.")));
	const FText Fans[] = { LOCTEXT("FanOff", "OFF"), LOCTEXT("FanLinear", "LINEAR"), LOCTEXT("FanV", "V"), LOCTEXT("FanInvertedV", "INV V") };
	Add(BuildChoiceRow(EChoiceRow::Fan, LOCTEXT("Fan", "FAN"), Fans, LOCTEXT("FanTip", "Size across the fixtures: the same everywhere, rising from first to last, biggest at the ends (V) or in the middle.")));
	const FText Axes[] = { LOCTEXT("Pan", "PAN"), LOCTEXT("Tilt", "TILT"), LOCTEXT("Both", "BOTH") };
	Add(BuildChoiceRow(EChoiceRow::Axis, LOCTEXT("Axis", "AXIS"), Axes, LOCTEXT("AxisTip", "Which axes a one-axis wave moves.")));
	const FText Modes[] = { LOCTEXT("Crossfade", "A ↔ B"), LOCTEXT("Rainbow", "RAINBOW") };
	Add(BuildChoiceRow(EChoiceRow::ColorMode, LOCTEXT("Colour", "COLOUR"), Modes, LOCTEXT("ModeTip", "Fade between colours A and B, or run round the colour wheel.")));
	const FText Colors[] = { LOCTEXT("Red", "RED"), LOCTEXT("Amber", "AMB"), LOCTEXT("Yellow", "YEL"), LOCTEXT("Green", "GRN"), LOCTEXT("Cyan", "CYN"), LOCTEXT("Blue", "BLU"),
		LOCTEXT("Magenta", "MAG"), LOCTEXT("White", "WHT") };
	Add(BuildChoiceRow(EChoiceRow::ColorA, LOCTEXT("ColorA", "A"), Colors, LOCTEXT("ColorATip", "The colour at the bottom of the wave.")));
	Add(BuildChoiceRow(EChoiceRow::ColorB, LOCTEXT("ColorB", "B"), Colors, LOCTEXT("ColorBTip", "The colour at the top of the wave.")));
	const FText Beams[] = { LOCTEXT("Zoom", "ZOOM"), LOCTEXT("Iris", "IRIS"), LOCTEXT("Frost", "FROST"), LOCTEXT("Shutter", "SHUTTER"), LOCTEXT("GoboRotation", "GOBO ROT") };
	static_assert(UE_ARRAY_COUNT(Beams) == StageEffectsPanel::BeamChoiceCount, "One button per beam choice.");
	Add(BuildChoiceRow(EChoiceRow::BeamAttribute, LOCTEXT("Beam", "BEAM"), Beams, LOCTEXT("BeamTip", "The beam attribute the effect moves.")));

	// How the effect follows the music (Docs/ADR/0029 §4).
	const FText AudioModes[] = { LOCTEXT("AudioOff", "OFF"), LOCTEXT("AudioSync", "SYNC"), LOCTEXT("AudioModulate", "MODULATE"), LOCTEXT("AudioTrigger", "TRIGGER") };
	Add(BuildChoiceRow(EChoiceRow::AudioMode, LOCTEXT("Audio", "AUDIO"), AudioModes,
		LOCTEXT("AudioTip", "Follow the music of the Audio panel's source. SYNC runs the wave at the tempo, on the beat. MODULATE lets a band's level scale it. TRIGGER steps the pattern on each beat. OFF: its own rate.")));
	const FText Bands[] = { LOCTEXT("SubBass", "SUB"), LOCTEXT("Bass", "BASS"), LOCTEXT("Mid", "MID"), LOCTEXT("High", "HIGH"), LOCTEXT("Level", "LEVEL") };
	Add(BuildChoiceRow(EChoiceRow::AudioBand, LOCTEXT("Band", "BAND"), Bands,
		LOCTEXT("BandTip", "MODULATE: the band whose level scales the effect. TRIGGER: LEVEL steps on every hit (onset) instead of every beat.")));
	const FText Targets[] = { LOCTEXT("TargetLevel", "LEVEL"), LOCTEXT("TargetSize", "SIZE"), LOCTEXT("TargetRate", "RATE") };
	Add(BuildChoiceRow(EChoiceRow::AudioTarget, LOCTEXT("Modulates", "MOD"), Targets,
		LOCTEXT("TargetTip", "What the band scales: the top of the level range (intensity, beam), the size of the swing (position), or the speed.")));
	const FText BeatChoices[] = { LOCTEXT("BeatQuarter", "1/4"), LOCTEXT("BeatHalf", "1/2"), LOCTEXT("BeatOne", "1"), LOCTEXT("BeatTwo", "2"), LOCTEXT("BeatFour", "4"), LOCTEXT("BeatEight", "8") };
	Add(BuildChoiceRow(EChoiceRow::AudioBeats, LOCTEXT("Beats", "BEATS"), BeatChoices,
		LOCTEXT("BeatsTip", "SYNC: beats to one cycle of the wave. TRIGGER: beats between steps (1/4: four steps a beat).")));
	const FText Responses[] = { LOCTEXT("ResponseFast", "FAST"), LOCTEXT("ResponseMedium", "MED"), LOCTEXT("ResponseSlow", "SLOW") };
	Add(BuildChoiceRow(EChoiceRow::AudioResponse, LOCTEXT("Response", "RESP"), Responses,
		LOCTEXT("ResponseTip", "How fast the modulation falls back after a hit: FAST pumps with every kick, SLOW breathes.")));
	Add(BuildSettingRow(ESetting::Amount, LOCTEXT("Amount", "AMOUNT"), LOCTEXT("AmountTip", "How much the band scales the effect: 0 not at all, 100 % the band alone.")));

	Add(BuildSettingRow(ESetting::Rate, LOCTEXT("Rate", "RATE"), LOCTEXT("RateTip", "Cycles per second (and beats per minute). 0 holds the pattern still.")));
	Add(BuildSettingRow(ESetting::Size, LOCTEXT("Size", "SIZE"), LOCTEXT("SizeTip", "How far pan and tilt swing either side of their position.")));
	Add(BuildSettingRow(ESetting::Low, LOCTEXT("Low", "LOW"), LOCTEXT("LowTip", "The bottom of the wave, as a share of each fixture's range.")));
	Add(BuildSettingRow(ESetting::High, LOCTEXT("High", "HIGH"), LOCTEXT("HighTip", "The top of the wave, as a share of each fixture's range.")));
	Add(BuildSettingRow(ESetting::Phase, LOCTEXT("Phase", "PHASE"), LOCTEXT("PhaseTip", "Where in the cycle the first fixture starts.")));
	Add(BuildSettingRow(ESetting::Spread, LOCTEXT("Spread", "SPREAD"), LOCTEXT("SpreadTip", "How far apart in the cycle the fixtures are: 0 in step, 360 one wave across them all (a chase).")));
	Add(BuildSettingRow(ESetting::Duty, LOCTEXT("Duty", "DUTY"), LOCTEXT("DutyTip", "Square: the share of each cycle that is on. Short for a chase or a strobe.")));
	Add(BuildSettingRow(ESetting::Wings, LOCTEXT("Wings", "WINGS"), LOCTEXT("WingsTip", "Split the fixtures into mirrored parts, so the pattern runs symmetrically.")));
	Add(BuildSettingRow(ESetting::Blocks, LOCTEXT("Blocks", "BLOCKS"), LOCTEXT("BlocksTip", "Fixtures that move together as one step of the pattern.")));
	return Column;
}

UWidget* UStageEffectsPanel::BuildRunningColumn()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Width->SetWidthOverride(StageEffectsPanel::RunningWidth);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Width->SetContent(Column);
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UHorizontalBoxSlot* TextSlot = Title->AddChildToHorizontalBox(MakeText(LOCTEXT("RunningTitle", "RUNNING"), 7, TEXT("Bold"), UITheme.TextSecondary)))
	{
		TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextSlot->SetVerticalAlignment(VAlign_Center);
	}
	UStageChoiceButton* StopAll = MakeButton(LOCTEXT("StopAll", "STOP ALL"), LOCTEXT("StopAllTip", "Stop every running effect, the programmer's and the cues' (one undo step). Clear brings the cues' effects back."), 0, false);
	StopAll->OnPicked.BindUObject(this, &ThisClass::HandleStopAllPicked);
	Title->AddChildToHorizontalBox(StopAll);
	Column->AddChildToVerticalBox(Title);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(StageCraftWidgetStyle::MakeScrollBar(UITheme));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	RunningList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(RunningList);
	return Width;
}

void UStageEffectsPanel::RebuildRunningList()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	RunningList->ClearChildren();
	RunningRows.Reset();
	for (int32 Index = 0; Index < Listed.Num(); ++Index)
	{
		UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Frame->SetPadding(FMargin(2.f));
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Frame->SetContent(Line);
		UStageChoiceButton* Pick = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Pick->SetStyle(MakeFlatButton(FLinearColor::Transparent, UITheme.RowHover, UITheme.Divider));
		Pick->SetToolTipText(LOCTEXT("PickTip", "Edit this effect."));
		Pick->SetChoiceIndex(Index);
		Pick->OnPicked.BindUObject(this, &ThisClass::HandleRunningPicked);
		UTextBlock* Label = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextPrimary);
		Label->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Pick->SetClipping(EWidgetClipping::ClipToBounds);
		Pick->AddChild(Label);
		if (UHorizontalBoxSlot* PickSlot = Line->AddChildToHorizontalBox(Pick))
		{
			PickSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		UStageChoiceButton* Edit = MakeButton(LOCTEXT("Edit", "EDIT"),
			LOCTEXT("EditTip", "Take this cue's effect into the programmer to change it live (one undo step); store the cue to keep the change."), Index, false);
		Edit->OnPicked.BindUObject(this, &ThisClass::HandleEditPicked);
		Edit->SetVisibility(Listed[Index].Source == EStageEffectSource::Programmer ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		Line->AddChildToHorizontalBox(Edit);
		UStageChoiceButton* Stop = MakeButton(LOCTEXT("Stop", "STOP"),
			LOCTEXT("StopTip", "Stop this effect (one undo step). A cue's effect is held off by the programmer: store to record the stop, Clear to bring it back."), Index, false);
		Stop->OnPicked.BindUObject(this, &ThisClass::HandleStopPicked);
		Line->AddChildToHorizontalBox(Stop);
		if (UVerticalBoxSlot* RowSlot = RunningList->AddChildToVerticalBox(Frame))
		{
			RowSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
		}
		RunningRows.Add({ Frame, Label, Edit });
	}
}

void UStageEffectsPanel::NativeConstruct()
{
	Super::NativeConstruct();
	UStageProgrammerSubsystem* ProgrammerSubsystem = UWorld::GetSubsystem<UStageProgrammerSubsystem>(GetWorld());
	UShowControlSubsystem* ShowSubsystem = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	Programmer = ProgrammerSubsystem;
	ShowControl = ShowSubsystem;
	if (ProgrammerSubsystem)
	{
		ProgrammerSubsystem->OnValuesChanged.AddUniqueDynamic(this, &ThisClass::HandleProgrammerChanged);
	}
	if (ShowSubsystem)
	{
		ShowSubsystem->OnControlSourceChanged.AddUniqueDynamic(this, &ThisClass::HandleControlSourceChanged);
		// Cues and playbacks start and stop effects too (Docs/ADR/0027 §5).
		ShowSubsystem->OnCueStarted.AddUniqueDynamic(this, &ThisClass::HandleCueEvent);
		ShowSubsystem->OnCueListChanged.AddUniqueDynamic(this, &ThisClass::HandleShowChanged);
		ShowSubsystem->OnDeskStateChanged.AddUniqueDynamic(this, &ThisClass::HandleShowChanged);
	}
	Refresh();
}

void UStageEffectsPanel::NativeDestruct()
{
	if (UStageProgrammerSubsystem* ProgrammerSubsystem = Programmer.Get())
	{
		ProgrammerSubsystem->OnValuesChanged.RemoveDynamic(this, &ThisClass::HandleProgrammerChanged);
	}
	if (UShowControlSubsystem* ShowSubsystem = ShowControl.Get())
	{
		ShowSubsystem->OnControlSourceChanged.RemoveDynamic(this, &ThisClass::HandleControlSourceChanged);
		ShowSubsystem->OnCueStarted.RemoveDynamic(this, &ThisClass::HandleCueEvent);
		ShowSubsystem->OnCueListChanged.RemoveDynamic(this, &ThisClass::HandleShowChanged);
		ShowSubsystem->OnDeskStateChanged.RemoveDynamic(this, &ThisClass::HandleShowChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimer);
	}
	Programmer.Reset();
	ShowControl.Reset();
	Super::NativeDestruct();
}

void UStageEffectsPanel::HandleProgrammerChanged()
{
	RequestRefresh();
}

void UStageEffectsPanel::HandleControlSourceChanged(EStageControlSource NewSource)
{
	RequestRefresh();
}

void UStageEffectsPanel::HandleShowChanged()
{
	RequestRefresh();
}

void UStageEffectsPanel::HandleCueEvent(float CueNumber)
{
	RequestRefresh();
}

void UStageEffectsPanel::RequestRefresh()
{
	UWorld* World = GetWorld();
	if (!World || RefreshTimer.IsValid())
	{
		return;
	}
	// Next tick: never inside the slider or button callback that caused it, and a drag's burst of changes is one refresh a frame.
	RefreshTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Refresh));
}

const FStageEffectEntry* UStageEffectsPanel::FindSelected() const
{
	const UStageProgrammerSubsystem* Subsystem = Programmer.Get();
	if (!Subsystem || !SelectedEffectId.IsValid())
	{
		return nullptr;
	}
	if (const FStageEffectEntry* Own = Subsystem->FindEffect(SelectedEffectId))
	{
		return Own;
	}
	const FStageRunningEffect* Shown = Listed.FindByPredicate([this](const FStageRunningEffect& Effect) { return Effect.Entry.Id == SelectedEffectId; });
	return Shown ? &Shown->Entry : nullptr;
}

bool UStageEffectsPanel::IsSelectedEditable() const
{
	const UStageProgrammerSubsystem* Subsystem = Programmer.Get();
	return Subsystem && SelectedEffectId.IsValid() && Subsystem->FindEffect(SelectedEffectId);
}

bool UStageEffectsPanel::GetPreviewState(FStageEffect& OutEffect, int32& OutCount, double& OutCycles) const
{
	const FStageEffectEntry* Selected = FindSelected();
	const UShowControlSubsystem* Show = ShowControl.Get();
	if (!Selected || !Show)
	{
		return false;
	}
	OutEffect = Selected->Effect;
	OutCount = Selected->FixtureIds.Num();
	OutCycles = Show->GetEffectCycles(Selected->Id);
	return true;
}

void UStageEffectsPanel::Refresh()
{
	RefreshTimer.Invalidate();
	const UStageProgrammerSubsystem* Subsystem = Programmer.Get();
	const UShowControlSubsystem* Show = ShowControl.Get();
	if (!Subsystem || !Show)
	{
		return;
	}
	// Every source's effects (Docs/ADR/0027 §5), without those fading out: they are already gone as far as the user is concerned.
	TArray<FStageRunningEffect> Running;
	Show->GetRunningEffects(Running);
	Running.RemoveAll([](const FStageRunningEffect& Effect) { return Effect.bStopping; });
	const bool bSameSet = Running.Num() == Listed.Num() && Algo::Compare(Running, Listed, [](const FStageRunningEffect& A, const FStageRunningEffect& B)
	{
		return A.Entry.Id == B.Entry.Id && A.Source == B.Source && A.Playback == B.Playback;
	});
	Listed = MoveTemp(Running);
	if (!bSameSet)
	{
		RebuildRunningList();
	}

	// The editor follows the effect it showed; when that one stopped, the newest of the programmer's, else the newest running.
	if (!FindSelected())
	{
		SelectedEffectId = Subsystem->HasEffects() ? Subsystem->GetEffects().Last().Id : (Listed.IsEmpty() ? FGuid() : Listed.Last().Entry.Id);
	}
	const bool bEnabled = Show->GetControlSource() == EStageControlSource::Internal;
	const int32 Count = Listed.Num();
	StatusText->SetText(!bEnabled ? LOCTEXT("External", "An external console is driving the fixtures: effects hold still.")
		: Count == 0 ? LOCTEXT("NoneRunning", "No effect running.")
		: FindSelected() && !IsSelectedEditable() ? LOCTEXT("FromCue", "This effect runs from a cue or playback: EDIT takes it into the programmer.")
		: FText::Format(LOCTEXT("Running", "{0} running · TAP the rate in time with the music."), FText::AsNumber(Count)));
	RefreshRunningList(bEnabled);
	RefreshEditor(FindSelected(), bEnabled && IsSelectedEditable());

	const UStageCraftUITheme& UITheme = GetTheme();
	if (UStageValueSlider* Slider = AudioMasterSlider.Get(); Slider && !Slider->HasMouseCapture())
	{
		Slider->SetValue(Show->GetAudioMaster());
	}
	if (UTextBlock* Value = AudioMasterText.Get())
	{
		// The slider's own level; the lit KILL beside it says when the music is ignored.
		Value->SetText(FText::Format(LOCTEXT("AudioMasterValue", "{0}%"), FText::AsNumber(FMath::RoundToInt(Show->GetAudioMaster() * 100.f))));
	}
	if (UStageChoiceButton* Kill = AudioKillButton.Get())
	{
		Kill->SetStyle(MakeButtonStyle(Show->IsAudioKilled(), UITheme.WarningColor));
		if (UTextBlock* Label = Cast<UTextBlock>(Kill->GetChildAt(0)))
		{
			Label->SetColorAndOpacity(FSlateColor(Show->IsAudioKilled() ? UITheme.TextPrimary : UITheme.TextSecondary));
		}
	}
}

void UStageEffectsPanel::RefreshRunningList(bool bEnabled)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	for (int32 Index = 0; Index < RunningRows.Num() && Index < Listed.Num(); ++Index)
	{
		const FStageEffectEntry& Entry = Listed[Index].Entry;
		const bool bSelected = Entry.Id == SelectedEffectId;
		if (UBorder* Frame = RunningRows[Index].Frame.Get())
		{
			Frame->SetBrush(MakeBox(bSelected ? WithAlpha(UITheme.AccentColor, 0.18f) : UITheme.RowBackground, CornerRadius,
				bSelected ? UITheme.AccentColor : UITheme.Divider, 1.f));
			Frame->SetIsEnabled(bEnabled);
		}
		if (UTextBlock* Label = RunningRows[Index].Label.Get())
		{
			const FStageRunningEffect& Effect = Listed[Index];
			const FText Source = Effect.Source == EStageEffectSource::Programmer ? LOCTEXT("SourceProgrammer", "PRG")
				: Effect.Source == EStageEffectSource::Playback ? FText::Format(LOCTEXT("SourcePlayback", "PB {0}"), FText::AsNumber(Effect.Playback + 1))
				: LOCTEXT("SourceCue", "CUE");
			// An effect that follows the music says how: its own rate means nothing while Sync or Trigger drives its clock.
			const FStageEffectAudio& Audio = Entry.Effect.Audio;
			const FText Speed = Audio.Mode == EStageEffectAudioMode::Sync ? LOCTEXT("SpeedSync", "♪ SYNC")
				: Audio.Mode == EStageEffectAudioMode::Trigger ? LOCTEXT("SpeedTrigger", "♪ TRIGGER")
				: Audio.Mode == EStageEffectAudioMode::Modulate && Entry.Effect.RateHz <= 0.f ? LOCTEXT("SpeedModulateStill", "♪ MODULATE")
				: Audio.Mode == EStageEffectAudioMode::Modulate ? FText::Format(LOCTEXT("SpeedModulate", "{0} Hz ♪"), FText::AsNumber(Entry.Effect.RateHz, &StageEffectsPanel::RateFormat()))
				: FText::Format(LOCTEXT("SpeedHz", "{0} Hz"), FText::AsNumber(Entry.Effect.RateHz, &StageEffectsPanel::RateFormat()));
			Label->SetText(FText::Format(LOCTEXT("RunningLine", "{0} · {1} · {2}"), Entry.Effect.Name, Source, Speed));
		}
	}
}

void UStageEffectsPanel::RefreshEditor(const FStageEffectEntry* Selected, bool bEnabled)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	const bool bHasSelection = Selected != nullptr;
	EditorBody->SetVisibility(bHasSelection ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	EditorHint->SetVisibility(bHasSelection ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	EditorBody->SetIsEnabled(bEnabled);
	if (!bHasSelection)
	{
		EditorTitle->SetText(LOCTEXT("NoEffect", "NO EFFECT SELECTED"));
		return;
	}
	const FStageEffect& Effect = Selected->Effect;
	const FText TargetName = StaticEnum<EStageEffectTarget>()->GetDisplayNameTextByValue(static_cast<int64>(Effect.Target));
	EditorTitle->SetText(FText::Format(LOCTEXT("EditorTitle", "{0}  ·  {1} on {2} fixture(s)"), Effect.Name, TargetName, FText::AsNumber(Selected->FixtureIds.Num())));

	const bool bPosition = Effect.Target == EStageEffectTarget::Position;
	const bool bLevels = Effect.Target == EStageEffectTarget::Intensity || Effect.Target == EStageEffectTarget::Beam;
	const bool bColor = Effect.Target == EStageEffectTarget::Color;
	const bool bTwoAxis = Effect.Waveform == EStageEffectWaveform::Circle || Effect.Waveform == EStageEffectWaveform::Figure8;
	const auto Show = [](const TWeakObjectPtr<UWidget>& Row, bool bVisible)
	{
		if (UWidget* Widget = Row.Get())
		{
			Widget->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	};
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::Axis)].Row, bPosition && !bTwoAxis);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::ColorMode)].Row, bColor);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::ColorA)].Row, bColor && Effect.ColorMode == EStageEffectColorMode::Crossfade);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::ColorB)].Row, bColor && Effect.ColorMode == EStageEffectColorMode::Crossfade);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::BeamAttribute)].Row, Effect.Target == EStageEffectTarget::Beam);
	Show(SettingRows[static_cast<int32>(ESetting::Size)].Row, bPosition);
	Show(SettingRows[static_cast<int32>(ESetting::Low)].Row, bLevels);
	Show(SettingRows[static_cast<int32>(ESetting::High)].Row, bLevels);
	Show(SettingRows[static_cast<int32>(ESetting::Duty)].Row, Effect.Waveform == EStageEffectWaveform::Square);
	const EStageEffectAudioMode AudioMode = Effect.Audio.Mode;
	const bool bModulate = AudioMode == EStageEffectAudioMode::Modulate;
	const bool bClockFromAudio = StageEffectRules::IsClockFromAudio(Effect.Audio);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::AudioBand)].Row, bModulate || AudioMode == EStageEffectAudioMode::Trigger);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::AudioTarget)].Row, bModulate);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::AudioBeats)].Row, bClockFromAudio);
	Show(ChoiceRows[static_cast<int32>(EChoiceRow::AudioResponse)].Row, bModulate);
	Show(SettingRows[static_cast<int32>(ESetting::Amount)].Row, bModulate);
	// Sync and Trigger take the clock from the music: the effect's own rate is kept (it is used again with no tempo) but greyed.
	if (UWidget* RateRow = SettingRows[static_cast<int32>(ESetting::Rate)].Row.Get())
	{
		RateRow->SetIsEnabled(!bClockFromAudio);
	}

	for (int32 Row = 0; Row < ChoiceRows.Num(); ++Row)
	{
		const int32 Lit = GetLitOption(static_cast<EChoiceRow>(Row), Effect);
		const bool bColorRow = Row == static_cast<int32>(EChoiceRow::ColorA) || Row == static_cast<int32>(EChoiceRow::ColorB);
		for (int32 Option = 0; Option < ChoiceRows[Row].Buttons.Num(); ++Option)
		{
			UStageChoiceButton* Button = ChoiceRows[Row].Buttons[Option].Get();
			if (!Button)
			{
				continue;
			}
			const FLinearColor LitColor = bColorRow ? StageEffectsPanel::ColorChoices[Option] : UITheme.AccentColor;
			Button->SetStyle(MakeButtonStyle(Option == Lit, LitColor));
			if (UTextBlock* Label = Cast<UTextBlock>(Button->GetChildAt(0)))
			{
				Label->SetColorAndOpacity(FSlateColor(Option == Lit ? UITheme.TextPrimary : UITheme.TextSecondary));
			}
		}
	}
	for (int32 Index = 0; Index < SettingRows.Num(); ++Index)
	{
		const ESetting Setting = static_cast<ESetting>(Index);
		if (UStageValueSlider* Slider = SettingRows[Index].Slider.Get(); Slider && !Slider->HasMouseCapture())
		{
			Slider->SetValue(ToSliderValue(Setting, Effect));
		}
		if (UTextBlock* Value = SettingRows[Index].Value.Get())
		{
			Value->SetText(FormatSetting(Setting, Effect));
		}
	}
}

float UStageEffectsPanel::ToSliderValue(ESetting Setting, const FStageEffect& Effect)
{
	using namespace StageEffectsPanel;
	switch (Setting)
	{
	case ESetting::Rate:	return FMath::Sqrt(FMath::Clamp(Effect.RateHz / SliderMaxRateHz, 0.f, 1.f));
	case ESetting::Size:	return FMath::Clamp(Effect.Size / MaxSliderSize, 0.f, 1.f);
	case ESetting::Low:		return Effect.Low;
	case ESetting::High:	return Effect.High;
	case ESetting::Phase:	return FMath::Clamp(FMath::Fmod(Effect.Phase, 360.f) / 360.f, 0.f, 1.f);
	case ESetting::Spread:	return FMath::Clamp(Effect.Spread / MaxSliderSpread, 0.f, 1.f);
	case ESetting::Duty:	return Effect.Duty;
	case ESetting::Wings:	return FMath::Clamp(static_cast<float>(Effect.Wings - 1) / (MaxSliderWings - 1), 0.f, 1.f);
	case ESetting::Blocks:	return FMath::Clamp(static_cast<float>(Effect.Blocks - 1) / (MaxSliderBlocks - 1), 0.f, 1.f);
	case ESetting::Amount:	return Effect.Audio.Amount;
	default:				return 0.f;
	}
}

void UStageEffectsPanel::FromSliderValue(ESetting Setting, float SliderValue, FStageEffect& InOutEffect)
{
	using namespace StageEffectsPanel;
	const float Value = FMath::Clamp(SliderValue, 0.f, 1.f);
	switch (Setting)
	{
	case ESetting::Rate:	InOutEffect.RateHz = SliderMaxRateHz * Value * Value; break;
	case ESetting::Size:	InOutEffect.Size = FMath::RoundToFloat(Value * MaxSliderSize); break;
	case ESetting::Low:		InOutEffect.Low = Value; break;
	case ESetting::High:	InOutEffect.High = Value; break;
	case ESetting::Phase:	InOutEffect.Phase = FMath::RoundToFloat(Value * 360.f); break;
	case ESetting::Spread:	InOutEffect.Spread = FMath::RoundToFloat(Value * MaxSliderSpread); break;
	case ESetting::Duty:	InOutEffect.Duty = Value; break;
	case ESetting::Wings:	InOutEffect.Wings = 1 + FMath::RoundToInt(Value * (MaxSliderWings - 1)); break;
	case ESetting::Blocks:	InOutEffect.Blocks = 1 + FMath::RoundToInt(Value * (MaxSliderBlocks - 1)); break;
	case ESetting::Amount:	InOutEffect.Audio.Amount = Value; break;
	default:				break;
	}
}

FText UStageEffectsPanel::FormatSetting(ESetting Setting, const FStageEffect& Effect)
{
	using namespace StageEffectsPanel;
	switch (Setting)
	{
	case ESetting::Rate:
		return FText::Format(LOCTEXT("RateValue", "{0} Hz · {1} BPM"),
			FText::AsNumber(Effect.RateHz, &StageEffectsPanel::RateFormat(2)),
			FText::AsNumber(FMath::RoundToInt(StageEffectRules::HzToBPM(Effect.RateHz))));
	case ESetting::Size:	return FormatDegrees(Effect.Size);
	case ESetting::Low:		return FormatPercent(Effect.Low);
	case ESetting::High:	return FormatPercent(Effect.High);
	case ESetting::Phase:	return FormatDegrees(Effect.Phase);
	case ESetting::Spread:	return FormatDegrees(Effect.Spread);
	case ESetting::Duty:	return FormatPercent(Effect.Duty);
	case ESetting::Wings:	return FText::AsNumber(Effect.Wings);
	case ESetting::Blocks:	return FText::AsNumber(Effect.Blocks);
	case ESetting::Amount:	return FormatPercent(Effect.Audio.Amount);
	default:				return FText::GetEmpty();
	}
}

int32 UStageEffectsPanel::GetLitOption(EChoiceRow Row, const FStageEffect& Effect)
{
	using namespace StageEffectsPanel;
	const auto FindColor = [](const FLinearColor& Color)
	{
		for (int32 Option = 0; Option < UE_ARRAY_COUNT(ColorChoices); ++Option)
		{
			if (Color.Equals(ColorChoices[Option], 1e-3f))
			{
				return Option;
			}
		}
		return static_cast<int32>(INDEX_NONE);
	};
	switch (Row)
	{
	case EChoiceRow::Waveform:	return static_cast<int32>(Effect.Waveform);
	case EChoiceRow::Direction:	return static_cast<int32>(Effect.Direction);
	case EChoiceRow::Fan:		return static_cast<int32>(Effect.SizeFan);
	case EChoiceRow::Axis:		return static_cast<int32>(Effect.Axis);
	case EChoiceRow::ColorMode:	return static_cast<int32>(Effect.ColorMode);
	case EChoiceRow::ColorA:	return FindColor(Effect.ColorA);
	case EChoiceRow::ColorB:	return FindColor(Effect.ColorB);
	case EChoiceRow::BeamAttribute:
		for (int32 Option = 0; Option < BeamChoiceCount; ++Option)
		{
			if (Effect.Attribute == BeamChoice(Option))
			{
				return Option;
			}
		}
		return INDEX_NONE;
	case EChoiceRow::AudioMode:		return static_cast<int32>(Effect.Audio.Mode);
	case EChoiceRow::AudioBand:		return static_cast<int32>(Effect.Audio.Band);
	case EChoiceRow::AudioTarget:	return static_cast<int32>(Effect.Audio.Modulates);
	case EChoiceRow::AudioBeats:	return static_cast<int32>(Effect.Audio.BeatsPerCycle);
	case EChoiceRow::AudioResponse:	return static_cast<int32>(Effect.Audio.Response);
	default:
		return INDEX_NONE;
	}
}

bool UStageEffectsPanel::ApplyOption(EChoiceRow Row, int32 Option, FStageEffect& InOutEffect)
{
	using namespace StageEffectsPanel;
	const FStageEffect Before = InOutEffect;
	switch (Row)
	{
	case EChoiceRow::Waveform:	InOutEffect.Waveform = static_cast<EStageEffectWaveform>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectWaveform::Figure8))); break;
	case EChoiceRow::Direction:	InOutEffect.Direction = static_cast<EStageEffectDirection>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectDirection::Bounce))); break;
	case EChoiceRow::Fan:		InOutEffect.SizeFan = static_cast<EStageFanMode>(FMath::Clamp(Option, 0, static_cast<int32>(EStageFanMode::InvertedV))); break;
	case EChoiceRow::Axis:		InOutEffect.Axis = static_cast<EStageEffectAxis>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectAxis::Both))); break;
	case EChoiceRow::ColorMode:	InOutEffect.ColorMode = static_cast<EStageEffectColorMode>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectColorMode::Rainbow))); break;
	case EChoiceRow::ColorA:	InOutEffect.ColorA = ColorChoices[FMath::Clamp(Option, 0, static_cast<int32>(UE_ARRAY_COUNT(ColorChoices)) - 1)]; break;
	case EChoiceRow::ColorB:	InOutEffect.ColorB = ColorChoices[FMath::Clamp(Option, 0, static_cast<int32>(UE_ARRAY_COUNT(ColorChoices)) - 1)]; break;
	case EChoiceRow::BeamAttribute:	InOutEffect.Attribute = BeamChoice(Option); break;
	case EChoiceRow::AudioMode:		InOutEffect.Audio.Mode = static_cast<EStageEffectAudioMode>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectAudioMode::Trigger))); break;
	case EChoiceRow::AudioBand:		InOutEffect.Audio.Band = static_cast<EStageAudioBand>(FMath::Clamp(Option, 0, static_cast<int32>(EStageAudioBand::Level))); break;
	case EChoiceRow::AudioTarget:	InOutEffect.Audio.Modulates = static_cast<EStageEffectAudioTarget>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectAudioTarget::Rate))); break;
	case EChoiceRow::AudioBeats:	InOutEffect.Audio.BeatsPerCycle = static_cast<EStageEffectBeatDivision>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectBeatDivision::Eight))); break;
	case EChoiceRow::AudioResponse:	InOutEffect.Audio.Response = static_cast<EStageEffectAudioResponse>(FMath::Clamp(Option, 0, static_cast<int32>(EStageEffectAudioResponse::Slow))); break;
	default:					break;
	}
	return !InOutEffect.Equals(Before);
}

void UStageEffectsPanel::CommitSelected(const FStageEffect& Effect)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && SelectedEffectId.IsValid())
	{
		Controller->RequestUpdateEffect(SelectedEffectId, Effect);
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleQuickPicked(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller || !QuickEffects.IsValidIndex(Index))
	{
		return;
	}
	const FGuid Started = Controller->RequestStartEffect(QuickEffects[Index]);
	if (Started.IsValid())
	{
		SelectedEffectId = Started;
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleChoicePicked(int32 ChoiceIndex)
{
	const FStageEffectEntry* Selected = FindSelected();
	const int32 Row = ChoiceIndex / StageEffectsPanel::ChoiceStride;
	if (!Selected || Row < 0 || Row >= static_cast<int32>(EChoiceRow::Count))
	{
		return;
	}
	FStageEffect Effect = Selected->Effect;
	if (ApplyOption(static_cast<EChoiceRow>(Row), ChoiceIndex % StageEffectsPanel::ChoiceStride, Effect))
	{
		CommitSelected(Effect);
	}
}

void UStageEffectsPanel::HandleSliderMoved(int32 SliderIndex, float Value)
{
	const FStageEffectEntry* Selected = FindSelected();
	AModularPlayerController* Controller = GetStageController();
	if (!Selected || !Controller || SliderIndex < 0 || SliderIndex >= static_cast<int32>(ESetting::Count))
	{
		return;
	}
	FStageEffect Effect = Selected->Effect;
	FromSliderValue(static_cast<ESetting>(SliderIndex), Value, Effect);
	// A drag previews live; its release (HandleSliderReleased) records the one undo step.
	Controller->RequestPreviewEffect(SelectedEffectId, Effect);
	RequestRefresh();
}

void UStageEffectsPanel::HandleSliderReleased()
{
	if (const FStageEffectEntry* Selected = FindSelected())
	{
		CommitSelected(Selected->Effect);
	}
}

void UStageEffectsPanel::HandleRunningPicked(int32 Index)
{
	if (Listed.IsValidIndex(Index))
	{
		SelectedEffectId = Listed[Index].Entry.Id;
		TapTimes.Reset();
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleStopPicked(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && Listed.IsValidIndex(Index))
	{
		Controller->RequestStopEffect(Listed[Index].Entry.Id);
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleEditPicked(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && Listed.IsValidIndex(Index) && Controller->RequestEditEffect(Listed[Index].Entry.Id))
	{
		SelectedEffectId = Listed[Index].Entry.Id;
		TapTimes.Reset();
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleAudioMasterMoved(int32 SliderIndex, float Value)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestSetAudioMaster(Value);
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleAudioKillPicked(int32 ChoiceIndex)
{
	AModularPlayerController* Controller = GetStageController();
	const UShowControlSubsystem* Show = ShowControl.Get();
	if (Controller && Show)
	{
		Controller->RequestSetAudioKill(!Show->IsAudioKilled());
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleStopAllPicked(int32 ChoiceIndex)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestStopEffect(FGuid());
	}
	RequestRefresh();
}

void UStageEffectsPanel::HandleTapPicked(int32 ChoiceIndex)
{
	const FStageEffectEntry* Selected = FindSelected();
	if (!Selected)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (!TapTimes.IsEmpty() && Now - TapTimes.Last() > StageEffectsPanel::TapResetSeconds)
	{
		TapTimes.Reset();
	}
	TapTimes.Add(Now);
	if (TapTimes.Num() > StageEffectsPanel::MaxTaps)
	{
		TapTimes.RemoveAt(0);
	}
	if (TapTimes.Num() < 2)
	{
		return;
	}
	// One cycle per tap: the average spacing of the last taps.
	const double Spacing = (TapTimes.Last() - TapTimes[0]) / (TapTimes.Num() - 1);
	if (Spacing <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	FStageEffect Effect = Selected->Effect;
	Effect.RateHz = static_cast<float>(1.0 / Spacing);
	CommitSelected(Effect);
}

#undef LOCTEXT_NAMESPACE
