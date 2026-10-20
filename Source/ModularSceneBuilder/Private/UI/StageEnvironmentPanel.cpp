// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageEnvironmentPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "UI/StageValueSlider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEnvironmentPanel)

#define LOCTEXT_NAMESPACE "StageEnvironmentPanel"

namespace StageEnvironmentPanel
{
	constexpr float SectionGap = 12.f;
	constexpr float ChoiceGap = 3.f;
	constexpr float RowGap = 6.f;
	constexpr float LabelWidth = 84.f;
	constexpr float ValueWidth = 48.f;

	/**
	 * Top of the Time of day slider, 23:59. Time and sun direction wrap (24:00 is 00:00, 360° is 0°), so a slider reaching the
	 * full period would read back as its own minimum and the handle would jump to the left end mid-drag.
	 */
	constexpr float LastMinuteOfDay = 24.f - 1.f / 60.f;

	/** The sliders, in panel order; each one's index in UStageEnvironmentPanel::Sliders. */
	enum class ESlider : int32
	{
		TimeOfDay,
		SunElevation,
		SunAzimuth,
		Clouds,
		Rain,
		Snow,
		Fog,
		Count
	};

	/** One slider's label, tip and range, indexed by ESlider. Ranges match StageEnvironment::Sanitize. */
	struct FSliderSpec
	{
		FText Label;
		FText Tip;
		float Min;
		float Max;
	};

	const FSliderSpec& GetSpec(ESlider Slider)
	{
		static const FSliderSpec Specs[] =
		{
			{ LOCTEXT("TimeOfDay", "Time of day"), LOCTEXT("TimeOfDayTip", "Local time. Sunrise is at 06:30 and sunset at 19:30; after dark the moon and stars take over."), 0.f, LastMinuteOfDay },
			{ LOCTEXT("SunHeight", "Sun height"), LOCTEXT("SunHeightTip", "How high the sun climbs at noon: low for winter light and long shadows, 90° straight overhead."), StageEnvironment::MinSunElevation, StageEnvironment::MaxSunElevation },
			{ LOCTEXT("SunDirection", "Sun direction"), LOCTEXT("SunDirectionTip", "Compass direction of the sun's path: turns the daylight and shadows around the stage."), 0.f, 359.f },
			{ LOCTEXT("Clouds", "Clouds"), LOCTEXT("CloudsTip", "Cloud cover, from a clear sky to overcast."), 0.f, 1.f },
			{ LOCTEXT("Rain", "Rain"), LOCTEXT("RainTip", "Rain intensity. Surfaces get wet with it."), 0.f, 1.f },
			{ LOCTEXT("Snow", "Snow"), LOCTEXT("SnowTip", "Snowfall intensity."), 0.f, 1.f },
			{ LOCTEXT("Fog", "Fog"), LOCTEXT("FogTip", "Atmospheric fog, from light haze to a few tens of metres of visibility."), 0.f, 1.f },
		};
		static_assert(UE_ARRAY_COUNT(Specs) == int32(ESlider::Count), "One spec per slider, in ESlider order.");
		return Specs[int32(Slider)];
	}

	float GetValue(const FStageEnvironmentSettings& Settings, ESlider Slider)
	{
		switch (Slider)
		{
		case ESlider::TimeOfDay:	return Settings.TimeOfDay;
		case ESlider::SunElevation:	return Settings.SunElevation;
		case ESlider::SunAzimuth:	return Settings.SunAzimuth;
		case ESlider::Clouds:		return Settings.CloudCoverage;
		case ESlider::Rain:			return Settings.Rain;
		case ESlider::Snow:			return Settings.Snow;
		case ESlider::Fog:			return Settings.Fog;
		default:					return 0.f;
		}
	}

	void SetValue(FStageEnvironmentSettings& Settings, ESlider Slider, float Value)
	{
		switch (Slider)
		{
		case ESlider::TimeOfDay:	Settings.TimeOfDay = Value; break;
		case ESlider::SunElevation:	Settings.SunElevation = Value; break;
		case ESlider::SunAzimuth:	Settings.SunAzimuth = Value; break;
		case ESlider::Clouds:		Settings.CloudCoverage = Value; break;
		case ESlider::Rain:			Settings.Rain = Value; break;
		case ESlider::Snow:			Settings.Snow = Value; break;
		case ESlider::Fog:			Settings.Fog = Value; break;
		default:					break;
		}
	}

	FText FormatValue(ESlider Slider, float Value)
	{
		switch (Slider)
		{
		case ESlider::TimeOfDay:
		{
			// Rounded to whole minutes first, so 23:59.6 shows 00:00 rather than 23:60.
			const int32 Minutes = FMath::RoundToInt(Value * 60.f) % (24 * 60);
			FNumberFormattingOptions TwoDigits;
			TwoDigits.SetMinimumIntegralDigits(2).SetUseGrouping(false);
			return FText::Format(LOCTEXT("Clock", "{0}:{1}"), FText::AsNumber(Minutes / 60, &TwoDigits), FText::AsNumber(Minutes % 60, &TwoDigits));
		}
		case ESlider::SunElevation:
		case ESlider::SunAzimuth:
			return FText::Format(LOCTEXT("Degrees", "{0}°"), FText::AsNumber(FMath::RoundToInt(Value)));
		default:
			return FText::AsPercent(Value);
		}
	}
}

const UStageCraftUITheme& UStageEnvironmentPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStageEnvironmentPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStageEnvironmentPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void UStageEnvironmentPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageEnvironmentPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildHeader());

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("HeaderDivider"));
	Divider->SetBrush(MakeLine(Palette.Divider, FVector2D(1.f, 1.f)));
	Column->AddChildToVerticalBox(Divider);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Body"));
	UBorder* BodyPadding = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BodyPadding"));
	BodyPadding->SetBrush(MakeBox(FLinearColor::Transparent));
	BodyPadding->SetPadding(FMargin(10.f, 8.f));
	BodyPadding->SetContent(Body);
	Scroll->AddChild(BodyPadding);

	const auto AddSection = [Body](UWidget* Section)
	{
		if (UVerticalBoxSlot* SectionSlot = Body->AddChildToVerticalBox(Section))
		{
			SectionSlot->SetPadding(FMargin(0.f, 0.f, 0.f, SectionGap));
		}
	};

	AddSection(BuildChoiceRow(LOCTEXT("OutdoorSky", "OUTDOOR SKY"),
		{ LOCTEXT("SkyOff", "Off"), LOCTEXT("SkyOn", "On") },
		{ LOCTEXT("SkyOffTip", "Indoor stage: the level's own lights, sky light and fog."),
		  LOCTEXT("SkyOnTip", "Outdoor stage: a real sky with sun, moon, clouds, rain, snow and fog. The level's own sun, sky light and fog are hidden meanwhile.") },
		SkyButtons, &ThisClass::HandleSkyPicked));

	NoticeText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	NoticeText->SetAutoWrapText(true);
	AddSection(NoticeText);

	AddSection(BuildChoiceRow(LOCTEXT("Weather", "WEATHER"),
		{ LOCTEXT("Clear", "Clear"), LOCTEXT("Cloudy", "Cloudy"), LOCTEXT("RainPreset", "Rain"), LOCTEXT("SnowPreset", "Snow"), LOCTEXT("FogPreset", "Fog") },
		{ LOCTEXT("ClearTip", "Clear sky, light haze."), LOCTEXT("CloudyTip", "Broken cloud, no precipitation."),
		  LOCTEXT("RainPresetTip", "Overcast with steady rain."), LOCTEXT("SnowPresetTip", "Overcast with snowfall."),
		  LOCTEXT("FogPresetTip", "Thick fog under a broken sky.") },
		WeatherButtons, &ThisClass::HandleWeatherPicked));

	Sliders.Reset(int32(ESlider::Count));
	ValueTexts.Reset(int32(ESlider::Count));
	AddSection(BuildSliderSection(LOCTEXT("Sun", "SUN"), int32(ESlider::TimeOfDay), 3));
	AddSection(BuildSliderSection(LOCTEXT("Conditions", "CONDITIONS"), int32(ESlider::Clouds), 4));
	check(Sliders.Num() == int32(ESlider::Count));
}

UWidget* UStageEnvironmentPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 8.f));
	UTextBlock* Title = MakeText(LOCTEXT("Title", "ENVIRONMENT"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Outdoor sky, time of day and weather for open-air stages."));
	Header->SetContent(Title);
	return Header;
}

UWidget* UStageEnvironmentPanel::BuildChoiceRow(const FText& Title, TConstArrayView<FText> Labels, TConstArrayView<FText> Tips,
	TArray<TObjectPtr<UStageChoiceButton>>& OutButtons, void (UStageEnvironmentPanel::*Handler)(int32))
{
	const UStageCraftUITheme& Palette = GetTheme();

	UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* TitleSlot = Section->AddChildToVerticalBox(MakeText(Title, 7, TEXT("Bold"), Palette.TextSecondary)))
	{
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Section->AddChildToVerticalBox(Row);

	OutButtons.Reset(Labels.Num());
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Button->SetChoiceIndex(Index);
		Button->OnPicked.BindUObject(this, Handler);
		if (Tips.IsValidIndex(Index))
		{
			Button->SetToolTipText(Tips[Index]);
		}
		if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Labels[Index], 8, TEXT("Bold"), Palette.TextSecondary))))
		{
			LabelSlot->SetPadding(FMargin(4.f, 4.f));
			LabelSlot->SetHorizontalAlignment(HAlign_Center);
		}
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(Index == 0 ? 0.f : StageEnvironmentPanel::ChoiceGap, 0.f, 0.f, 0.f));
		}
		OutButtons.Add(Button);
	}
	return Section;
}

UWidget* UStageEnvironmentPanel::BuildSliderSection(const FText& Title, int32 FirstSlider, int32 SliderCount)
{
	using namespace StageEnvironmentPanel;
	const UStageCraftUITheme& Palette = GetTheme();
	const FSliderStyle SliderStyle = StageCraftWidgetStyle::MakeSlider(Palette);

	UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* TitleSlot = Section->AddChildToVerticalBox(MakeText(Title, 7, TEXT("Bold"), Palette.TextSecondary)))
	{
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	}

	for (int32 Index = FirstSlider; Index < FirstSlider + SliderCount; ++Index)
	{
		const FSliderSpec& Spec = GetSpec(static_cast<ESlider>(Index));
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->SetToolTipText(Spec.Tip);

		// Label | slider | value: fixed label and value columns, so every slider starts and ends at the same X.
		USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		LabelBox->SetWidthOverride(LabelWidth);
		UTextBlock* Label = MakeText(Spec.Label, 8, TEXT("Regular"), Palette.TextPrimary);
		Label->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		LabelBox->SetContent(Label);
		if (UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelBox))
		{
			LabelSlot->SetVerticalAlignment(VAlign_Center);
		}

		UStageValueSlider* Slider = WidgetTree->ConstructWidget<UStageValueSlider>(UStageValueSlider::StaticClass());
		Slider->SetSliderIndex(Index);
		Slider->SetMinValue(Spec.Min);
		Slider->SetMaxValue(Spec.Max);
		Slider->SetWidgetStyle(SliderStyle);
		Slider->OnMoved.BindUObject(this, &ThisClass::HandleSliderMoved);
		if (UHorizontalBoxSlot* SliderSlot = Row->AddChildToHorizontalBox(Slider))
		{
			SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			SliderSlot->SetVerticalAlignment(VAlign_Center);
			SliderSlot->SetPadding(FMargin(4.f, 0.f));
		}
		Sliders.Add(Slider);

		USizeBox* ValueBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		ValueBox->SetWidthOverride(ValueWidth);
		UTextBlock* Value = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
		Value->SetJustification(ETextJustify::Right);
		ValueBox->SetContent(Value);
		if (UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(ValueBox))
		{
			ValueSlot->SetVerticalAlignment(VAlign_Center);
		}
		ValueTexts.Add(Value);

		USizeBox* RowHeight = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		RowHeight->SetHeightOverride(StageCraftWidgetStyle::FieldHeight);
		RowHeight->SetContent(Row);
		if (UVerticalBoxSlot* RowSlot = Section->AddChildToVerticalBox(RowHeight))
		{
			RowSlot->SetPadding(FMargin(0.f, RowGap * 0.5f));
		}
	}
	return Section;
}

void UStageEnvironmentPanel::NativeConstruct()
{
	Super::NativeConstruct();

	UStageEnvironmentSubsystem* Subsystem = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(GetWorld());
	Environment = Subsystem;
	if (Subsystem)
	{
		Subsystem->OnEnvironmentChanged.AddUniqueDynamic(this, &ThisClass::HandleEnvironmentChanged);
	}
	Refresh();
}

void UStageEnvironmentPanel::NativeDestruct()
{
	if (UStageEnvironmentSubsystem* Subsystem = Environment.Get())
	{
		Subsystem->OnEnvironmentChanged.RemoveDynamic(this, &ThisClass::HandleEnvironmentChanged);
	}
	Environment.Reset();
	Super::NativeDestruct();
}

void UStageEnvironmentPanel::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	// Selected: solid accent like a lit console key; the others outlined, so they still read as buttons.
	FButtonStyle Style = bSelected
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bSelected)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bSelected ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageEnvironmentPanel::Refresh()
{
	using namespace StageEnvironmentPanel;
	const UStageEnvironmentSubsystem* Subsystem = Environment.Get();
	const FStageEnvironmentSettings Settings = Subsystem ? Subsystem->GetSettings() : FStageEnvironmentSettings();
	const bool bAvailable = Subsystem && Subsystem->IsSkyAvailable();
	const bool bLive = bAvailable && Settings.bSkyEnabled;

	for (UStageChoiceButton* Button : SkyButtons)
	{
		if (Button)
		{
			StyleChoice(*Button, Button->GetChoiceIndex() == (Settings.bSkyEnabled ? 1 : 0));
			Button->SetIsEnabled(bAvailable);
		}
	}
	const EStageWeatherPreset Preset = StageEnvironment::FindWeatherPreset(Settings);
	for (UStageChoiceButton* Button : WeatherButtons)
	{
		if (Button)
		{
			StyleChoice(*Button, Button->GetChoiceIndex() == int32(Preset));
			Button->SetIsEnabled(bLive);
		}
	}
	for (int32 Index = 0; Index < Sliders.Num(); ++Index)
	{
		const float Value = GetValue(Settings, static_cast<ESlider>(Index));
		if (UStageValueSlider* Slider = Sliders[Index])
		{
			Slider->SetValue(Value);
			Slider->SetIsEnabled(bLive);
		}
		if (ValueTexts.IsValidIndex(Index) && ValueTexts[Index])
		{
			ValueTexts[Index]->SetText(FormatValue(static_cast<ESlider>(Index), Value));
		}
	}

	if (NoticeText)
	{
		const UStageCraftUITheme& Palette = GetTheme();
		NoticeText->SetText(!Subsystem ? LOCTEXT("NoStage", "The environment is available while a stage is open.")
			: !bAvailable ? LOCTEXT("NoBackend", "No sky system is installed, so the outdoor sky is unavailable.")
			: bLive ? FText::Format(LOCTEXT("SkyOnNotice", "{0} sky. The level's own sun, sky light and fog are hidden while it is on."), Subsystem->GetSkySystemName())
			: LOCTEXT("SkyOffNotice", "Indoor stage: the level's own lighting. Turn the outdoor sky on to set time and weather."));
		NoticeText->SetColorAndOpacity(FSlateColor(bAvailable || !Subsystem ? Palette.TextSecondary : Palette.WarningColor));
	}
}

void UStageEnvironmentPanel::HandleSkyPicked(int32 Index)
{
	// The single write path; the panel updates from OnEnvironmentChanged, not from what it asked for.
	if (UStageEnvironmentSubsystem* Subsystem = Environment.Get())
	{
		FStageEnvironmentSettings Settings = Subsystem->GetSettings();
		Settings.bSkyEnabled = Index == 1;
		Subsystem->SetSettings(Settings);
	}
}

void UStageEnvironmentPanel::HandleWeatherPicked(int32 Index)
{
	if (UStageEnvironmentSubsystem* Subsystem = Environment.Get())
	{
		Subsystem->ApplyWeatherPreset(static_cast<EStageWeatherPreset>(Index));
	}
}

void UStageEnvironmentPanel::HandleSliderMoved(int32 SliderIndex, float Value)
{
	if (UStageEnvironmentSubsystem* Subsystem = Environment.Get())
	{
		FStageEnvironmentSettings Settings = Subsystem->GetSettings();
		StageEnvironmentPanel::SetValue(Settings, static_cast<StageEnvironmentPanel::ESlider>(SliderIndex), Value);
		Subsystem->SetSettings(Settings);
	}
}

void UStageEnvironmentPanel::HandleEnvironmentChanged(const FStageEnvironmentSettings& Settings)
{
	Refresh();
}

#undef LOCTEXT_NAMESPACE
