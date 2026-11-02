// Copyright Epic Games, Inc. All Rights Reserved.

#include "Environment/StageEnvironmentSubsystem.h"

#include "Components/PostProcessComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentBackend.h"
#include "Environment/StageStudioRig.h"
#include "GameFramework/Actor.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEnvironmentSubsystem)

bool UStageEnvironmentSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageEnvironmentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (BackendClass.IsNull())
	{
		UE_LOG(LogStageCraft, Log, TEXT("Environment: no sky backend configured; the outdoor sky is unavailable."));
		return;
	}
	// A native class from a module that is already loaded, so this is a lookup rather than a disk load.
	UClass* Class = BackendClass.LoadSynchronous();
	if (!Class || Class->HasAnyClassFlags(CLASS_Abstract) || !Class->IsChildOf<UStageEnvironmentBackend>())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Environment: sky backend '%s' is missing or not a concrete UStageEnvironmentBackend (is its module built and listed in the .uproject?). The outdoor sky is unavailable."),
			*BackendClass.ToString());
		return;
	}
	Backend = NewObject<UStageEnvironmentBackend>(this, Class);
}

void UStageEnvironmentSubsystem::Deinitialize()
{
	bBlending = false;
	if (Backend)
	{
		Backend->Shutdown();
		Backend = nullptr;
	}
	RemoveStudioRig();
	if (AActor* Actor = LookActor.Get(); Actor && !Actor->GetWorld()->bIsTearingDown)
	{
		Actor->Destroy();
	}
	LookActor.Reset();
	LookComponent.Reset();
	if (StudioCubemapHandle.IsValid())
	{
		StudioCubemapHandle->CancelHandle();
		StudioCubemapHandle.Reset();
	}
	bWorldBegunPlay = false;
	Super::Deinitialize();
}

void UStageEnvironmentSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bWorldBegunPlay = true;

	// The look's holder: an unbound post-process component at a low priority, so a level's own volumes still win where they
	// set something (ADR 0024 §3). Transient: generated from the settings, never saved with the level.
	FActorSpawnParameters Params;
	Params.Name = TEXT("StageEnvironmentLook");
	Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	Params.ObjectFlags |= RF_Transient;
	if (AActor* Actor = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params))
	{
		UPostProcessComponent* Component = NewObject<UPostProcessComponent>(Actor, TEXT("Look"));
		Component->bUnbound = true;
		Component->Priority = -1.f;
		Actor->SetRootComponent(Component);
		Component->RegisterComponent();
		LookActor = Actor;
		LookComponent = Component;
	}

	Present(Displayed);
}

TStatId UStageEnvironmentSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UStageEnvironmentSubsystem, STATGROUP_Tickables);
}

bool UStageEnvironmentSubsystem::IsTickable() const
{
	return bBlending;
}

void UStageEnvironmentSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bBlending)
	{
		return;
	}
	BlendElapsed += FMath::Max(DeltaTime, 0.f);
	const float Alpha = BlendSeconds > 0.f ? BlendElapsed / BlendSeconds : 1.f;
	if (Alpha >= 1.f)
	{
		bBlending = false;
		Present(Settings);
		return;
	}
	Present(StageEnvironment::BlendForDisplay(BlendFrom, Settings, Alpha));
}

FText UStageEnvironmentSubsystem::GetSkySystemName() const
{
	return Backend ? Backend->GetDisplayName() : FText::GetEmpty();
}

bool UStageEnvironmentSubsystem::IsStudioHdriAvailable() const
{
	const AStageStudioRig* Rig = StudioRig.Get();
	return Rig ? Rig->UsesHdri() : (StudioCubemap.Get() != nullptr);
}

void UStageEnvironmentSubsystem::SetSettings(const FStageEnvironmentSettings& NewSettings, EStageEnvironmentTransition Transition)
{
	FStageEnvironmentSettings Sanitized = StageEnvironment::Sanitize(NewSettings);
	if (Sanitized.IsOutdoorSky() && !Backend)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Environment: cannot turn the outdoor sky on: no sky backend is installed."));
		Sanitized.Lighting = EStageLightingSource::Level;
	}
	if (Sanitized.Equals(Settings))
	{
		return;
	}
	Settings = Sanitized;

	if (Transition == EStageEnvironmentTransition::Blend && BlendSeconds > 0.f && bWorldBegunPlay)
	{
		// From what is on screen, so a change during a blend continues smoothly instead of jumping.
		BlendFrom = Displayed;
		BlendElapsed = 0.f;
		bBlending = true;
	}
	else
	{
		bBlending = false;
		Present(Settings);
	}
	OnEnvironmentChanged.Broadcast(Settings);
}

void UStageEnvironmentSubsystem::ApplyWeatherPreset(EStageWeatherPreset Preset)
{
	SetSettings(StageEnvironment::ApplyWeatherPreset(Settings, Preset));
}

void UStageEnvironmentSubsystem::Present(const FStageEnvironmentSettings& Show)
{
	Displayed = Show;
	UWorld* World = GetWorld();
	if (!bWorldBegunPlay || !World)
	{
		return;
	}

	// Order matters: the backend gives the level's lights back before the rig hides them again, and the other way round.
	if (Show.Lighting != EStageLightingSource::Studio)
	{
		RemoveStudioRig();
	}
	if (Backend)
	{
		Backend->Apply(*World, Show);
	}
	if (Show.Lighting == EStageLightingSource::Studio)
	{
		EnsureStudioRig(*World);
		if (AStageStudioRig* Rig = StudioRig.Get())
		{
			Rig->ApplyLook(Show.Look);
		}
	}
	ApplyLook(Show.Look);
}

void UStageEnvironmentSubsystem::ApplyLook(const FStageEnvironmentLook& Look)
{
	UPostProcessComponent* Component = LookComponent.Get();
	if (!Component)
	{
		return;
	}
	// Every field is overridden: the defaults of FStageEnvironmentLook are the engine's own, so a stage that never set a
	// look renders exactly as before, and a level volume with a higher priority still overrides what it sets.
	FPostProcessSettings& PP = Component->Settings;
	PP.bOverride_AutoExposureBias = true;
	PP.AutoExposureBias = Look.ExposureCompensation;
	PP.bOverride_BloomIntensity = true;
	PP.BloomIntensity = Look.BloomIntensity;
	PP.bOverride_AmbientOcclusionIntensity = true;
	PP.AmbientOcclusionIntensity = Look.AmbientOcclusionIntensity;
	PP.bOverride_WhiteTemp = true;
	PP.WhiteTemp = Look.WhiteBalanceTemp;
	PP.bOverride_ColorSaturation = true;
	PP.ColorSaturation = FVector4(1.f, 1.f, 1.f, Look.Saturation);
	PP.bOverride_ColorContrast = true;
	PP.ColorContrast = FVector4(1.f, 1.f, 1.f, Look.Contrast);
	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = Look.VignetteIntensity;
}

void UStageEnvironmentSubsystem::EnsureStudioRig(UWorld& World)
{
	if (StudioRig.IsValid())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.Name = TEXT("StageStudioRig");
	Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	AStageStudioRig* Rig = World.SpawnActor<AStageStudioRig>(AStageStudioRig::StaticClass(), FTransform::Identity, Params);
	if (!Rig)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Environment: could not spawn the studio rig in %s; the level's lighting stays."), *World.GetName());
		return;
	}
	StudioRig = Rig;

	// The HDRI arrives asynchronously; until then (or if it never does) the rig lights with its flat fallback.
	UTextureCube* Loaded = StudioCubemap.Get();
	Rig->SetCubemap(Loaded);
	if (!Loaded && !StudioCubemap.IsNull() && !StudioCubemapHandle.IsValid())
	{
		StudioCubemapHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(StudioCubemap.ToSoftObjectPath(),
			FStreamableDelegate::CreateUObject(this, &ThisClass::HandleStudioCubemapLoaded));
	}
	UE_LOG(LogStageCraft, Log, TEXT("Environment: studio lighting on (%s)."), Loaded ? TEXT("HDRI") : TEXT("flat light until the HDRI loads"));
}

void UStageEnvironmentSubsystem::HandleStudioCubemapLoaded()
{
	UTextureCube* Loaded = StudioCubemap.Get();
	if (!Loaded)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Environment: studio HDRI '%s' could not be loaded; the studio keeps its flat light."), *StudioCubemap.ToString());
		return;
	}
	if (AStageStudioRig* Rig = StudioRig.Get())
	{
		Rig->SetCubemap(Loaded);
	}
}

void UStageEnvironmentSubsystem::RemoveStudioRig()
{
	AStageStudioRig* Rig = StudioRig.Get();
	StudioRig.Reset();
	if (Rig && Rig->GetWorld() && !Rig->GetWorld()->bIsTearingDown)
	{
		Rig->Destroy();
		UE_LOG(LogStageCraft, Log, TEXT("Environment: studio lighting off; level lighting restored."));
	}
}
