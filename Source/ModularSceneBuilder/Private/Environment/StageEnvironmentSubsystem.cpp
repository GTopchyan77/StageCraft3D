// Copyright Epic Games, Inc. All Rights Reserved.

#include "Environment/StageEnvironmentSubsystem.h"

#include "Engine/World.h"
#include "Environment/StageEnvironmentBackend.h"
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
	if (Backend)
	{
		Backend->Shutdown();
		Backend = nullptr;
	}
	bWorldBegunPlay = false;
	Super::Deinitialize();
}

void UStageEnvironmentSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	bWorldBegunPlay = true;
	ApplyToBackend();
}

FText UStageEnvironmentSubsystem::GetSkySystemName() const
{
	return Backend ? Backend->GetDisplayName() : FText::GetEmpty();
}

void UStageEnvironmentSubsystem::SetSettings(const FStageEnvironmentSettings& NewSettings)
{
	FStageEnvironmentSettings Sanitized = StageEnvironment::Sanitize(NewSettings);
	if (Sanitized.bSkyEnabled && !Backend)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Environment: cannot turn the outdoor sky on: no sky backend is installed."));
		Sanitized.bSkyEnabled = false;
	}
	if (Sanitized.Equals(Settings))
	{
		return;
	}
	Settings = Sanitized;
	ApplyToBackend();
	OnEnvironmentChanged.Broadcast(Settings);
}

void UStageEnvironmentSubsystem::ApplyWeatherPreset(EStageWeatherPreset Preset)
{
	SetSettings(StageEnvironment::ApplyWeatherPreset(Settings, Preset));
}

void UStageEnvironmentSubsystem::ApplyToBackend()
{
	UWorld* World = GetWorld();
	if (Backend && bWorldBegunPlay && World)
	{
		Backend->Apply(*World, Settings);
	}
}
