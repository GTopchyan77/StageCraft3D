// Copyright Epic Games, Inc. All Rights Reserved.

#include "Environment/StageLevelLighting.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

void FStageHiddenLevelLighting::Hide(UWorld& World, const AActor& Owner)
{
	if (!Hidden.IsEmpty())
	{
		return;
	}
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		if (*It == &Owner)
		{
			continue;
		}
		for (UActorComponent* Component : It->GetComponents())
		{
			USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
			const bool bLevelLighting = Component && (Component->IsA<UDirectionalLightComponent>() || Component->IsA<USkyLightComponent>()
				|| Component->IsA<USkyAtmosphereComponent>() || Component->IsA<UVolumetricCloudComponent>()
				|| Component->IsA<UExponentialHeightFogComponent>());
			if (bLevelLighting && SceneComponent && SceneComponent->IsVisible())
			{
				SceneComponent->SetVisibility(false);
				Hidden.Add(SceneComponent);
			}
		}
	}
}

void FStageHiddenLevelLighting::Restore()
{
	for (const TWeakObjectPtr<USceneComponent>& Weak : Hidden)
	{
		if (USceneComponent* Component = Weak.Get())
		{
			Component->SetVisibility(true);
		}
	}
	Hidden.Reset();
}
