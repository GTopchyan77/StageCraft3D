// Copyright DamenTools internal. Sky & Weather Controls plugin.

#include "SkyWeatherControlsWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"
#include "UltraDynamicSkyBridge.h"

namespace SkyWeatherControls
{
	// This class is no longer instantiated (its dropdowns moved into WBP_SplineRouteHUD /
	// SplineRouteHUDWidget, which registers its own SkyWeather.TimeTransitionSeconds /
	// WeatherTransitionSeconds CVars) — kept as plain constants here, not CVars, so this
	// dead-but-not-yet-deleted file doesn't double-register the same console variable names.
	static constexpr float DefaultTimeTransitionSeconds = 2.0f;
	static constexpr float DefaultWeatherTransitionSeconds = 8.0f;

	struct FTimeOfDayOption
	{
		const TCHAR* Label;
		float TimeOfDay; // UDS military time x 100, i.e. 1830 == 6:30pm.
	};

	// Ordered through the day so the dropdown reads like a clock.
	static const FTimeOfDayOption TimeOfDayOptions[] =
	{
		{ TEXT("Dawn"),        500.0f  },
		{ TEXT("Sunrise"),     630.0f  },
		{ TEXT("Morning"),     900.0f  },
		{ TEXT("Midday"),      1200.0f },
		{ TEXT("Afternoon"),   1500.0f },
		{ TEXT("Golden Hour"), 1815.0f },
		{ TEXT("Sunset"),      1930.0f },
		{ TEXT("Dusk"),        2015.0f },
		{ TEXT("Night"),       2230.0f },
		{ TEXT("Midnight"),    0.0f    },
	};

	struct FWeatherOption
	{
		const TCHAR* Label;
		const TCHAR* PresetAsset; // Asset name under Blueprints/Weather_Effects/Weather_Presets.
	};

	// Ordered clear -> severe. Every entry maps to a preset that ships with UDS 8.
	static const FWeatherOption WeatherOptions[] =
	{
		{ TEXT("Clear Skies"),   TEXT("Clear_Skies")       },
		{ TEXT("Partly Cloudy"), TEXT("Partly_Cloudy")     },
		{ TEXT("Cloudy"),        TEXT("Cloudy")            },
		{ TEXT("Overcast"),      TEXT("Overcast")          },
		{ TEXT("Foggy"),         TEXT("Foggy")             },
		{ TEXT("Light Rain"),    TEXT("Rain_Light")        },
		{ TEXT("Rain"),          TEXT("Rain")              },
		{ TEXT("Thunderstorm"),  TEXT("Rain_Thunderstorm") },
		{ TEXT("Light Snow"),    TEXT("Snow_Light")        },
		{ TEXT("Snow"),          TEXT("Snow")              },
		{ TEXT("Blizzard"),      TEXT("Snow_Blizzard")     },
		{ TEXT("Dust"),          TEXT("Sand_Dust_Calm")    },
		{ TEXT("Dust Storm"),    TEXT("Sand_Dust_Storm")   },
	};

	static const FLinearColor PanelColour(0.015f, 0.025f, 0.04f, 0.82f);
	static const FLinearColor LabelColour(0.62f, 0.70f, 0.78f, 1.0f);
	static constexpr float DropdownWidth = 178.0f;

	/** Index of the option closest to a given Time of Day, wrapping across midnight. */
	static int32 NearestTimeOfDayOption(float TimeOfDay)
	{
		int32 Best = 0;
		float BestDelta = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(TimeOfDayOptions); ++Index)
		{
			const float Raw = FMath::Abs(TimeOfDayOptions[Index].TimeOfDay - TimeOfDay);
			const float Delta = FMath::Min(Raw, 2400.0f - Raw);
			if (Delta < BestDelta)
			{
				BestDelta = Delta;
				Best = Index;
			}
		}
		return Best;
	}
}

TSharedRef<SWidget> USkyWeatherControlsWidget::RebuildWidget()
{
	// AddToViewport gives this widget the whole screen. Only the panel itself may take
	// clicks; the transparent remainder has to stay click-through for the HUD beneath it.
	// Set here rather than only at the call site so a derived Widget Blueprint, which would
	// otherwise get the designer default of Visible, cannot reintroduce the blocker.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// A Widget Blueprint derived from this class supplies its own tree; only build ours
	// when nothing is there yet.
	if (WidgetTree != nullptr && WidgetTree->RootWidget == nullptr)
	{
		BuildControlPanel();
	}

	return Super::RebuildWidget();
}

void USkyWeatherControlsWidget::BuildControlPanel()
{
	using namespace SkyWeatherControls;

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("SkyWeatherRoot"));
	WidgetTree->RootWidget = Root;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("SkyWeatherPanel"));
	Panel->SetBrushColor(PanelColour);
	Panel->SetPadding(FMargin(18.0f, 14.0f));

	if (UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(Panel))
	{
		// Anchor and alignment share the same corner, so ScreenPadding always reads as an
		// inset regardless of which corner the panel is docked to.
		PanelSlot->SetAnchors(FAnchors(ScreenAnchor.X, ScreenAnchor.Y));
		PanelSlot->SetAlignment(ScreenAnchor);
		PanelSlot->SetAutoSize(true);
		PanelSlot->SetPosition(FVector2D(
			ScreenAnchor.X > 0.5f ? -ScreenPadding.X : ScreenPadding.X,
			ScreenAnchor.Y > 0.5f ? -ScreenPadding.Y : ScreenPadding.Y));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("SkyWeatherRow"));
	Panel->SetContent(Row);

	UWidget* TimeGroup = BuildLabelledDropdown(TEXT("TIME OF DAY"), TimeOfDayBox);
	if (UHorizontalBoxSlot* TimeSlot = Row->AddChildToHorizontalBox(TimeGroup))
	{
		TimeSlot->SetPadding(FMargin(0.0f, 0.0f, 14.0f, 0.0f));
	}

	Row->AddChildToHorizontalBox(BuildLabelledDropdown(TEXT("WEATHER"), WeatherBox));

	PopulateOptions();
}

UWidget* USkyWeatherControlsWidget::BuildLabelledDropdown(const TCHAR* Label, TObjectPtr<UComboBoxString>& OutComboBox)
{
	using namespace SkyWeatherControls;

	UVerticalBox* Group = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

	UTextBlock* Caption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Caption->SetText(FText::FromString(Label));
	Caption->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 9));
	Caption->SetColorAndOpacity(FSlateColor(LabelColour));
	if (UVerticalBoxSlot* CaptionSlot = Group->AddChildToVerticalBox(Caption))
	{
		CaptionSlot->SetPadding(FMargin(2.0f, 0.0f, 0.0f, 4.0f));
	}

	// UComboBoxString exposes no public font setter (InitFont is protected and direct
	// Font access is deprecated), so the box keeps the engine default type. Restyling
	// is done by deriving a Widget Blueprint from this class.
	OutComboBox = WidgetTree->ConstructWidget<UComboBoxString>(UComboBoxString::StaticClass());

	USizeBox* Sizer = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Sizer->SetWidthOverride(DropdownWidth);
	Sizer->AddChild(OutComboBox);
	Group->AddChildToVerticalBox(Sizer);

	return Group;
}

void USkyWeatherControlsWidget::PopulateOptions()
{
	using namespace SkyWeatherControls;

	// Seeding the boxes fires OnSelectionChanged, which we do not want to bounce back
	// into UDS -- opening the level should not restate the sky the level already has.
	TGuardValue<bool> SuppressGuard(bSuppressSelectionCallbacks, true);

	if (TimeOfDayBox != nullptr)
	{
		TimeOfDayBox->ClearOptions();
		for (const FTimeOfDayOption& Option : TimeOfDayOptions)
		{
			TimeOfDayBox->AddOption(Option.Label);
		}

		const float CurrentTime = UUltraDynamicSkyBridge::GetTimeOfDay(this);
		const int32 StartIndex = CurrentTime >= 0.0f ? NearestTimeOfDayOption(CurrentTime) : 0;
		TimeOfDayBox->SetSelectedOption(TimeOfDayOptions[StartIndex].Label);
		TimeOfDayBox->OnSelectionChanged.AddUniqueDynamic(this, &USkyWeatherControlsWidget::HandleTimeOfDayChanged);
	}

	if (WeatherBox != nullptr)
	{
		WeatherBox->ClearOptions();
		for (const FWeatherOption& Option : WeatherOptions)
		{
			WeatherBox->AddOption(Option.Label);
		}

		// UDW has no cheap "which preset am I on" query, so the box starts on the first
		// entry as a display default without pushing it to UDW.
		WeatherBox->SetSelectedOption(WeatherOptions[0].Label);
		WeatherBox->OnSelectionChanged.AddUniqueDynamic(this, &USkyWeatherControlsWidget::HandleWeatherChanged);
	}
}

void USkyWeatherControlsWidget::HandleTimeOfDayChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	using namespace SkyWeatherControls;

	if (bSuppressSelectionCallbacks)
	{
		return;
	}

	for (const FTimeOfDayOption& Option : TimeOfDayOptions)
	{
		if (SelectedItem.Equals(Option.Label))
		{
			UUltraDynamicSkyBridge::TransitionTimeOfDay(this, Option.TimeOfDay, DefaultTimeTransitionSeconds);
			return;
		}
	}
}

void USkyWeatherControlsWidget::HandleWeatherChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	using namespace SkyWeatherControls;

	if (bSuppressSelectionCallbacks)
	{
		return;
	}

	for (const FWeatherOption& Option : WeatherOptions)
	{
		if (SelectedItem.Equals(Option.Label))
		{
			UUltraDynamicSkyBridge::ChangeWeather(this, FName(Option.PresetAsset), DefaultWeatherTransitionSeconds);
			return;
		}
	}
}

void USkyWeatherControlsWidget::ApplyCurrentSelection()
{
	if (TimeOfDayBox != nullptr)
	{
		HandleTimeOfDayChanged(TimeOfDayBox->GetSelectedOption(), ESelectInfo::Direct);
	}
	if (WeatherBox != nullptr)
	{
		HandleWeatherChanged(WeatherBox->GetSelectedOption(), ESelectInfo::Direct);
	}
}
