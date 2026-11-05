// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageAudioPanel.h"

#include "Audio/StageAudioSubsystem.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Import/StageFileDialog.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"
#include "UI/StageValueSlider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudioPanel)

#define LOCTEXT_NAMESPACE "StageAudioPanel"

namespace StageAudioPanel
{
	constexpr float LabelWidth = 58.f;
	constexpr float ValueWidth = 92.f;
	constexpr float LampSize = 14.f;
	/** The lamp is lit for the first part of each beat. */
	constexpr float LampOnPhase = 0.15f;

	enum ETransport : int32 { PlayPause, Stop, Loop };
	enum ESlider : int32 { Seek, Gain };

	/** Up to one decimal (exactly one with bFixed), no grouping. */
	const FNumberFormattingOptions& OneDecimal(bool bFixed)
	{
		static const FNumberFormattingOptions Loose = FNumberFormattingOptions().SetUseGrouping(false).SetMaximumFractionalDigits(1);
		static const FNumberFormattingOptions Fixed = FNumberFormattingOptions().SetUseGrouping(false).SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		return bFixed ? Fixed : Loose;
	}

	FText FormatTime(double Seconds)
	{
		const int32 Whole = FMath::Max(0, FMath::FloorToInt(Seconds));
		return FText::FromString(FString::Printf(TEXT("%d:%02d"), Whole / 60, Whole % 60));
	}
}

// --- Construction ---

const UStageCraftUITheme& UStageAudioPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageAudioPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UStageAudioReactiveSubsystem* UStageAudioPanel::GetAudio() const
{
	return Audio.Get();
}

UTextBlock* UStageAudioPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

FButtonStyle UStageAudioPanel::MakeButtonStyle(bool bLit) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	const FLinearColor Fill = bLit ? WithAlpha(UITheme.AccentColor, 0.45f) : FLinearColor::Transparent;
	FButtonStyle Style = MakeFlatButton(Fill, bLit ? WithAlpha(UITheme.AccentColor, 0.6f) : UITheme.RowHover, UITheme.Divider);
	Style.SetNormal(MakeBox(Fill, CornerRadius, UITheme.Divider, 1.f));
	return Style;
}

UStageChoiceButton* UStageAudioPanel::MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetStyle(MakeButtonStyle(false));
	Button->SetToolTipText(ToolTip);
	Button->SetChoiceIndex(ChoiceIndex);
	UTextBlock* Text = MakeText(Label, 7, TEXT("Bold"), UITheme.TextSecondary);
	Text->SetJustification(ETextJustify::Center);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(6.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return Button;
}

void UStageAudioPanel::SetButtonLit(UStageChoiceButton* Button, bool bLit) const
{
	if (!Button)
	{
		return;
	}
	const UStageCraftUITheme& UITheme = GetTheme();
	Button->SetStyle(MakeButtonStyle(bLit));
	if (UTextBlock* Label = Cast<UTextBlock>(Button->GetChildAt(0)))
	{
		Label->SetColorAndOpacity(FSlateColor(bLit ? UITheme.TextPrimary : UITheme.TextSecondary));
	}
}

UWidget* UStageAudioPanel::MakeLabeledRow(const FText& Label, UWidget* Content)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelBox->SetWidthOverride(StageAudioPanel::LabelWidth);
	LabelBox->SetContent(MakeText(Label, 7, TEXT("Bold"), UITheme.TextSecondary));
	if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}
	if (UHorizontalBoxSlot* ContentSlot = Row->AddChildToHorizontalBox(Content))
	{
		ContentSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ContentSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Row;
}

void UStageAudioPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageAudioPanel::BuildTree()
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
	HeaderLine->AddChildToHorizontalBox(MakeText(LOCTEXT("Title", "AUDIO"), 9, TEXT("Bold"), UITheme.TextPrimary));
	StatusText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextSecondary);
	if (UHorizontalBoxSlot* StatusSlot = HeaderLine->AddChildToHorizontalBox(StatusText))
	{
		StatusSlot->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
		StatusSlot->SetVerticalAlignment(VAlign_Center);
		StatusSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	Column->AddChildToVerticalBox(Header);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(MakeScrollBar(UITheme));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(10.f, 6.f));
	}
	UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(Rows);
	for (UWidget* Row : { BuildSourceRow(), BuildFileRow(), BuildInputRow(), BuildMeters(), BuildFooter() })
	{
		if (UVerticalBoxSlot* RowSlot = Rows->AddChildToVerticalBox(Row))
		{
			RowSlot->SetPadding(FMargin(0.f, 3.f));
		}
	}
}

UWidget* UStageAudioPanel::BuildSourceRow()
{
	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	const TPair<FText, FText> Sources[] =
	{
		{ LOCTEXT("SourceOff", "OFF"), LOCTEXT("SourceOffTip", "No music: effects that follow it hold their pattern, or stay fully lit.") },
		{ LOCTEXT("SourceFile", "FILE..."), LOCTEXT("SourceFileTip", "Play a song from this PC (WAV, FLAC, MP3, M4A, AAC or WMA) and follow it.") },
		{ LOCTEXT("SourceInput", "INPUT"), LOCTEXT("SourceInputTip", "Follow a line input or microphone. It is only listened to, never played back or recorded.") },
		{ LOCTEXT("SourceSystem", "SYSTEM"), LOCTEXT("SourceSystemTip", "Follow whatever this PC plays: YouTube in a browser, a music app. Listened to in memory only, never recorded.") },
		{ LOCTEXT("SourceTest", "TEST"), LOCTEXT("SourceTestTip", "A built-in beat at a steady tempo, to try effects without music.") },
	};
	static_assert(UE_ARRAY_COUNT(Sources) == static_cast<int32>(EStageAudioSource::TestSignal) + 1, "One button per source.");
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Sources); ++Index)
	{
		UStageChoiceButton* Button = MakeButton(Sources[Index].Key, Sources[Index].Value, Index);
		Button->OnPicked.BindUObject(this, &ThisClass::HandleSourcePicked);
		if (UHorizontalBoxSlot* ButtonSlot = Buttons->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(0.f, 0.f, 3.f, 0.f));
		}
		SourceButtons.Add(Button);
	}
	return MakeLabeledRow(LOCTEXT("Source", "SOURCE"), Buttons);
}

UWidget* UStageAudioPanel::BuildFileRow()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UVerticalBox* Block = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UTextBlock* Name = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextPrimary);
	FileName = Name;
	Block->AddChildToVerticalBox(Name);

	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	const TPair<FText, FText> Commands[] =
	{
		{ LOCTEXT("Play", "PLAY"), LOCTEXT("PlayTip", "Play or pause the song.") },
		{ LOCTEXT("Stop", "STOP"), LOCTEXT("StopTip", "Stop and go back to the start.") },
		{ LOCTEXT("Loop", "LOOP"), LOCTEXT("LoopTip", "Start again at the end.") },
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Commands); ++Index)
	{
		UStageChoiceButton* Button = MakeButton(Commands[Index].Key, Commands[Index].Value, Index);
		Button->OnPicked.BindUObject(this, &ThisClass::HandleTransportPicked);
		if (UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(0.f, 0.f, 3.f, 0.f));
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
		}
		if (Index == StageAudioPanel::PlayPause)
		{
			PlayButton = Button;
		}
		else if (Index == StageAudioPanel::Loop)
		{
			LoopButton = Button;
		}
	}
	UStageValueSlider* Seek = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Seek->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(UITheme));
	Seek->SetSliderHandleColor(FLinearColor::White);
	Seek->SetStepSize(0.001f);
	Seek->SetSliderIndex(StageAudioPanel::Seek);
	Seek->SetToolTipText(LOCTEXT("SeekTip", "Drag to a point in the song; it plays on from there when you let go."));
	Seek->OnMoved.BindUObject(this, &ThisClass::HandleSliderMoved);
	Seek->OnMouseCaptureEnd.AddUniqueDynamic(this, &ThisClass::HandleSliderReleased);
	SeekSlider = Seek;
	if (UHorizontalBoxSlot* SeekSlot = Line->AddChildToHorizontalBox(Seek))
	{
		SeekSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SeekSlot->SetVerticalAlignment(VAlign_Center);
		SeekSlot->SetPadding(FMargin(4.f, 0.f));
	}
	USizeBox* TimeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	TimeBox->SetWidthOverride(StageAudioPanel::ValueWidth);
	UTextBlock* Time = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextPrimary);
	Time->SetJustification(ETextJustify::Right);
	TimeBox->SetContent(Time);
	PositionText = Time;
	if (UHorizontalBoxSlot* TimeSlot = Line->AddChildToHorizontalBox(TimeBox))
	{
		TimeSlot->SetVerticalAlignment(VAlign_Center);
	}
	Block->AddChildToVerticalBox(Line);
	UWidget* Row = MakeLabeledRow(LOCTEXT("File", "FILE"), Block);
	FileRow = Row;
	return Row;
}

UWidget* UStageAudioPanel::BuildInputRow()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UVerticalBox* Block = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UStageThemedComboBox* Combo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass(), TEXT("DeviceCombo"));
	Combo->ApplyTheme(UITheme);
	Combo->SetToolTipText(LOCTEXT("DeviceTip", "The recording device INPUT listens to."));
	DeviceCombo = Combo;
	Block->AddChildToVerticalBox(Combo);

	UHorizontalBox* GainLine = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	GainLine->SetToolTipText(LOCTEXT("GainTip", "Boost a quiet input or cut a loud one before it is analysed (-24 .. +24 dB)."));
	GainLine->AddChildToHorizontalBox(MakeText(LOCTEXT("Gain", "GAIN"), 7, TEXT("Bold"), UITheme.TextSecondary));
	UStageValueSlider* Gain = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
	Gain->SetWidgetStyle(StageCraftWidgetStyle::MakeSlider(UITheme));
	Gain->SetSliderHandleColor(FLinearColor::White);
	Gain->SetStepSize(1.f / 48.f);
	Gain->SetSliderIndex(StageAudioPanel::Gain);
	Gain->OnMoved.BindUObject(this, &ThisClass::HandleSliderMoved);
	Gain->OnMouseCaptureEnd.AddUniqueDynamic(this, &ThisClass::HandleSliderReleased);
	GainSlider = Gain;
	if (UHorizontalBoxSlot* GainSlot = GainLine->AddChildToHorizontalBox(Gain))
	{
		GainSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		GainSlot->SetVerticalAlignment(VAlign_Center);
		GainSlot->SetPadding(FMargin(6.f, 0.f));
	}
	USizeBox* ValueBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ValueBox->SetWidthOverride(StageAudioPanel::ValueWidth);
	UTextBlock* Value = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextPrimary);
	Value->SetJustification(ETextJustify::Right);
	ValueBox->SetContent(Value);
	GainText = Value;
	GainLine->AddChildToHorizontalBox(ValueBox);
	if (UVerticalBoxSlot* GainSlot = Block->AddChildToVerticalBox(GainLine))
	{
		GainSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
	}
	UWidget* Row = MakeLabeledRow(LOCTEXT("Input", "INPUT"), Block);
	InputRow = Row;
	return Row;
}

UWidget* UStageAudioPanel::BuildMeters()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	UVerticalBox* Block = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

	UHorizontalBox* TempoLine = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	TempoLine->SetToolTipText(LOCTEXT("TempoTip", "The tempo found in the music, how sure the analyser is of it, and a lamp on every beat (as the audience hears it)."));
	UBorder* Lamp = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Lamp->SetBrush(MakeBox(UITheme.RowBackground, StageAudioPanel::LampSize * 0.5f, UITheme.Divider, 1.f));
	USizeBox* LampBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LampBox->SetWidthOverride(StageAudioPanel::LampSize);
	LampBox->SetHeightOverride(StageAudioPanel::LampSize);
	LampBox->SetContent(Lamp);
	BeatLamp = Lamp;
	if (UHorizontalBoxSlot* LampSlot = TempoLine->AddChildToHorizontalBox(LampBox))
	{
		LampSlot->SetVerticalAlignment(VAlign_Center);
	}
	UTextBlock* Tempo = MakeText(FText::GetEmpty(), 9, TEXT("Bold"), UITheme.TextPrimary);
	TempoText = Tempo;
	if (UHorizontalBoxSlot* TempoSlot = TempoLine->AddChildToHorizontalBox(Tempo))
	{
		TempoSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
		TempoSlot->SetVerticalAlignment(VAlign_Center);
	}
	Block->AddChildToVerticalBox(TempoLine);

	FProgressBarStyle BarStyle;
	BarStyle.SetBackgroundImage(MakeBox(UITheme.RowBackground, CornerRadius, UITheme.Divider, 1.f));
	BarStyle.SetFillImage(MakeBox(UITheme.AccentColor, CornerRadius));
	BarStyle.SetMarqueeImage(MakeBox(UITheme.AccentColor, CornerRadius));
	const FText Names[MeterCount] = { LOCTEXT("MeterSub", "SUB"), LOCTEXT("MeterBass", "BASS"), LOCTEXT("MeterMid", "MID"), LOCTEXT("MeterHigh", "HIGH"), LOCTEXT("MeterLevel", "LEVEL") };
	for (int32 Index = 0; Index < MeterCount; ++Index)
	{
		UProgressBar* Bar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
		Bar->SetWidgetStyle(BarStyle);
		Bar->SetFillColorAndOpacity(FLinearColor::White);
		Bar->SetPercent(0.f);
		Meters[Index] = Bar;
		USizeBox* BarBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		BarBox->SetHeightOverride(10.f);
		BarBox->SetContent(Bar);
		UWidget* Row = MakeLabeledRow(Names[Index], BarBox);
		Row->SetToolTipText(LOCTEXT("MeterTip", "The band's level, scaled to the music's own recent loudness: what MODULATE effects read."));
		if (UVerticalBoxSlot* RowSlot = Block->AddChildToVerticalBox(Row))
		{
			RowSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
		}
	}
	return MakeLabeledRow(LOCTEXT("Music", "MUSIC"), Block);
}

UWidget* UStageAudioPanel::BuildFooter()
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UVerticalBox* Block = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UStageChoiceButton* Background = MakeButton(LOCTEXT("Background", "KEEP PLAYING IN THE BACKGROUND"),
		LOCTEXT("BackgroundTip", "Keep the music playing, and the lights following it, while another window has focus (a browser playing YouTube, say)."), 0);
	Background->OnPicked.BindUObject(this, &ThisClass::HandleBackgroundPicked);
	BackgroundButton = Background;
	if (UVerticalBoxSlot* ButtonSlot = Block->AddChildToVerticalBox(Background))
	{
		ButtonSlot->SetHorizontalAlignment(HAlign_Left);
	}
	UTextBlock* Detail = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextDisabled);
	Detail->SetAutoWrapText(true);
	DetailText = Detail;
	if (UVerticalBoxSlot* DetailSlot = Block->AddChildToVerticalBox(Detail))
	{
		DetailSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	return MakeLabeledRow(FText::GetEmpty(), Block);
}

// --- Lifetime ---

void UStageAudioPanel::NativeConstruct()
{
	Super::NativeConstruct();
	if (UWorld* World = GetWorld())
	{
		if (UStageAudioReactiveSubsystem* Subsystem = World->GetSubsystem<UStageAudioReactiveSubsystem>())
		{
			Audio = Subsystem;
			StatusHandle = Subsystem->OnAudioStatusChanged.AddUObject(this, &ThisClass::HandleStatusChanged);
		}
	}
	if (UStageThemedComboBox* Combo = DeviceCombo.Get())
	{
		Combo->OnSelectionChanged.AddUniqueDynamic(this, &ThisClass::HandleDeviceChanged);
	}
	RefreshDevices();
	Refresh();
}

void UStageAudioPanel::NativeDestruct()
{
	if (UStageAudioReactiveSubsystem* Subsystem = Audio.Get())
	{
		Subsystem->OnAudioStatusChanged.Remove(StatusHandle);
	}
	StatusHandle.Reset();
	Audio.Reset();
	if (UStageThemedComboBox* Combo = DeviceCombo.Get())
	{
		Combo->OnSelectionChanged.RemoveDynamic(this, &ThisClass::HandleDeviceChanged);
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimer);
		World->GetTimerManager().ClearTimer(MeterTimer);
	}
	Super::NativeDestruct();
}

// --- Refresh ---

void UStageAudioPanel::HandleStatusChanged()
{
	RequestRefresh();
}

void UStageAudioPanel::RequestRefresh()
{
	UWorld* World = GetWorld();
	if (!World || World->GetTimerManager().IsTimerPending(RefreshTimer))
	{
		return;
	}
	RefreshTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Refresh));
}

void UStageAudioPanel::RefreshDevices()
{
	UStageThemedComboBox* Combo = DeviceCombo.Get();
	if (!Combo)
	{
		return;
	}
	TGuardValue<bool> Filling(bFillingDevices, true);
	Combo->ClearOptions();
	DeviceIds.Reset();
	Combo->AddOption(LOCTEXT("DefaultDevice", "Windows default recording device").ToString());
	DeviceIds.Add(FString());
	for (const TPair<FString, FString>& Device : UStageAudioReactiveSubsystem::ListInputDevices())
	{
		Combo->AddOption(Device.Key);
		DeviceIds.Add(Device.Value);
	}
	const FString Chosen = GetAudio() ? GetAudio()->GetSettings().InputDeviceId : FString();
	Combo->SetSelectedIndex(FMath::Max(0, DeviceIds.IndexOfByKey(Chosen)));
}

void UStageAudioPanel::Refresh()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	UStageAudioReactiveSubsystem* Subsystem = GetAudio();
	if (!Subsystem || !StatusText)
	{
		UpdateMeterTimer(false);
		return;
	}
	Status = Subsystem->GetStatus();
	const bool bRunning = Status.State == EStageAudioSourceState::Running;
	const bool bFailed = Status.State == EStageAudioSourceState::Failed;
	LitSource = Status.State == EStageAudioSourceState::Off ? EStageAudioSource::Off : Status.Source;

	FText Line;
	switch (Status.State)
	{
	case EStageAudioSourceState::Off:
		Line = LOCTEXT("StateOff", "No music. Effects that follow it hold their pattern, or stay fully lit.");
		break;
	case EStageAudioSourceState::Starting:
		Line = FText::Format(LOCTEXT("StateStarting", "Opening {0}..."), FText::FromString(Status.Description));
		break;
	case EStageAudioSourceState::Running:
		Line = FText::FromString(Status.Description);
		break;
	case EStageAudioSourceState::Failed:
		Line = Status.Error;
		break;
	}
	StatusText->SetText(Line);
	StatusText->SetColorAndOpacity(FSlateColor(bFailed ? UITheme.WarningColor : UITheme.TextSecondary));

	for (int32 Index = 0; Index < SourceButtons.Num(); ++Index)
	{
		SetButtonLit(SourceButtons[Index].Get(), Index == static_cast<int32>(LitSource));
	}

	// The file: its transport and position.
	const bool bFile = Status.Source == EStageAudioSource::File && !bFailed && Status.State != EStageAudioSourceState::Off;
	if (UWidget* Row = FileRow.Get())
	{
		Row->SetVisibility(bFile ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (UTextBlock* Name = FileName.Get())
	{
		Name->SetText(FText::FromString(Status.Description));
	}
	const bool bPlaying = Status.bPlaying && !Status.bPaused;
	if (UStageChoiceButton* Play = PlayButton.Get())
	{
		SetButtonLit(Play, bPlaying);
		if (UTextBlock* Label = Cast<UTextBlock>(Play->GetChildAt(0)))
		{
			Label->SetText(bPlaying ? LOCTEXT("Pause", "PAUSE") : LOCTEXT("Play", "PLAY"));
		}
	}
	SetButtonLit(LoopButton.Get(), Status.bLoop);
	const double Duration = FMath::Max(Status.DurationSeconds, 0.0);
	const double Position = SeekPreviewSeconds.Get(Status.PositionSeconds);
	if (UStageValueSlider* Seek = SeekSlider.Get(); Seek && !Seek->HasMouseCapture())
	{
		Seek->SetValue(Duration > 0.0 ? static_cast<float>(FMath::Clamp(Position / Duration, 0.0, 1.0)) : 0.f);
	}
	if (UTextBlock* Time = PositionText.Get())
	{
		Time->SetText(FText::Format(LOCTEXT("Position", "{0} / {1}"), StageAudioPanel::FormatTime(Position), StageAudioPanel::FormatTime(Duration)));
	}

	// The input: its device and gain.
	if (UWidget* Row = InputRow.Get())
	{
		Row->SetVisibility(Status.Source == EStageAudioSource::Input && Status.State != EStageAudioSourceState::Off ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	const FStageAudioReactiveSettings Settings = Subsystem->GetSettings();
	if (UStageValueSlider* Gain = GainSlider.Get(); Gain && !Gain->HasMouseCapture())
	{
		Gain->SetValue((Settings.InputGainDb - StageAudioReactive::MinInputGainDb) / (StageAudioReactive::MaxInputGainDb - StageAudioReactive::MinInputGainDb));
	}
	if (UTextBlock* GainValue = GainText.Get())
	{
		GainValue->SetText(FText::Format(LOCTEXT("GainValue", "{0} dB"), FText::AsNumber(FMath::RoundToInt(Settings.InputGainDb))));
	}

	// Background playback lives with the other sound preferences (UStageAudioSubsystem).
	const UGameInstance* GameInstance = GetGameInstance();
	const UStageAudioSubsystem* Sound = GameInstance ? GameInstance->GetSubsystem<UStageAudioSubsystem>() : nullptr;
	SetButtonLit(BackgroundButton.Get(), Sound && Sound->IsPlayedInBackground());

	if (UTextBlock* Detail = DetailText.Get())
	{
		Detail->SetText(bRunning
			? FText::Format(LOCTEXT("Detail", "Analysis {0} % of a CPU core · {1} samples dropped · {2} underruns. Nothing is recorded."),
				FText::AsNumber(Status.AnalyzerCpuPercent, &StageAudioPanel::OneDecimal(false)),
				FText::AsNumber(Status.DroppedSamples), FText::AsNumber(Status.Underruns))
			: LOCTEXT("DetailIdle", "Music is analysed in memory only: nothing is recorded, saved or sent."));
	}
	UpdateMeterTimer(bRunning);
	UpdateMeters();
}

void UStageAudioPanel::UpdateMeterTimer(bool bRunning)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FTimerManager& Timers = World->GetTimerManager();
	if (bRunning && !Timers.IsTimerActive(MeterTimer))
	{
		Timers.SetTimer(MeterTimer, FTimerDelegate::CreateUObject(this, &ThisClass::UpdateMeters), MeterIntervalSeconds, /*bLoop*/ true);
	}
	else if (!bRunning)
	{
		Timers.ClearTimer(MeterTimer);
	}
}

void UStageAudioPanel::UpdateMeters()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	UStageAudioReactiveSubsystem* Subsystem = GetAudio();
	const FStageAudioFeatures Features = Subsystem ? Subsystem->GetFeatures() : FStageAudioFeatures();
	for (int32 Index = 0; Index < MeterCount; ++Index)
	{
		if (UProgressBar* Bar = Meters[Index].Get())
		{
			Bar->SetPercent(Features.bSignal ? FMath::Clamp(Features.GetBand(static_cast<EStageAudioBand>(Index)), 0.f, 1.f) : 0.f);
		}
	}
	const bool bTempo = Features.bSignal && Features.BPM > 0.f;
	if (UTextBlock* Tempo = TempoText.Get())
	{
		Tempo->SetText(!Features.bSignal ? LOCTEXT("NoSignal", "No signal")
			: !bTempo ? LOCTEXT("FindingTempo", "Finding the tempo...")
			: FText::Format(LOCTEXT("Tempo", "{0} BPM · {1} % sure"),
				FText::AsNumber(Features.BPM, &StageAudioPanel::OneDecimal(true)),
				FText::AsNumber(FMath::RoundToInt(Features.Confidence * 100.f))));
	}
	if (UBorder* Lamp = BeatLamp.Get())
	{
		const bool bOnBeat = bTempo && Features.PhaseAt(FPlatformTime::Seconds()) < StageAudioPanel::LampOnPhase;
		Lamp->SetBrush(MakeBox(bOnBeat ? UITheme.AccentColor : UITheme.RowBackground, StageAudioPanel::LampSize * 0.5f, UITheme.Divider, 1.f));
	}
}

// --- Input ---

void UStageAudioPanel::HandleSourcePicked(int32 SourceIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller || SourceIndex < 0 || SourceIndex > static_cast<int32>(EStageAudioSource::TestSignal))
	{
		return;
	}
	const EStageAudioSource Source = static_cast<EStageAudioSource>(SourceIndex);
	if (Source == EStageAudioSource::File)
	{
		// The song is chosen here, per session; it is never remembered (ADR 0029 §5).
		if (LastFileDirectory.IsEmpty())
		{
			LastFileDirectory = FPlatformProcess::UserDir();
		}
		TArray<FString> Files;
		if (StageFileDialog::OpenFiles(LOCTEXT("OpenSong", "Open a Song"), LastFileDirectory,
				TEXT("Audio files (*.wav;*.flac;*.mp3;*.m4a;*.aac;*.wma)|*.wav;*.wave;*.flac;*.mp3;*.m4a;*.aac;*.wma"), Files)
			&& !Files.IsEmpty())
		{
			LastFileDirectory = FPaths::GetPath(Files[0]);
			Controller->RequestOpenAudioFile(Files[0]);
		}
	}
	else
	{
		Controller->RequestSetAudioSource(Source);
	}
	RequestRefresh();
}

void UStageAudioPanel::HandleTransportPicked(int32 CommandIndex)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller)
	{
		return;
	}
	FStageAudioTransportRequest Request;
	switch (CommandIndex)
	{
	case StageAudioPanel::PlayPause:
		Request.Command = Status.bPlaying && !Status.bPaused ? EStageAudioTransportCommand::Pause : EStageAudioTransportCommand::Play;
		break;
	case StageAudioPanel::Stop:
		Request.Command = EStageAudioTransportCommand::Stop;
		break;
	case StageAudioPanel::Loop:
		Request.Command = EStageAudioTransportCommand::SetLoop;
		Request.bLoop = !Status.bLoop;
		break;
	default:
		return;
	}
	Controller->RequestAudioTransport(Request);
	RequestRefresh();
}

void UStageAudioPanel::HandleBackgroundPicked(int32 ChoiceIndex)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UStageAudioSubsystem* Sound = GameInstance ? GameInstance->GetSubsystem<UStageAudioSubsystem>() : nullptr;
	if (AModularPlayerController* Controller = GetStageController(); Controller && Sound)
	{
		Controller->RequestSetPlayAudioInBackground(!Sound->IsPlayedInBackground());
	}
	RequestRefresh();
}

void UStageAudioPanel::HandleSliderMoved(int32 SliderIndex, float Value)
{
	if (SliderIndex == StageAudioPanel::Seek)
	{
		// While dragging only the shown position moves; the release seeks once.
		SeekPreviewSeconds = FMath::Clamp(static_cast<double>(Value), 0.0, 1.0) * FMath::Max(Status.DurationSeconds, 0.0);
		if (UTextBlock* Time = PositionText.Get())
		{
			Time->SetText(FText::Format(LOCTEXT("Position", "{0} / {1}"), StageAudioPanel::FormatTime(SeekPreviewSeconds.GetValue()),
				StageAudioPanel::FormatTime(Status.DurationSeconds)));
		}
	}
	else if (SliderIndex == StageAudioPanel::Gain)
	{
		bGainDragged = true;
		if (UTextBlock* GainValue = GainText.Get())
		{
			const float Db = FMath::Lerp(StageAudioReactive::MinInputGainDb, StageAudioReactive::MaxInputGainDb, FMath::Clamp(Value, 0.f, 1.f));
			GainValue->SetText(FText::Format(LOCTEXT("GainValue", "{0} dB"), FText::AsNumber(FMath::RoundToInt(Db))));
		}
	}
}

void UStageAudioPanel::HandleSliderReleased()
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller)
	{
		return;
	}
	if (SeekPreviewSeconds.IsSet())
	{
		FStageAudioTransportRequest Request;
		Request.Command = EStageAudioTransportCommand::Seek;
		Request.Seconds = SeekPreviewSeconds.GetValue();
		SeekPreviewSeconds.Reset();
		Controller->RequestAudioTransport(Request);
	}
	const bool bGain = bGainDragged;
	bGainDragged = false;
	if (const UStageValueSlider* Gain = GainSlider.Get(); bGain && Gain && GetAudio())
	{
		FStageAudioReactiveSettings Settings = GetAudio()->GetSettings();
		const float Db = static_cast<float>(FMath::RoundToInt(FMath::Lerp(StageAudioReactive::MinInputGainDb, StageAudioReactive::MaxInputGainDb, Gain->GetValue())));
		if (Db != Settings.InputGainDb)
		{
			Settings.InputGainDb = Db;
			Controller->RequestApplyAudioSettings(Settings);
		}
	}
	RequestRefresh();
}

void UStageAudioPanel::HandleDeviceChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	AModularPlayerController* Controller = GetStageController();
	const UStageThemedComboBox* Combo = DeviceCombo.Get();
	if (bFillingDevices || !Controller || !Combo || !GetAudio() || SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	const int32 Index = Combo->GetSelectedIndex();
	if (!DeviceIds.IsValidIndex(Index))
	{
		return;
	}
	FStageAudioReactiveSettings Settings = GetAudio()->GetSettings();
	if (Settings.InputDeviceId != DeviceIds[Index])
	{
		Settings.InputDeviceId = DeviceIds[Index];
		Controller->RequestApplyAudioSettings(Settings);
	}
	RequestRefresh();
}

#undef LOCTEXT_NAMESPACE
