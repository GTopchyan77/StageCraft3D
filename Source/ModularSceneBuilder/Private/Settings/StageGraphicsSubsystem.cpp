// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/StageGraphicsSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGraphicsSubsystem)

UGameUserSettings* UStageGraphicsSubsystem::GetEngineSettings()
{
	return GEngine ? GEngine->GetGameUserSettings() : nullptr;
}

FStageGraphicsChoices UStageGraphicsSubsystem::GetChoices() const
{
	const UGameUserSettings* Settings = GetEngineSettings();
	return Settings ? ReadChoices(*Settings) : FStageGraphicsChoices();
}

bool UStageGraphicsSubsystem::SetChoices(const FStageGraphicsChoices& Choices)
{
	UGameUserSettings* Settings = GetEngineSettings();
	if (!Settings || !WriteChoices(*Settings, Choices))
	{
		return false;
	}
	// Scalability levels (with the render resolution), VSync and the frame cap all apply here; the window mode and size are
	// not touched, so no display mode change happens.
	Settings->ApplyNonResolutionSettings();
	Settings->SaveSettings();
	const FStageGraphicsChoices Stored = ReadChoices(*Settings);
	UE_LOG(LogStageCraft, Log, TEXT("Graphics: quality %d, resolution %d %%, frame cap %d, VSync %s (saved)."),
		StageGraphics::ToScalabilityLevel(Stored.Quality), Stored.ResolutionPercent, Stored.FrameCap, Stored.bVSync ? TEXT("on") : TEXT("off"));
	OnGraphicsChanged.Broadcast();
	return true;
}

FStageGraphicsChoices UStageGraphicsSubsystem::ReadChoices(const UGameUserSettings& Settings)
{
	float Normalized = 0.f;
	float Percent = 0.f;
	float MinPercent = 0.f;
	float MaxPercent = 0.f;
	Settings.GetResolutionScaleInformationEx(Normalized, Percent, MinPercent, MaxPercent);

	FStageGraphicsChoices Choices;
	const int32 GroupLevels[] =
	{
		Settings.GetViewDistanceQuality(), Settings.GetAntiAliasingQuality(), Settings.GetShadowQuality(), Settings.GetGlobalIlluminationQuality(),
		Settings.GetReflectionQuality(), Settings.GetPostProcessingQuality(), Settings.GetTextureQuality(), Settings.GetVisualEffectQuality(),
		Settings.GetFoliageQuality(), Settings.GetShadingQuality(), Settings.GetLandscapeQuality()
	};
	Choices.Quality = StageGraphics::FromGroupLevels(GroupLevels);
	Choices.ResolutionPercent = FMath::RoundToInt(Percent);
	Choices.FrameCap = FMath::RoundToInt(Settings.GetFrameRateLimit());
	Choices.bVSync = Settings.IsVSyncEnabled();
	return Choices;
}

bool UStageGraphicsSubsystem::WriteChoices(UGameUserSettings& Settings, const FStageGraphicsChoices& Choices)
{
	const FStageGraphicsChoices Clean = StageGraphics::Sanitize(Choices);
	const FStageGraphicsChoices Before = ReadChoices(Settings);

	const int32 Level = StageGraphics::ToScalabilityLevel(Clean.Quality);
	if (Level != INDEX_NONE && Clean.Quality != Before.Quality)
	{
		Settings.SetOverallScalabilityLevel(Level);
	}
	// Always, after the quality: the single level above set the engine's own resolution for that level, and the
	// Resolution row is a choice of its own.
	Settings.SetResolutionScaleValueEx(static_cast<float>(Clean.ResolutionPercent));
	Settings.SetFrameRateLimit(static_cast<float>(Clean.FrameCap));
	Settings.SetVSyncEnabled(Clean.bVSync);

	return ReadChoices(Settings) != Before;
}

#if !UE_BUILD_SHIPPING

namespace StageGraphicsConsole
{
	FString Describe(const FStageGraphicsChoices& Choices)
	{
		static const TCHAR* Names[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Custom") };
		return FString::Printf(TEXT("Quality=%s Resolution=%d Cap=%d VSync=%d"), Names[static_cast<uint8>(Choices.Quality)],
			Choices.ResolutionPercent, Choices.FrameCap, Choices.bVSync ? 1 : 0);
	}

	void Graphics(const TArray<FString>& Args, UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageGraphicsSubsystem* Graphics = GameInstance ? GameInstance->GetSubsystem<UStageGraphicsSubsystem>() : nullptr;
		if (!Graphics)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Graphics: no game instance."));
			return;
		}
		FStageGraphicsChoices Choices = Graphics->GetChoices();
		for (const FString& Arg : Args)
		{
			FString Key;
			FString Value;
			if (!Arg.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Key == TEXT("Quality"))
			{
				const TArray<FString> Levels = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic") };
				const int32 Level = Levels.IndexOfByKey(Value);
				Choices.Quality = Level == INDEX_NONE ? Choices.Quality : static_cast<EStageGraphicsQuality>(Level);
			}
			else if (Key == TEXT("Resolution"))
			{
				Choices.ResolutionPercent = FCString::Atoi(*Value);
			}
			else if (Key == TEXT("Cap"))
			{
				Choices.FrameCap = FCString::Atoi(*Value);
			}
			else if (Key == TEXT("VSync"))
			{
				Choices.bVSync = FCString::Atoi(*Value) != 0;
			}
		}
		const bool bChanged = !Args.IsEmpty() && Graphics->SetChoices(Choices);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Graphics: %s%s; r.ScreenPercentage %.0f, t.MaxFPS %.0f, sg.ShadowQuality %d."),
			*Describe(Graphics->GetChoices()), bChanged ? TEXT(" (changed)") : TEXT(""),
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"))->GetFloat(),
			IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))->GetFloat(),
			IConsoleManager::Get().FindConsoleVariable(TEXT("sg.ShadowQuality"))->GetInt());
	}

	FAutoConsoleCommandWithWorldAndArgs GraphicsCommand(TEXT("StageCraft.Graphics"),
		TEXT("[Quality=Low|Medium|High|Epic] [Resolution=0|25..100] [Cap=0|15..360] [VSync=0|1] Logs, or sets as the Preferences panel does (applied and saved). Development only."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Graphics));
}

#endif // !UE_BUILD_SHIPPING
