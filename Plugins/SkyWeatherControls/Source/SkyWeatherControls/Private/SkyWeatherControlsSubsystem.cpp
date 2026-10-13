// Copyright DamenTools internal. Sky & Weather Controls plugin.

#include "SkyWeatherControlsSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "SkyWeatherControlsWidget.h"
#include "TimerManager.h"
#include "UltraDynamicSkyBridge.h"

namespace SkyWeatherControls
{
	// Currently inert: the panel this gated has moved into WBP_SplineRouteHUD (see
	// OnWorldBeginPlay()/SetControlsVisible() below), which has no separate auto-show concept.
	// Left registered rather than deleted in case the standalone panel is ever wanted back.
	static TAutoConsoleVariable<bool> CVarAutoShow(
		TEXT("SkyWeather.AutoShow"),
		true,
		TEXT("[Currently unused — the panel moved into WBP_SplineRouteHUD.] Formerly: automatically show the Time of Day / Weather dropdowns in levels that contain Ultra Dynamic Sky."),
		ECVF_Default);

	/** Z-order high enough to sit above the demo HUD without changing that HUD. */
	static constexpr int32 PanelZOrder = 1000;

	static constexpr float SpawnRetryPeriod = 0.25f;
	static constexpr float SpawnTimeoutSeconds = 15.0f;
}

bool USkyWeatherControlsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	// Editor preview / inactive worlds have no viewport to add to.
	return World != nullptr && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void USkyWeatherControlsSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// The standalone Time of Day / Weather panel this subsystem used to auto-spawn has moved
	// into WBP_SplineRouteHUD (SplineRouteHUDWidget's TimeOfDayPicker/WeatherPicker, wired to
	// the same UUltraDynamicSkyBridge calls) so it no longer appears as its own top-right
	// panel. SetControlsVisible() below is now a soft no-op for the same reason — see its own
	// comment — so there is nothing left for AutoShow to usefully schedule here.
}

void USkyWeatherControlsSubsystem::TrySpawnControls()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const bool bTimedOut = World->GetTimeSeconds() >= SpawnDeadlineSeconds;

	if (World->GetFirstPlayerController() == nullptr || !UUltraDynamicSkyBridge::IsSkyPresent(World))
	{
		if (bTimedOut)
		{
			World->GetTimerManager().ClearTimer(SpawnTimerHandle);
			UE_LOG(LogSkyWeatherControls, Log,
				TEXT("No Ultra Dynamic Sky found in '%s'; sky/weather dropdowns not shown."), *World->GetName());
		}
		return;
	}

	World->GetTimerManager().ClearTimer(SpawnTimerHandle);
	SetControlsVisible(true);
}

void USkyWeatherControlsSubsystem::SetControlsVisible(bool bVisible)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (Controls == nullptr)
	{
		if (!bVisible)
		{
			return;
		}

		// The standalone panel this used to create (USkyWeatherControlsWidget, top-right of the
		// viewport) has moved into WBP_SplineRouteHUD — see the note on OnWorldBeginPlay() above.
		// Deliberately not calling CreateWidget() here any more so `SkyWeather.Toggle` can't bring
		// back a second, now-redundant copy of the same controls.
		UE_LOG(LogSkyWeatherControls, Log,
			TEXT("SkyWeather controls now live inside WBP_SplineRouteHUD (TimeOfDayPicker/WeatherPicker) — the standalone panel is no longer spawned."));
		return;
	}

	if (bVisible)
	{
		if (!Controls->IsInViewport())
		{
			Controls->AddToViewport(SkyWeatherControls::PanelZOrder);
		}
		// The widget is given the whole viewport by AddToViewport, so it must not be hit-testable
		// itself -- only its children may take clicks, or the empty rest of the screen swallows
		// input meant for the HUD underneath.
		Controls->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
	else
	{
		Controls->SetVisibility(ESlateVisibility::Collapsed);
	}
}

bool USkyWeatherControlsSubsystem::AreControlsVisible() const
{
	return Controls != nullptr && Controls->IsInViewport() && Controls->GetVisibility() != ESlateVisibility::Collapsed;
}

void USkyWeatherControlsSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SpawnTimerHandle);
	}

	if (Controls != nullptr)
	{
		Controls->RemoveFromParent();
		Controls = nullptr;
	}

	Super::Deinitialize();
}

namespace SkyWeatherControls
{
	static void ToggleControls(UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		if (USkyWeatherControlsSubsystem* Subsystem = World->GetSubsystem<USkyWeatherControlsSubsystem>())
		{
			Subsystem->SetControlsVisible(!Subsystem->AreControlsVisible());
		}
	}

	static FAutoConsoleCommandWithWorld GToggleControlsCommand(
		TEXT("SkyWeather.Toggle"),
		TEXT("Show or hide the Time of Day / Weather dropdowns."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ToggleControls));

	/**
	 * Scriptable equivalents of the two dropdowns. Handy for driving a demo from a console
	 * command or a startup -ExecCmds, and they exercise exactly the same bridge path the
	 * dropdowns use.
	 */
	static void SetWeatherCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogSkyWeatherControls, Display,
				TEXT("Usage: SkyWeather.SetWeather <PresetAssetName> [TransitionSeconds], e.g. Rain_Thunderstorm 4"));
			return;
		}

		const float Transition = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f;
		const bool bApplied = UUltraDynamicSkyBridge::ChangeWeather(World, FName(*Args[0]), Transition);
		UE_LOG(LogSkyWeatherControls, Display, TEXT("SkyWeather.SetWeather %s -> %s"),
			*Args[0], bApplied ? TEXT("applied") : TEXT("FAILED"));
	}

	static void SetTimeCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogSkyWeatherControls, Display,
				TEXT("Usage: SkyWeather.SetTime <0-2400> [TransitionSeconds], e.g. 1830 2"));
			return;
		}

		const float Transition = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.0f;
		const bool bApplied = UUltraDynamicSkyBridge::TransitionTimeOfDay(World, FCString::Atof(*Args[0]), Transition);
		UE_LOG(LogSkyWeatherControls, Display, TEXT("SkyWeather.SetTime %s -> %s (now %.0f)"),
			*Args[0], bApplied ? TEXT("applied") : TEXT("FAILED"), UUltraDynamicSkyBridge::GetTimeOfDay(World));
	}

	static FAutoConsoleCommandWithWorldAndArgs GSetWeatherCommand(
		TEXT("SkyWeather.SetWeather"),
		TEXT("Change Ultra Dynamic Weather to a preset by asset name: SkyWeather.SetWeather <Preset> [Seconds]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetWeatherCommand));

	static FAutoConsoleCommandWithWorldAndArgs GSetTimeCommand(
		TEXT("SkyWeather.SetTime"),
		TEXT("Set Ultra Dynamic Sky Time of Day: SkyWeather.SetTime <0-2400> [Seconds]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetTimeCommand));
}
