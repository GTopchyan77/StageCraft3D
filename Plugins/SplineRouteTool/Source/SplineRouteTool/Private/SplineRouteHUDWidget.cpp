// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteHUDWidget.h"
#include "SplineRouteTimeSubsystem.h"
#include "SplineRouteAuthoringSubsystem.h"
#include "SplineRouteActor.h"
#include "VesselCatalog.h"
#include "SplineRouteTool.h"

#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/ComboBoxString.h"
#include "Components/Button.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "HAL/IConsoleManager.h"
#include "UltraDynamicSkyBridge.h"
#include "NGGVideoRecorderSubsystem.h"

namespace
{
	USplineRouteAuthoringSubsystem* GetAuthoring(UWorld* World)
	{
		return World ? World->GetSubsystem<USplineRouteAuthoringSubsystem>() : nullptr;
	}

	UNGGVideoRecorderSubsystem* GetRecorder(UWorld* World)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<UNGGVideoRecorderSubsystem>() : nullptr;
	}

	// Moved verbatim from the standalone SkyWeatherControls plugin's own widget
	// (USkyWeatherControlsWidget), which no longer builds its own panel — these option
	// tables and the CVars below are the same ones that drove its dropdowns.
	TAutoConsoleVariable<float> CVarTimeTransitionSeconds(
		TEXT("SkyWeather.TimeTransitionSeconds"),
		2.0f,
		TEXT("Seconds Ultra Dynamic Sky takes to blend to a newly picked Time of Day. 0 snaps."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarWeatherTransitionSeconds(
		TEXT("SkyWeather.WeatherTransitionSeconds"),
		8.0f,
		TEXT("Seconds Ultra Dynamic Weather takes to blend to a newly picked weather preset. 0 snaps."),
		ECVF_Default);

	struct FTimeOfDayOption
	{
		const TCHAR* Label;
		float TimeOfDay; // UDS military time x 100, i.e. 1830 == 6:30pm.
	};

	// Ordered through the day so the dropdown reads like a clock.
	const FTimeOfDayOption TimeOfDayOptions[] =
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
	const FWeatherOption WeatherOptions[] =
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

	/** Index of the option closest to a given Time of Day, wrapping across midnight. */
	int32 NearestTimeOfDayOption(float TimeOfDay)
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

void USplineRouteHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	USplineRouteTimeSubsystem* TimeSys = nullptr;
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			TimeSys = GI->GetSubsystem<USplineRouteTimeSubsystem>();
		}
	}

	if (TimeSlider)
	{
		const float MaxValue = TimeSys ? TimeSys->MaxTimeScale : 5.0f;
		const float Current = TimeSys ? TimeSys->GetTimeScale() : 1.0f;
		TimeSlider->SetMinValue(0.0f);
		TimeSlider->SetMaxValue(MaxValue);
		TimeSlider->SetValue(Current);
		TimeSlider->OnValueChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleSliderChanged);
	}

	if (TimeSys)
	{
		TimeSys->OnTimeScaleChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleTimeScaleChanged);
		RefreshLabel(TimeSys->GetTimeScale());
	}
	else
	{
		RefreshLabel(1.0f);
	}

	// Stretch: vessel picker + segment speed UI.
	PopulateVesselPicker();
	if (VesselPicker)
	{
		VesselPicker->OnSelectionChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleVesselPicked);
	}

	// Time of Day / Weather — populate + seed BEFORE binding, same as VesselPicker above, so
	// seeding the initial selection never bounces back into UDS (no listener is attached yet).
	PopulateSkyWeatherControls();
	if (TimeOfDayPicker)
	{
		TimeOfDayPicker->OnSelectionChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleTimeOfDayChanged);
	}
	if (WeatherPicker)
	{
		WeatherPicker->OnSelectionChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleWeatherChanged);
	}
	if (SpawnVesselButton)
	{
		SpawnVesselButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleSpawnVesselClicked);
	}
	if (CyclePrevButton)
	{
		CyclePrevButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleCyclePrevClicked);
	}
	if (CycleNextButton)
	{
		CycleNextButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleCycleNextClicked);
	}
	if (SegmentSpeedSlider)
	{
		SegmentSpeedSlider->SetMinValue(MinSegmentSpeed);
		SegmentSpeedSlider->SetMaxValue(MaxSegmentSpeed);
		SegmentSpeedSlider->SetValue(DefaultSegmentSpeedDisplay);
		SegmentSpeedSlider->OnValueChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleSegmentSpeedChanged);
	}

	if (HandleSizeSlider)
	{
		HandleSizeSlider->SetMinValue(MinHandleSize);
		HandleSizeSlider->SetMaxValue(MaxHandleSize);
		HandleSizeSlider->SetValue(DefaultHandleSize);
		HandleSizeSlider->OnValueChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleHandleSizeChanged);
	}
	RefreshHandleSizeLabel(DefaultHandleSize);

	if (VesselSizeSlider)
	{
		VesselSizeSlider->SetMinValue(MinVesselSize);
		VesselSizeSlider->SetMaxValue(MaxVesselSize);
		VesselSizeSlider->SetValue(DefaultVesselSize);
		VesselSizeSlider->OnValueChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleVesselSizeChanged);
	}
	RefreshVesselSizeLabel(DefaultVesselSize);

	if (MinSpawnGapSlider)
	{
		MinSpawnGapSlider->SetMinValue(1000.0f);
		MinSpawnGapSlider->SetMaxValue(20000.0f);
		MinSpawnGapSlider->SetValue(5000.0f);
		MinSpawnGapSlider->OnValueChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleMinSpawnGapChanged);
	}
	RefreshMinSpawnGapLabel(5000.0f);

	if (TurnRateSlider)
	{
		TurnRateSlider->SetMinValue(MinTurnRate);
		TurnRateSlider->SetMaxValue(MaxTurnRate);
		TurnRateSlider->SetValue(DefaultTurnRate);
		TurnRateSlider->OnValueChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleTurnRateChanged);
	}
	RefreshTurnRateLabel(DefaultTurnRate);

	if (ReverseToggleButton)
	{
		ReverseToggleButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleReverseToggleClicked);
	}

	if (PlayButton)            { PlayButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandlePlayClicked); }
	if (PauseButton)           { PauseButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandlePauseClicked); }
	if (StopButton)            { StopButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleStopClicked); }
	if (SaveButton)            { SaveButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleSaveClicked); }
	if (LoadButton)            { LoadButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleLoadClicked); }
	if (DeleteAllVesselsButton)   { DeleteAllVesselsButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleDeleteAllVesselsClicked); }
	if (DeleteAllWaypointsButton) { DeleteAllWaypointsButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleDeleteAllWaypointsClicked); }
	if (RouteTypeButton)       { RouteTypeButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleRouteTypeClicked); }
	if (ObstructionModeButton) { ObstructionModeButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleObstructionModeClicked); }
	if (ObsPrevButton)         { ObsPrevButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleObsPrevClicked); }
	if (ObsNextButton)         { ObsNextButton->OnClicked.AddDynamic(this, &USplineRouteHUDWidget::HandleObsNextClicked); }

	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->OnActiveRouteChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleActiveRouteChanged);
		// Adopt any level-placed route immediately so the slider/preview-ghost work before the first place-click.
		ASplineRouteActor* InitialRoute = Auth->GetActiveRoute();
		if (!InitialRoute)
		{
			InitialRoute = Auth->EnsureActiveRoute();
			UE_LOG(LogSplineRoute, Log,
				TEXT("[SplineRoute] HUD construct: adopted route %s via EnsureActiveRoute"),
				*GetNameSafe(InitialRoute));
		}
		BindRouteSelection(InitialRoute);
		// Push the slider's starting value to the adopted route so its handles match the HUD on first frame.
		if (InitialRoute && HandleSizeSlider)
		{
			InitialRoute->SetHandleSize(HandleSizeSlider->GetValue());
		}
		// Tell the subsystem about the inner Panel widget — NOT `this`, because the root CanvasPanel fills the viewport.
		// Falls back to `this` only if no inner Panel is bound (degraded but at least logs the registration).
		Auth->RegisterHUDPanel(Panel ? static_cast<UWidget*>(Panel) : static_cast<UWidget*>(this));
	}

	if (UNGGVideoRecorderSubsystem* Recorder = GetRecorder(GetWorld()))
	{
		Recorder->OnRecordingWillStart.AddDynamic(this, &USplineRouteHUDWidget::HandleRecordingWillStart);
		Recorder->OnRecordingFinished.AddDynamic(this, &USplineRouteHUDWidget::HandleRecordingFinished);
	}

	// One-shot summary so we can confirm at PIE start that the widget actually saw the catalog and bound to the subsystem.
	{
		USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
		UVesselCatalog* Cat = Auth ? Auth->GetOrLoadCatalog() : nullptr;
		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] HUD constructed: catalog=%s entries=%d activeRoute=%s segSliderMax=%.0f"),
			*GetNameSafe(Cat),
			Cat ? Cat->Entries.Num() : 0,
			Auth ? *GetNameSafe(Auth->GetActiveRoute()) : TEXT("(no subsystem)"),
			MaxSegmentSpeed);
	}

	// Initial slider ranges from the catalog's global slider-range fields (bResetToDefault=true so
	// first open lands on the catalog's defaults, not whatever the WBP slider had baked in).
	ApplyCatalogSliderRanges(/*bResetToDefault=*/true);

	RefreshSegmentSpeedUI();
	RefreshWaypointLabel();
	RefreshReverseUI();
	RefreshControlBar();
}

void USplineRouteHUDWidget::NativeDestruct()
{
	if (TimeSlider)
	{
		TimeSlider->OnValueChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleSliderChanged);
	}

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (USplineRouteTimeSubsystem* TimeSys = GI->GetSubsystem<USplineRouteTimeSubsystem>())
			{
				TimeSys->OnTimeScaleChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleTimeScaleChanged);
			}
		}
		if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(World))
		{
			Auth->OnActiveRouteChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleActiveRouteChanged);
		}
		if (UNGGVideoRecorderSubsystem* Recorder = GetRecorder(World))
		{
			Recorder->OnRecordingWillStart.RemoveDynamic(this, &USplineRouteHUDWidget::HandleRecordingWillStart);
			Recorder->OnRecordingFinished.RemoveDynamic(this, &USplineRouteHUDWidget::HandleRecordingFinished);
		}
	}

	UnbindRouteSelection(BoundRoute);

	if (VesselPicker)
	{
		VesselPicker->OnSelectionChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleVesselPicked);
	}
	if (TimeOfDayPicker)
	{
		TimeOfDayPicker->OnSelectionChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleTimeOfDayChanged);
	}
	if (WeatherPicker)
	{
		WeatherPicker->OnSelectionChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleWeatherChanged);
	}
	if (SpawnVesselButton)
	{
		SpawnVesselButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleSpawnVesselClicked);
	}
	if (CyclePrevButton)
	{
		CyclePrevButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleCyclePrevClicked);
	}
	if (CycleNextButton)
	{
		CycleNextButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleCycleNextClicked);
	}
	if (SegmentSpeedSlider)
	{
		SegmentSpeedSlider->OnValueChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleSegmentSpeedChanged);
	}
	if (HandleSizeSlider)
	{
		HandleSizeSlider->OnValueChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleHandleSizeChanged);
	}
	if (VesselSizeSlider)
	{
		VesselSizeSlider->OnValueChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleVesselSizeChanged);
	}
	if (MinSpawnGapSlider)
	{
		MinSpawnGapSlider->OnValueChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleMinSpawnGapChanged);
	}
	if (TurnRateSlider)
	{
		TurnRateSlider->OnValueChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleTurnRateChanged);
	}
	if (ReverseToggleButton)
	{
		ReverseToggleButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleReverseToggleClicked);
	}
	if (PlayButton)            { PlayButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandlePlayClicked); }
	if (PauseButton)           { PauseButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandlePauseClicked); }
	if (StopButton)            { StopButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleStopClicked); }
	if (SaveButton)            { SaveButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleSaveClicked); }
	if (LoadButton)            { LoadButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleLoadClicked); }
	if (DeleteAllVesselsButton)   { DeleteAllVesselsButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleDeleteAllVesselsClicked); }
	if (DeleteAllWaypointsButton) { DeleteAllWaypointsButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleDeleteAllWaypointsClicked); }
	if (RouteTypeButton)       { RouteTypeButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleRouteTypeClicked); }
	if (ObstructionModeButton) { ObstructionModeButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleObstructionModeClicked); }
	if (ObsPrevButton)         { ObsPrevButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleObsPrevClicked); }
	if (ObsNextButton)         { ObsNextButton->OnClicked.RemoveDynamic(this, &USplineRouteHUDWidget::HandleObsNextClicked); }

	Super::NativeDestruct();
}

void USplineRouteHUDWidget::HandleSliderChanged(float NewValue)
{
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (USplineRouteTimeSubsystem* TimeSys = GI->GetSubsystem<USplineRouteTimeSubsystem>())
			{
				TimeSys->SetTimeScale(NewValue);
			}
		}
	}
	RefreshLabel(NewValue);
}

void USplineRouteHUDWidget::HandleTimeScaleChanged(float NewScale)
{
	if (TimeSlider && !FMath::IsNearlyEqual(TimeSlider->GetValue(), NewScale))
	{
		TimeSlider->SetValue(NewScale);
	}
	RefreshLabel(NewScale);
}

void USplineRouteHUDWidget::RefreshLabel(float Scale) const
{
	if (TimeScaleLabel)
	{
		TimeScaleLabel->SetText(FText::FromString(FString::Printf(TEXT("Time Scale: %.2fx"), Scale)));
	}
}

void USplineRouteHUDWidget::PopulateVesselPicker()
{
	if (!VesselPicker)
	{
		return;
	}
	VesselPicker->ClearOptions();

	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	UVesselCatalog* Cat = Auth ? Auth->GetOrLoadCatalog() : nullptr;
	if (!Cat)
	{
		return;
	}
	for (int32 i = 0; i < Cat->Entries.Num(); ++i)
	{
		const FText& Name = Cat->Entries[i].DisplayName;
		const FString Label = Name.IsEmpty() ? FString::Printf(TEXT("Vessel %d"), i) : Name.ToString();
		VesselPicker->AddOption(Label);
	}
	if (Cat->Entries.Num() > 0)
	{
		VesselPicker->SetSelectedIndex(FMath::Clamp(Auth->SelectedVesselIndex, 0, Cat->Entries.Num() - 1));
	}
}

void USplineRouteHUDWidget::HandleVesselPicked(FString SelectedItem, ESelectInfo::Type /*SelectionType*/)
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	if (!Auth || !VesselPicker)
	{
		return;
	}
	Auth->SelectedVesselIndex = VesselPicker->GetSelectedIndex();
}

void USplineRouteHUDWidget::PopulateSkyWeatherControls()
{
	if (TimeOfDayPicker)
	{
		TimeOfDayPicker->ClearOptions();
		for (const FTimeOfDayOption& Option : TimeOfDayOptions)
		{
			TimeOfDayPicker->AddOption(Option.Label);
		}

		const float CurrentTime = UUltraDynamicSkyBridge::GetTimeOfDay(this);
		const int32 StartIndex = CurrentTime >= 0.0f ? NearestTimeOfDayOption(CurrentTime) : 0;
		TimeOfDayPicker->SetSelectedOption(TimeOfDayOptions[StartIndex].Label);
	}

	if (WeatherPicker)
	{
		WeatherPicker->ClearOptions();
		for (const FWeatherOption& Option : WeatherOptions)
		{
			WeatherPicker->AddOption(Option.Label);
		}

		// UDW has no cheap "which preset am I on" query, so the box starts on the first
		// entry as a display default without pushing it to UDW.
		WeatherPicker->SetSelectedOption(WeatherOptions[0].Label);
	}
}

void USplineRouteHUDWidget::HandleTimeOfDayChanged(FString SelectedItem, ESelectInfo::Type /*SelectionType*/)
{
	for (const FTimeOfDayOption& Option : TimeOfDayOptions)
	{
		if (SelectedItem.Equals(Option.Label))
		{
			UUltraDynamicSkyBridge::TransitionTimeOfDay(this, Option.TimeOfDay, CVarTimeTransitionSeconds.GetValueOnGameThread());
			return;
		}
	}
}

void USplineRouteHUDWidget::HandleWeatherChanged(FString SelectedItem, ESelectInfo::Type /*SelectionType*/)
{
	for (const FWeatherOption& Option : WeatherOptions)
	{
		if (SelectedItem.Equals(Option.Label))
		{
			UUltraDynamicSkyBridge::ChangeWeather(this, FName(Option.PresetAsset), CVarWeatherTransitionSeconds.GetValueOnGameThread());
			return;
		}
	}
}

void USplineRouteHUDWidget::HandleSpawnVesselClicked()
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	if (!Auth)
	{
		return;
	}
	const int32 Idx = VesselPicker ? VesselPicker->GetSelectedIndex() : Auth->SelectedVesselIndex;
	AActor* Spawned = Auth->SpawnVesselOnActiveRoute(Idx);
	if (!Spawned)
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] SpawnVesselButton: Wait for vessel to clear gap"));
		return;
	}

	if (MinSpawnGapSlider && MinSpawnGapLabel && Auth)
	{
		MinSpawnGapSlider->SetValue(Auth->MinSpawnGapMeters);
		MinSpawnGapLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Min Gap: %.0fm (auto)"), Auth->MinSpawnGapMeters)));
	}

	// Briefly disable the button so a rapid double-click can't queue a second attempt while the
	// gate is still open from this spawn — purely a UX debounce, the subsystem gate is authoritative.
	if (SpawnVesselButton)
	{
		SpawnVesselButton->SetIsEnabled(false);
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(SpawnButtonReenableTimer, this, &USplineRouteHUDWidget::ReenableSpawnButton, 0.5f, false);
		}
	}
}

void USplineRouteHUDWidget::ReenableSpawnButton()
{
	if (SpawnVesselButton)
	{
		SpawnVesselButton->SetIsEnabled(true);
	}
}

void USplineRouteHUDWidget::HandleSegmentSpeedChanged(float NewValue)
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	if (!Auth)
	{
		return;
	}
	Auth->SetSpeedForSelectedSegment(NewValue);
	RefreshSegmentSpeedUI();
}

void USplineRouteHUDWidget::HandleHandleSizeChanged(float NewValue)
{
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] HUD HandleSizeSlider -> %.2f"), NewValue);
	ApplyHandleSizeToActiveRoute(NewValue);
	RefreshHandleSizeLabel(NewValue);
}

void USplineRouteHUDWidget::HandleActiveRouteChanged(ASplineRouteActor* NewRoute)
{
	BindRouteSelection(NewRoute);
	RefreshSegmentSpeedUI();
	RefreshWaypointLabel();
	RefreshReverseUI();
	RefreshControlBar();
	// Newly-bound routes adopt whatever the slider is currently showing — keeps placement size consistent.
	if (HandleSizeSlider && NewRoute)
	{
		NewRoute->SetHandleSize(HandleSizeSlider->GetValue());
	}
}

void USplineRouteHUDWidget::RefreshHandleSizeLabel(float Size) const
{
	if (HandleSizeLabel)
	{
		HandleSizeLabel->SetText(FText::FromString(FString::Printf(TEXT("Waypoint Size: %.0f"), Size)));
	}
}

void USplineRouteHUDWidget::HandleVesselSizeChanged(float NewValue)
{
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] HUD VesselSizeSlider -> %.3f"), NewValue);
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	if (Auth)
	{
		Auth->SetVesselSizeMultiplier(NewValue);
	}
	RefreshVesselSizeLabel(NewValue);

	// Reflect the mesh-based gap SetVesselSizeMultiplier just computed from the largest live vessel.
	if (Auth && MinSpawnGapSlider && MinSpawnGapLabel)
	{
		const float RecommendedGap = Auth->MinSpawnGapMeters;
		MinSpawnGapSlider->SetValue(RecommendedGap);
		MinSpawnGapLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Min Gap: %.0fm (auto)"), RecommendedGap)));
	}
}

void USplineRouteHUDWidget::RefreshVesselSizeLabel(float Size) const
{
	if (VesselSizeLabel)
	{
		VesselSizeLabel->SetText(FText::FromString(FString::Printf(TEXT("Ship Size: %.2fx"), Size)));
	}
}

void USplineRouteHUDWidget::HandleMinSpawnGapChanged(float NewValue)
{
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] HUD MinSpawnGapSlider -> %.1f"), NewValue);
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->SetMinSpawnGap(NewValue);
	}
	RefreshMinSpawnGapLabel(NewValue);
}

void USplineRouteHUDWidget::RefreshMinSpawnGapLabel(float Value) const
{
	if (MinSpawnGapLabel)
	{
		MinSpawnGapLabel->SetText(FText::FromString(FString::Printf(TEXT("Min Gap: %.0fm"), Value)));
	}
}

void USplineRouteHUDWidget::HandleTurnRateChanged(float NewValue)
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->SetTurnRateForAllVessels(NewValue);
	}
	RefreshTurnRateLabel(NewValue);
}

void USplineRouteHUDWidget::RefreshTurnRateLabel(float Rate) const
{
	if (TurnRateLabel)
	{
		if (Rate <= 0.0f)
		{
			TurnRateLabel->SetText(FText::FromString(TEXT("Turn Rate: instant (snap)")));
		}
		else
		{
			TurnRateLabel->SetText(FText::FromString(FString::Printf(TEXT("Turn Rate: %.0f deg/s"), Rate)));
		}
	}
}

void USplineRouteHUDWidget::HandleReverseToggleClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->ToggleReverseForSelectedPoint();
	}
	RefreshReverseUI();
}

void USplineRouteHUDWidget::HandlePlayClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->PlayAll(); }
}

void USplineRouteHUDWidget::HandlePauseClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->PauseAll(); }
}

void USplineRouteHUDWidget::HandleStopClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->StopAll(); }
}

void USplineRouteHUDWidget::HandleSaveClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->SaveScenario(Auth->ScenarioSlotName); }
}

void USplineRouteHUDWidget::HandleLoadClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->LoadScenario(Auth->ScenarioSlotName);
		// Rebuild HUD readouts against the freshly-loaded world state.
		RefreshSegmentSpeedUI();
		RefreshWaypointLabel();
		RefreshReverseUI();
		RefreshControlBar();
	}
}

void USplineRouteHUDWidget::HandleDeleteAllVesselsClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->ClearAllVessels(); }
}

void USplineRouteHUDWidget::HandleDeleteAllWaypointsClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->ClearAllWaypoints(); }
	// Waypoint selection/labels are now stale on every route — refresh HUD readouts that depend on it.
	RefreshWaypointLabel();
	RefreshReverseUI();
	RefreshSegmentSpeedUI();
}

void USplineRouteHUDWidget::HandleRouteTypeClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->ToggleActiveRouteVehicleType(); }
	RefreshControlBar();
}

void USplineRouteHUDWidget::HandleObstructionModeClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld())) { Auth->SetObstructionPlaceMode(!Auth->IsObstructionPlaceMode()); }
	RefreshControlBar();
}

void USplineRouteHUDWidget::HandleObsPrevClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		const int32 N = Auth->GetNumObstructionTypes();
		Auth->SetSelectedObstructionIndex(((Auth->SelectedObstructionIndex - 1) % N + N) % N);
	}
	RefreshControlBar();
}

void USplineRouteHUDWidget::HandleObsNextClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		const int32 N = Auth->GetNumObstructionTypes();
		Auth->SetSelectedObstructionIndex((Auth->SelectedObstructionIndex + 1) % N);
	}
	RefreshControlBar();
}

void USplineRouteHUDWidget::RefreshControlBar()
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	if (!Auth)
	{
		return;
	}
	if (RouteTypeLabel)
	{
		const bool bGround = Auth->GetActiveRouteVehicleType() == ERouteVehicleType::Ground;
		RouteTypeLabel->SetText(FText::FromString(bGround ? TEXT("Route: LAND (ground)") : TEXT("Route: WATER (vessel)")));
	}
	if (ObstructionLabel)
	{
		const TCHAR* Mode = Auth->IsObstructionPlaceMode() ? TEXT("ON") : TEXT("off");
		const FText Type = Auth->GetObstructionLabel(Auth->SelectedObstructionIndex);
		ObstructionLabel->SetText(FText::FromString(FString::Printf(TEXT("Obstruction [%s]: %s"), Mode, *Type.ToString())));
	}
}

void USplineRouteHUDWidget::RefreshReverseUI()
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	ASplineRouteActor* Route = Auth ? Auth->GetActiveRoute() : nullptr;
	const int32 Selected = Route ? Route->GetSelectedControlPoint() : INDEX_NONE;
	const bool bHasSelection = (Route != nullptr) && (Selected != INDEX_NONE);
	const bool bReverse = bHasSelection && Route->GetPointReverse(Selected);

	if (ReverseToggleButton)
	{
		ReverseToggleButton->SetIsEnabled(bHasSelection);
	}
	if (ReverseLabel)
	{
		if (!bHasSelection)
		{
			ReverseLabel->SetText(FText::FromString(TEXT("Reverse node (select a waypoint)")));
		}
		else
		{
			ReverseLabel->SetText(FText::FromString(
				bReverse ? TEXT("Reverse node: ON (drives stern-first)") : TEXT("Reverse node: off")));
		}
	}
}

void USplineRouteHUDWidget::ApplyHandleSizeToActiveRoute(float NewSize)
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	if (!Auth)
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRoute] ApplyHandleSize: no authoring subsystem (size=%.2f dropped)"), NewSize);
		return;
	}
	// EnsureActiveRoute claims a level-placed route (or spawns one) so the slider drives something on the first drag,
	// even when the user hasn't clicked to place a waypoint yet.
	ASplineRouteActor* Route = Auth->EnsureActiveRoute();
	if (Route)
	{
		UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] ApplyHandleSize: forwarding %.2f to route %s"), NewSize, *Route->GetName());
		Route->SetHandleSize(NewSize);
	}
	else
	{
		UE_LOG(LogSplineRoute, Warning,
			TEXT("[SplineRoute] ApplyHandleSize: EnsureActiveRoute returned null (size=%.2f dropped). Check DefaultRouteClass."),
			NewSize);
	}
}

void USplineRouteHUDWidget::HandleSelectedControlPointChanged(int32 /*NewIndex*/)
{
	RefreshSegmentSpeedUI();
	RefreshWaypointLabel();
	RefreshReverseUI();
}

void USplineRouteHUDWidget::HandleCyclePrevClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->SelectPrevPoint();
	}
}

void USplineRouteHUDWidget::HandleCycleNextClicked()
{
	if (USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld()))
	{
		Auth->SelectNextPoint();
	}
}

void USplineRouteHUDWidget::HandleRecordingWillStart(const FString& FilePath)
{
	// Collapse the whole widget — every direct child of the root canvas (Row1, Row2, Panel and everything
	// under it), no named exceptions. The only thing that stays visible while recording is
	// NGGVideoRecorder's own Record/Stop panel, which lives entirely outside this widget tree (a separate
	// Slate overlay), so there's nothing here to exclude.
	CachedVisibilityBeforeRecording = GetVisibility();
	SetVisibility(ESlateVisibility::Collapsed);
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] HUD hidden ahead of recording -> %s"), *FilePath);
}

void USplineRouteHUDWidget::HandleRecordingFinished(bool bSuccess, const FString& FilePath, const FString& Error)
{
	SetVisibility(CachedVisibilityBeforeRecording);
	UE_LOG(LogSplineRoute, Log, TEXT("[SplineRoute] HUD restored after recording (success=%s file=%s error=%s)"),
		bSuccess ? TEXT("true") : TEXT("false"), *FilePath, *Error);
}

void USplineRouteHUDWidget::RefreshWaypointLabel() const
{
	if (!SelectedPointLabel)
	{
		return;
	}
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	ASplineRouteActor* Route = Auth ? Auth->GetActiveRoute() : nullptr;
	const int32 Num = Route ? Route->GetNumControlPoints() : 0;
	const int32 Selected = Route ? Route->GetSelectedControlPoint() : INDEX_NONE;

	if (!Route || Num == 0)
	{
		SelectedPointLabel->SetText(FText::FromString(TEXT("Waypoint: (none)")));
	}
	else if (Selected == INDEX_NONE)
	{
		SelectedPointLabel->SetText(FText::FromString(FString::Printf(TEXT("Waypoint: - / %d"), Num)));
	}
	else
	{
		// 1-based for display so operators see "1 / 5" instead of "0 / 5".
		SelectedPointLabel->SetText(FText::FromString(FString::Printf(TEXT("Waypoint: %d / %d"), Selected + 1, Num)));
	}
}

void USplineRouteHUDWidget::BindRouteSelection(ASplineRouteActor* Route)
{
	if (BoundRoute == Route)
	{
		return;
	}
	UnbindRouteSelection(BoundRoute);
	BoundRoute = Route;
	if (BoundRoute)
	{
		BoundRoute->OnSelectedControlPointChanged.AddDynamic(this, &USplineRouteHUDWidget::HandleSelectedControlPointChanged);
	}
}

void USplineRouteHUDWidget::UnbindRouteSelection(ASplineRouteActor* Route)
{
	if (Route)
	{
		Route->OnSelectedControlPointChanged.RemoveDynamic(this, &USplineRouteHUDWidget::HandleSelectedControlPointChanged);
	}
}

void USplineRouteHUDWidget::ApplyCatalogSliderRanges(bool bResetToDefault)
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	UVesselCatalog* Cat = Auth ? Auth->GetOrLoadCatalog() : nullptr;
	if (!Cat)
	{
		return;
	}

	auto Apply = [bResetToDefault](USlider* Slider, const FVesselSliderRange& Range,
		float FallbackMin, float FallbackMax, float FallbackDefault) -> float
	{
		if (!Slider)
		{
			return FallbackDefault;
		}
		const bool bUseRange = Range.IsSet();
		const float Min = bUseRange ? Range.Min : FallbackMin;
		const float Max = bUseRange ? Range.Max : FallbackMax;
		const float Def = FMath::Clamp(bUseRange ? Range.Default : FallbackDefault, Min, Max);
		Slider->SetMinValue(Min);
		Slider->SetMaxValue(Max);
		if (bResetToDefault)
		{
			Slider->SetValue(Def);
			return Def;
		}
		// Preserve current value but clamp into the new range; only update if it actually moved.
		const float Cur = Slider->GetValue();
		const float Clamped = FMath::Clamp(Cur, Min, Max);
		if (!FMath::IsNearlyEqual(Cur, Clamped))
		{
			Slider->SetValue(Clamped);
		}
		return Clamped;
	};

	const float NewVesselSize    = Apply(VesselSizeSlider,    Cat->VesselSizeRange,    MinVesselSize,    MaxVesselSize,    DefaultVesselSize);
	const float NewSegmentSpeed  = Apply(SegmentSpeedSlider,  Cat->SegmentSpeedRange,  MinSegmentSpeed,  MaxSegmentSpeed,  DefaultSegmentSpeedDisplay);
	const float NewHandleSize    = Apply(HandleSizeSlider,    Cat->HandleSizeRange,    MinHandleSize,    MaxHandleSize,    DefaultHandleSize);
	const float NewTurnRate      = Apply(TurnRateSlider,      Cat->TurnRateRange,      MinTurnRate,      MaxTurnRate,      DefaultTurnRate);
	if (TimeSlider)
	{
		// TimeScale's fallback max comes from the subsystem at runtime (NativeConstruct seeded it).
		const FVesselSliderRange& R = Cat->TimeScaleRange;
		const float FallbackMin = TimeSlider->GetMinValue();
		const float FallbackMax = TimeSlider->GetMaxValue();
		const float FallbackCur = TimeSlider->GetValue();
		const bool bUseRange = R.IsSet();
		const float Min = bUseRange ? R.Min : FallbackMin;
		const float Max = bUseRange ? R.Max : FallbackMax;
		const float Def = FMath::Clamp(bUseRange ? R.Default : FallbackCur, Min, Max);
		TimeSlider->SetMinValue(Min);
		TimeSlider->SetMaxValue(Max);
		if (bResetToDefault && bUseRange)
		{
			TimeSlider->SetValue(Def);
		}
		else
		{
			const float Clamped = FMath::Clamp(FallbackCur, Min, Max);
			if (!FMath::IsNearlyEqual(FallbackCur, Clamped))
			{
				TimeSlider->SetValue(Clamped);
			}
		}
	}

	// If we forced default values, push them through to the systems they drive so visuals/state match.
	if (bResetToDefault)
	{
		if (VesselSizeSlider)
		{
			if (Auth) { Auth->SetVesselSizeMultiplier(NewVesselSize); }
			RefreshVesselSizeLabel(NewVesselSize);
		}
		if (HandleSizeSlider)
		{
			ApplyHandleSizeToActiveRoute(NewHandleSize);
			RefreshHandleSizeLabel(NewHandleSize);
		}
		if (TurnRateSlider)
		{
			if (Auth) { Auth->SetTurnRateForAllVessels(NewTurnRate); }
			RefreshTurnRateLabel(NewTurnRate);
		}
		// SegmentSpeed default isn't pushed to the route — segments only adopt overrides when the user drags.
		(void)NewSegmentSpeed;
	}

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRoute] Applied catalog slider ranges: size=[%.2f..%.2f] segSpeed=[%.0f..%.0f] handle=[%.0f..%.0f] time=[%.2f..%.2f]"),
		VesselSizeSlider    ? VesselSizeSlider->GetMinValue()    : 0.0f, VesselSizeSlider    ? VesselSizeSlider->GetMaxValue()    : 0.0f,
		SegmentSpeedSlider  ? SegmentSpeedSlider->GetMinValue()  : 0.0f, SegmentSpeedSlider  ? SegmentSpeedSlider->GetMaxValue()  : 0.0f,
		HandleSizeSlider    ? HandleSizeSlider->GetMinValue()    : 0.0f, HandleSizeSlider    ? HandleSizeSlider->GetMaxValue()    : 0.0f,
		TimeSlider          ? TimeSlider->GetMinValue()          : 0.0f, TimeSlider          ? TimeSlider->GetMaxValue()          : 0.0f);
}

void USplineRouteHUDWidget::RefreshSegmentSpeedUI()
{
	USplineRouteAuthoringSubsystem* Auth = GetAuthoring(GetWorld());
	ASplineRouteActor* Route = Auth ? Auth->GetActiveRoute() : nullptr;
	const int32 Selected = Route ? Route->GetSelectedControlPoint() : INDEX_NONE;

	const bool bHasSelection = (Route != nullptr) && (Selected != INDEX_NONE) && (Selected < Route->GetNumControlPoints());

	if (SelectedSegmentLabel)
	{
		if (!bHasSelection)
		{
			SelectedSegmentLabel->SetText(FText::FromString(TEXT("Waypoint Speed: (select a point)")));
		}
		else
		{
			const float Override = Route->GetPointSpeedOverride(Selected);
			if (Override < 0.0f)
			{
				SelectedSegmentLabel->SetText(FText::FromString(FString::Printf(
					TEXT("Waypoint %d (no override — drag slider to set %.0f-%.0f u/s)"),
					Selected + 1, MinSegmentSpeed, MaxSegmentSpeed)));
			}
			else
			{
				SelectedSegmentLabel->SetText(FText::FromString(FString::Printf(TEXT("Waypoint %d Speed: %.0f u/s"), Selected + 1, Override)));
			}
		}
	}

	if (SegmentSpeedSlider)
	{
		SegmentSpeedSlider->SetIsEnabled(bHasSelection);
		if (bHasSelection)
		{
			const float Override = Route->GetPointSpeedOverride(Selected);
			// When no override exists, park the slider at the default-display value so the first interaction starts from "default" instead of "frozen".
			const float DisplayValue = (Override < 0.0f) ? DefaultSegmentSpeedDisplay : Override;
			if (!FMath::IsNearlyEqual(SegmentSpeedSlider->GetValue(), DisplayValue))
			{
				SegmentSpeedSlider->SetValue(DisplayValue);
			}
		}
	}
}
