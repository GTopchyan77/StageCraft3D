// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the outdoor environment (Docs/ADR/0009). Each one writes like the Environment panel:
// through AModularPlayerController::RequestSetEnvironment as one undo step (ADR 0009 §9), so a scripted run exercises the
// real path. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Environment/StageAtmospherePreset.h"
#include "Environment/StageAtmosphereSubsystem.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/IConsoleManager.h"
#include "History/StageEditHistorySubsystem.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "RenderTimer.h"
#include "UI/StageEnvironmentPanel.h"
#include "UI/StageValueSlider.h"
#include "UObject/UObjectIterator.h"

namespace StageEnvironmentCommands
{
	UStageEnvironmentSubsystem* GetEnvironment(UWorld* World)
	{
		UStageEnvironmentSubsystem* Environment = UWorld::GetSubsystem<UStageEnvironmentSubsystem>(World);
		if (!Environment)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Environment: no environment in this world (Game and PIE worlds only)."));
		}
		return Environment;
	}

	void LogSettings(const TCHAR* Command, const UStageEnvironmentSubsystem& Environment)
	{
		const FStageEnvironmentSettings Settings = Environment.GetSettings();
		UE_LOG(LogStageCraft, Display, TEXT("%s: lighting %s (sky system %s); time %.2f h, sun height %.0f deg, sun direction %.0f deg; clouds %.2f, rain %.2f, snow %.2f, fog %.2f; preset %s."),
			Command, *StaticEnum<EStageLightingSource>()->GetNameStringByValue(int64(Settings.Lighting)),
			Environment.IsSkyAvailable() ? *Environment.GetSkySystemName().ToString() : TEXT("no backend"),
			Settings.TimeOfDay, Settings.SunElevation, Settings.SunAzimuth, Settings.CloudCoverage, Settings.Rain, Settings.Snow, Settings.Fog,
			*UEnum::GetValueAsString(StageEnvironment::FindWeatherPreset(Settings)));
	}

	/** Writes Settings as the panel does: through the controller as one undo step named StepName, or directly without one. */
	void Write(UWorld* World, UStageEnvironmentSubsystem& Environment, const FStageEnvironmentSettings& Settings, const FString& StepName)
	{
		if (AModularPlayerController* Controller = Cast<AModularPlayerController>(World->GetFirstPlayerController()))
		{
			Controller->RequestSetEnvironment(Settings, FText::FromString(StepName));
		}
		else
		{
			Environment.SetSettings(Settings);
		}
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (const UStageEnvironmentSubsystem* Environment = GetEnvironment(World))
		{
			LogSettings(TEXT("StageCraft.Environment.Status"), *Environment);
		}
	}

	void Set(const TArray<FString>& Args, UWorld* World)
	{
		UStageEnvironmentSubsystem* Environment = GetEnvironment(World);
		if (!Environment || Args.Num() < 2)
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Environment.Set <Sky|Lighting|Time|SunHeight|SunDirection|Clouds|Rain|Snow|Fog|Exposure|Bloom|WhiteBalance> <value> (Sky: on|off; Lighting: Level|OutdoorSky|Studio)"));
			return;
		}
		FStageEnvironmentSettings Settings = Environment->GetSettings();
		const FString& Field = Args[0];
		const float Value = FCString::Atof(*Args[1]);
		if (Field.Equals(TEXT("Sky"), ESearchCase::IgnoreCase))			{ Settings.Lighting = Args[1].Equals(TEXT("on"), ESearchCase::IgnoreCase) || Args[1] == TEXT("1") ? EStageLightingSource::OutdoorSky : EStageLightingSource::Level; }
		else if (Field.Equals(TEXT("Time"), ESearchCase::IgnoreCase))		{ Settings.TimeOfDay = Value; }
		else if (Field.Equals(TEXT("SunHeight"), ESearchCase::IgnoreCase))	{ Settings.SunElevation = Value; }
		else if (Field.Equals(TEXT("SunDirection"), ESearchCase::IgnoreCase)) { Settings.SunAzimuth = Value; }
		else if (Field.Equals(TEXT("Clouds"), ESearchCase::IgnoreCase))		{ Settings.CloudCoverage = Value; }
		else if (Field.Equals(TEXT("Rain"), ESearchCase::IgnoreCase))		{ Settings.Rain = Value; }
		else if (Field.Equals(TEXT("Snow"), ESearchCase::IgnoreCase))		{ Settings.Snow = Value; }
		else if (Field.Equals(TEXT("Fog"), ESearchCase::IgnoreCase))		{ Settings.Fog = Value; }
		else if (Field.Equals(TEXT("Lighting"), ESearchCase::IgnoreCase))
		{
			const int64 Source = StaticEnum<EStageLightingSource>()->GetValueByNameString(Args[1], EGetByNameFlags::None);
			if (Source == INDEX_NONE)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Environment.Set Lighting: Level, OutdoorSky or Studio."));
				return;
			}
			Settings.Lighting = EStageLightingSource(Source);
		}
		else if (Field.Equals(TEXT("Exposure"), ESearchCase::IgnoreCase))	{ Settings.Look.ExposureCompensation = Value; }
		else if (Field.Equals(TEXT("Bloom"), ESearchCase::IgnoreCase))		{ Settings.Look.BloomIntensity = Value; }
		else if (Field.Equals(TEXT("WhiteBalance"), ESearchCase::IgnoreCase)) { Settings.Look.WhiteBalanceTemp = Value; }
		else
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Environment.Set: unknown field '%s'."), *Field);
			return;
		}
		Write(World, *Environment, Settings, FString::Join(Args, TEXT(" ")));
		LogSettings(TEXT("StageCraft.Environment.Set"), *Environment);
	}

	void Preset(const TArray<FString>& Args, UWorld* World)
	{
		UStageEnvironmentSubsystem* Environment = GetEnvironment(World);
		const UEnum* Enum = StaticEnum<EStageWeatherPreset>();
		const int64 Value = Environment && !Args.IsEmpty() ? Enum->GetValueByNameString(Args[0], EGetByNameFlags::None) : INDEX_NONE;
		if (Value == INDEX_NONE || Value == int64(EStageWeatherPreset::Custom))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Environment.Preset <Clear|Cloudy|Rain|Snow|Fog>"));
			return;
		}
		Write(World, *Environment, StageEnvironment::ApplyWeatherPreset(Environment->GetSettings(), static_cast<EStageWeatherPreset>(Value)), TEXT("Weather: ") + Args[0]);
		LogSettings(TEXT("StageCraft.Environment.Preset"), *Environment);
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Environment.Status"), TEXT("Logs the outdoor sky, time, sun path and weather."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SetCommand(TEXT("StageCraft.Environment.Set"), TEXT("<Sky|Lighting|Time|SunHeight|SunDirection|Clouds|Rain|Snow|Fog|Exposure|Bloom|WhiteBalance> <value> The Environment panel's controls."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Set));
	FAutoConsoleCommandWithWorldAndArgs PresetCommand(TEXT("StageCraft.Environment.Preset"), TEXT("<Clear|Cloudy|Rain|Snow|Fog> The Environment panel's weather presets."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Preset));

	/** The atmosphere whose display name or asset name contains Text (case-insensitive), or null. */
	UStageAtmospherePreset* FindAtmosphereByText(const UStageAtmosphereSubsystem& Catalog, const FString& Text)
	{
		for (UStageAtmospherePreset* Preset : Catalog.GetAtmospheres())
		{
			if (Preset && (Preset->DisplayName.ToString().Replace(TEXT(" "), TEXT("")).Contains(Text.Replace(TEXT(" "), TEXT("")))
				|| Preset->GetName().Contains(Text)))
			{
				return Preset;
			}
		}
		return nullptr;
	}

	/** Applies an atmosphere through the controller, exactly as the panel's card does (one undo step, blended). */
	void Atmosphere(const TArray<FString>& Args, UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UStageAtmosphereSubsystem* Catalog = GameInstance ? GameInstance->GetSubsystem<UStageAtmosphereSubsystem>() : nullptr;
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		UStageAtmospherePreset* Preset = Catalog && !Args.IsEmpty() ? FindAtmosphereByText(*Catalog, FString::Join(Args, TEXT(" "))) : nullptr;
		if (!Preset || !Controller)
		{
			FString Names;
			for (const UStageAtmospherePreset* Each : Catalog ? Catalog->GetAtmospheres() : TArray<UStageAtmospherePreset*>())
			{
				Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + Each->DisplayName.ToString();
			}
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Environment.Atmosphere <name> (%s)%s"), *Names, Controller ? TEXT("") : TEXT("; needs a StageCraft player controller"));
			return;
		}
		const bool bApplied = Controller->RequestApplyAtmosphere(Preset->GetPrimaryAssetId());
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Environment.Atmosphere: %s %s."), *Preset->DisplayName.ToString(), bApplied ? TEXT("applied (blending)") : TEXT("refused"));
	}

	/**
	 * Measures what atmosphere blends cost (ADR 0024 §5): applies every atmosphere in turn, each followed by a second at rest,
	 * and logs frame and game-thread times while blending against at rest. Run in a -game session; numbers, not guesses.
	 */
	struct FAtmosphereBench
	{
		TArray<float> BlendFrames, IdleFrames, BlendGame, IdleGame;
		int32 Next = 0;
		bool bWasBlending = false;
		double NextAt = 0.0;
		FTSTicker::FDelegateHandle Handle;
		TWeakObjectPtr<UWorld> World;
	};

	void BenchAtmosphereBlend(const TArray<FString>& Args, UWorld* World)
	{
		static TSharedPtr<FAtmosphereBench> Running;
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UStageAtmosphereSubsystem* Catalog = GameInstance ? GameInstance->GetSubsystem<UStageAtmosphereSubsystem>() : nullptr;
		if (Running.IsValid() || !Catalog || Catalog->GetAtmospheres().IsEmpty() || !GetEnvironment(World))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.AtmosphereBlend: %s"), Running.IsValid() ? TEXT("already running.") : TEXT("needs a game world with loaded atmospheres."));
			return;
		}
		Running = MakeShared<FAtmosphereBench>();
		Running->World = World;
		Running->NextAt = FPlatformTime::Seconds();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Bench.AtmosphereBlend: cycling %d atmospheres."), Catalog->GetAtmospheres().Num());

		Running->Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float DeltaTime)
		{
			const TSharedPtr<FAtmosphereBench> Bench = Running;
			UWorld* BenchWorld = Bench.IsValid() ? Bench->World.Get() : nullptr;
			const UStageEnvironmentSubsystem* Environment = BenchWorld ? UWorld::GetSubsystem<UStageEnvironmentSubsystem>(BenchWorld) : nullptr;
			const UStageAtmosphereSubsystem* BenchCatalog = BenchWorld && BenchWorld->GetGameInstance() ? BenchWorld->GetGameInstance()->GetSubsystem<UStageAtmosphereSubsystem>() : nullptr;
			if (!Environment || !BenchCatalog)
			{
				Running.Reset();
				return false;
			}

			const float FrameMs = DeltaTime * 1000.f;
			const float GameMs = float(FPlatformTime::ToMilliseconds(GGameThreadTime));
			(Environment->IsBlending() ? Bench->BlendFrames : Bench->IdleFrames).Add(FrameMs);
			(Environment->IsBlending() ? Bench->BlendGame : Bench->IdleGame).Add(GameMs);

			// A second at rest counts from the end of each blend.
			if (Environment->IsBlending())
			{
				Bench->bWasBlending = true;
				return true;
			}
			if (Bench->bWasBlending)
			{
				Bench->bWasBlending = false;
				Bench->NextAt = FPlatformTime::Seconds() + 1.0;
			}
			if (FPlatformTime::Seconds() < Bench->NextAt)
			{
				return true;
			}
			const TArray<UStageAtmospherePreset*>& Presets = BenchCatalog->GetAtmospheres();
			if (Bench->Next < Presets.Num())
			{
				AModularPlayerController* Controller = Cast<AModularPlayerController>(BenchWorld->GetFirstPlayerController());
				if (Controller && Presets[Bench->Next])
				{
					Controller->RequestApplyAtmosphere(Presets[Bench->Next]->GetPrimaryAssetId());
				}
				++Bench->Next;
				return true;
			}

			auto Describe = [](const TArray<float>& Values)
			{
				if (Values.IsEmpty())
				{
					return FString(TEXT("no frames"));
				}
				TArray<float> Sorted = Values;
				Sorted.Sort();
				float Sum = 0.f;
				for (const float Value : Values)
				{
					Sum += Value;
				}
				return FString::Printf(TEXT("%d frames, avg %.2f ms, p95 %.2f ms, worst %.2f ms"), Values.Num(), Sum / Values.Num(),
					Sorted[FMath::Min(Sorted.Num() - 1, FMath::FloorToInt(Sorted.Num() * 0.95f))], Sorted.Last());
			};
			UE_LOG(LogStageCraft, Display, TEXT("Bench.AtmosphereBlend frame while blending: %s"), *Describe(Bench->BlendFrames));
			UE_LOG(LogStageCraft, Display, TEXT("Bench.AtmosphereBlend frame at rest:        %s"), *Describe(Bench->IdleFrames));
			UE_LOG(LogStageCraft, Display, TEXT("Bench.AtmosphereBlend game thread blending: %s"), *Describe(Bench->BlendGame));
			UE_LOG(LogStageCraft, Display, TEXT("Bench.AtmosphereBlend game thread at rest:  %s"), *Describe(Bench->IdleGame));
			Running.Reset();
			return false;
		}));
	}

	FAutoConsoleCommandWithWorldAndArgs AtmosphereCommand(TEXT("StageCraft.Environment.Atmosphere"), TEXT("<name> Applies an atmosphere preset like the Environment panel's card (one undo step, blended)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Atmosphere));
	FAutoConsoleCommandWithWorldAndArgs BenchCommand(TEXT("StageCraft.Bench.AtmosphereBlend"), TEXT("Cycles every atmosphere and logs frame and game-thread times while blending against at rest."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&BenchAtmosphereBlend));

	/** The open Environment panel's slider Index in World (shown, so it has a geometry to click), or null. */
	UStageValueSlider* FindPanelSlider(const UWorld* World, int32 Index)
	{
		for (TObjectIterator<UStageValueSlider> It; It; ++It)
		{
			if (It->GetWorld() == World && It->GetSliderIndex() == Index && It->GetTypedOuter<UStageEnvironmentPanel>() && It->GetCachedWidget().IsValid())
			{
				return *It;
			}
		}
		return nullptr;
	}

	/**
	 * StageCraft.Environment.DragSlider <Index> <To 0..1> [Frames=10]: a real left-button drag through Slate on the open
	 * Environment panel's slider Index (panel order, 0 Time of day ... 9 White balance), from its handle to the fraction To of
	 * its length, as a user's drag; after the release it logs the value and the undo stack, so a run can count the steps.
	 */
	void DragSlider(const TArray<FString>& Args, UWorld* World)
	{
		UStageValueSlider* Slider = Args.Num() >= 2 ? FindPanelSlider(World, FCString::Atoi(*Args[0])) : nullptr;
		if (!Slider || !FSlateApplication::IsInitialized())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Environment.DragSlider <0..9> <To 0..1> [Frames=10] (the Environment panel must be open)"));
			return;
		}
		const float To = FMath::Clamp(FCString::Atof(*Args[1]), 0.f, 1.f);
		const int32 Frames = Args.IsValidIndex(2) ? FMath::Clamp(FCString::Atoi(*Args[2]), 1, 600) : 10;
		const FGeometry& Geometry = Slider->GetCachedWidget()->GetCachedGeometry();
		const FVector2D Start = FVector2D(Geometry.GetAbsolutePositionAtCoordinates(FVector2f(Slider->GetNormalizedValue(), 0.5f)));
		const FVector2D End = FVector2D(Geometry.GetAbsolutePositionAtCoordinates(FVector2f(To, 0.5f)));

		FSlateApplication& Slate = FSlateApplication::Get();
		const FInputDeviceId Mouse = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
		const TSet<FKey> Held({ EKeys::LeftMouseButton });
		Slate.SetCursorPos(Start);
		Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(Mouse, FSlateApplicationBase::CursorPointerIndex, Start, Start, Held, EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
		// A hidden panel (behind another tab) would take no click; the log says so instead of reporting an unchanged value.
		const bool bGrabbed = Slider->GetCachedWidget()->HasMouseCapture();

		const TWeakObjectPtr<UStageValueSlider> WeakSlider(Slider);
		const TWeakObjectPtr<UWorld> WeakWorld(World);
		int32 Frame = 0;
		FVector2D Last = Start;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakSlider, WeakWorld, Mouse, Held, Start, End, Frames, bGrabbed, Frame, Last](float) mutable
		{
			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}
			FSlateApplication& Slate = FSlateApplication::Get();
			if (++Frame <= Frames)
			{
				const FVector2D Position = FMath::Lerp(Start, End, float(Frame) / Frames);
				Slate.SetCursorPos(Position);
				Slate.ProcessMouseMoveEvent(FPointerEvent(Mouse, FSlateApplicationBase::CursorPointerIndex, Position, Last, Held, EKeys::Invalid, 0.f, FModifierKeysState()));
				Last = Position;
				return true;
			}
			Slate.ProcessMouseButtonUpEvent(FPointerEvent(Mouse, FSlateApplicationBase::CursorPointerIndex, Last, Last, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState()));
			const UStageEditHistorySubsystem* History = WeakWorld.IsValid() ? UWorld::GetSubsystem<UStageEditHistorySubsystem>(WeakWorld.Get()) : nullptr;
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Environment.DragSlider %d: %s, %d moves, value now %.3f; undo steps %d, next undo \"%s\"."),
				WeakSlider.IsValid() ? WeakSlider->GetSliderIndex() : INDEX_NONE, bGrabbed ? TEXT("grabbed") : TEXT("NOT GRABBED (panel hidden?)"), Frames, WeakSlider.IsValid() ? WeakSlider->GetValue() : 0.f,
				History ? History->GetUndoCount() : -1, History ? *History->GetUndoDescription().ToString() : TEXT("-"));
			return false;
		}));
	}

	FAutoConsoleCommandWithWorldAndArgs DragSliderCommand(TEXT("StageCraft.Environment.DragSlider"), TEXT("<0..9> <To 0..1> [Frames] A real mouse drag through Slate on the open Environment panel's slider; logs the undo stack after."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DragSlider));
}

#endif // !UE_BUILD_SHIPPING
