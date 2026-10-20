// Copyright Epic Games, Inc. All Rights Reserved.

#include "StageSkyCreatorBackend.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

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
	if (!Settings.bSkyEnabled)
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
		HiddenLevelComponents.Reset();
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
	HideLevelSky(World, *SkyActor);
	UE_LOG(LogStageSkyCreator, Log, TEXT("Environment: outdoor sky on (Sky Creator); %d level sky component(s) hidden."), HiddenLevelComponents.Num());
	return SkyActor;
}

void UStageSkyCreatorBackend::HideLevelSky(UWorld& World, const AActor& SkyActor)
{
	// One pass when the sky turns on, not a poll: the level's sun, sky light, atmosphere, clouds and fog would otherwise
	// light and fog the stage a second time on top of the sky's own.
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		if (*It == &SkyActor)
		{
			continue;
		}
		for (UActorComponent* Component : It->GetComponents())
		{
			USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
			const bool bLevelSky = Component && (Component->IsA<UDirectionalLightComponent>() || Component->IsA<USkyLightComponent>()
				|| Component->IsA<USkyAtmosphereComponent>() || Component->IsA<UVolumetricCloudComponent>()
				|| Component->IsA<UExponentialHeightFogComponent>());
			if (bLevelSky && SceneComponent && SceneComponent->IsVisible())
			{
				SceneComponent->SetVisibility(false);
				HiddenLevelComponents.Add(SceneComponent);
			}
		}
	}
}

void UStageSkyCreatorBackend::RemoveSky()
{
	if (ASkyCreator* SkyActor = Sky.Get())
	{
		SkyActor->Destroy();
		UE_LOG(LogStageSkyCreator, Log, TEXT("Environment: outdoor sky off; level sky restored."));
	}
	Sky.Reset();

	for (const TWeakObjectPtr<USceneComponent>& Hidden : HiddenLevelComponents)
	{
		if (USceneComponent* Component = Hidden.Get())
		{
			Component->SetVisibility(true);
		}
	}
	HiddenLevelComponents.Reset();
}

#undef LOCTEXT_NAMESPACE
