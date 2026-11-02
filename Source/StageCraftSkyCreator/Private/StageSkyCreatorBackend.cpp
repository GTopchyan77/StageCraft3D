// Copyright Epic Games, Inc. All Rights Reserved.

#include "StageSkyCreatorBackend.h"

#include "Engine/World.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4305) // See StageSkyCreatorWeather.h.
#endif
#include "SkyCreatorActor.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSkyCreatorBackend)

DEFINE_LOG_CATEGORY_STATIC(LogStageSkyCreator, Log, All);

#define LOCTEXT_NAMESPACE "StageSkyCreatorBackend"

void UStageSkyCreatorBackend::Apply(UWorld& World, const FStageEnvironmentSettings& Settings)
{
	// The level's lights or the studio rig (UStageEnvironmentSubsystem) light the stage: no sky.
	if (!Settings.IsOutdoorSky())
	{
		RemoveSky();
		return;
	}

	ASkyCreator* SkyActor = Sky.Get();
	if (!SkyActor)
	{
		SkyActor = SpawnSky(World);
		if (!SkyActor)
		{
			return;
		}
	}
	SkyActor->SetSunSimplePositionSettings(StageSkyCreator::SunriseHour, StageSkyCreator::SunsetHour, Settings.SunElevation, Settings.SunAzimuth);
	SkyActor->SetTime(Settings.TimeOfDay);
	SkyActor->SetWeatherSettings(StageSkyCreator::MakeWeather(BaseWeather, Settings));
}

void UStageSkyCreatorBackend::Shutdown()
{
	// A world that is tearing down destroys its own actors, the sky and the hidden level components included; destroying
	// or showing them from here would act on a world that has already left its world context.
	const ASkyCreator* SkyActor = Sky.Get();
	const UWorld* SkyWorld = SkyActor ? SkyActor->GetWorld() : nullptr;
	if (SkyWorld && SkyWorld->bIsTearingDown)
	{
		Sky.Reset();
		HiddenLevel.Forget();
		return;
	}
	RemoveSky();
}

FText UStageSkyCreatorBackend::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "Sky Creator");
}

ASkyCreator* UStageSkyCreatorBackend::SpawnSky(UWorld& World)
{
	FActorSpawnParameters Params;
	Params.Name = TEXT("StageEnvironmentSky");
	Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Generated from the environment settings on demand: never saved with a level.
	Params.ObjectFlags |= RF_Transient;

	ASkyCreator* SkyActor = World.SpawnActor<ASkyCreator>(ASkyCreator::StaticClass(), FTransform::Identity, Params);
	if (!SkyActor)
	{
		UE_LOG(LogStageSkyCreator, Warning, TEXT("Environment: could not spawn the Sky Creator sky in %s; the outdoor sky stays off."), *World.GetName());
		return nullptr;
	}
	Sky = SkyActor;
	BaseWeather = SkyActor->GetWeatherSettings();
	// The level's sun, sky light, atmosphere, clouds and fog would otherwise light and fog the stage a second time.
	HiddenLevel.Hide(World, *SkyActor);
	UE_LOG(LogStageSkyCreator, Log, TEXT("Environment: outdoor sky on (Sky Creator); %d level sky component(s) hidden."), HiddenLevel.Num());
	return SkyActor;
}

void UStageSkyCreatorBackend::RemoveSky()
{
	if (ASkyCreator* SkyActor = Sky.Get())
	{
		SkyActor->Destroy();
		UE_LOG(LogStageSkyCreator, Log, TEXT("Environment: outdoor sky off; level sky restored."));
	}
	Sky.Reset();

	HiddenLevel.Restore();
}

#undef LOCTEXT_NAMESPACE
