// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the outdoor environment (Docs/ADR/0009). Each one goes through
// UStageEnvironmentSubsystem::SetSettings, the Environment panel's own write path, so a scripted run exercises the real
// path. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "Environment/StageEnvironmentSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"

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
		UE_LOG(LogStageCraft, Display, TEXT("%s: sky %s (%s); time %.2f h, sun height %.0f deg, sun direction %.0f deg; clouds %.2f, rain %.2f, snow %.2f, fog %.2f; preset %s."),
			Command, Settings.bSkyEnabled ? TEXT("on") : TEXT("off"),
			Environment.IsSkyAvailable() ? *Environment.GetSkySystemName().ToString() : TEXT("no backend"),
			Settings.TimeOfDay, Settings.SunElevation, Settings.SunAzimuth, Settings.CloudCoverage, Settings.Rain, Settings.Snow, Settings.Fog,
			*UEnum::GetValueAsString(StageEnvironment::FindWeatherPreset(Settings)));
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
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Environment.Set <Sky|Time|SunHeight|SunDirection|Clouds|Rain|Snow|Fog> <value> (Sky: on|off)"));
			return;
		}
		FStageEnvironmentSettings Settings = Environment->GetSettings();
		const FString& Field = Args[0];
		const float Value = FCString::Atof(*Args[1]);
		if (Field.Equals(TEXT("Sky"), ESearchCase::IgnoreCase))			{ Settings.bSkyEnabled = Args[1].Equals(TEXT("on"), ESearchCase::IgnoreCase) || Args[1] == TEXT("1"); }
		else if (Field.Equals(TEXT("Time"), ESearchCase::IgnoreCase))		{ Settings.TimeOfDay = Value; }
		else if (Field.Equals(TEXT("SunHeight"), ESearchCase::IgnoreCase))	{ Settings.SunElevation = Value; }
		else if (Field.Equals(TEXT("SunDirection"), ESearchCase::IgnoreCase)) { Settings.SunAzimuth = Value; }
		else if (Field.Equals(TEXT("Clouds"), ESearchCase::IgnoreCase))		{ Settings.CloudCoverage = Value; }
		else if (Field.Equals(TEXT("Rain"), ESearchCase::IgnoreCase))		{ Settings.Rain = Value; }
		else if (Field.Equals(TEXT("Snow"), ESearchCase::IgnoreCase))		{ Settings.Snow = Value; }
		else if (Field.Equals(TEXT("Fog"), ESearchCase::IgnoreCase))		{ Settings.Fog = Value; }
		else
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Environment.Set: unknown field '%s'."), *Field);
			return;
		}
		Environment->SetSettings(Settings);
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
		Environment->ApplyWeatherPreset(static_cast<EStageWeatherPreset>(Value));
		LogSettings(TEXT("StageCraft.Environment.Preset"), *Environment);
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Environment.Status"), TEXT("Logs the outdoor sky, time, sun path and weather."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SetCommand(TEXT("StageCraft.Environment.Set"), TEXT("<Sky|Time|SunHeight|SunDirection|Clouds|Rain|Snow|Fog> <value> The Environment panel's controls."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Set));
	FAutoConsoleCommandWithWorldAndArgs PresetCommand(TEXT("StageCraft.Environment.Preset"), TEXT("<Clear|Cloudy|Rain|Snow|Fog> The Environment panel's weather presets."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Preset));
}

#endif // !UE_BUILD_SHIPPING
