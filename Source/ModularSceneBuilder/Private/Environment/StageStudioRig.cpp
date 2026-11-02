// Copyright Epic Games, Inc. All Rights Reserved.

#include "Environment/StageStudioRig.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "Environment/StageEnvironmentTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageStudioRig)

namespace StageStudioRig
{
	/** Light grey of the backdrop haze and of the fallback sky light's lower hemisphere. */
	const FLinearColor Backdrop(0.62f, 0.63f, 0.65f);
	/** The key light comes from the front left, 40 degrees up. */
	const FRotator KeyDirection(-40.f, -35.f, 0.f);
}

AStageStudioRig::AStageStudioRig()
{
	PrimaryActorTick.bCanEverTick = false;
	SetCanBeDamaged(false);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	SetRootComponent(Root);

	SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
	SkyLight->SetupAttachment(Root);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->bRealTimeCapture = false;
	SkyLight->SourceType = SLS_CapturedScene;
	SkyLight->bLowerHemisphereIsBlack = false;
	SkyLight->LowerHemisphereColor = StageStudioRig::Backdrop;
	SkyLight->Intensity = 1.2f;

	KeyLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("KeyLight"));
	KeyLight->SetupAttachment(Root);
	KeyLight->SetMobility(EComponentMobility::Movable);
	KeyLight->SetRelativeRotation(StageStudioRig::KeyDirection);
	KeyLight->Intensity = 3.f;
	// A large source angle gives the soft shadow edge of a big studio softbox.
	KeyLight->LightSourceAngle = 12.f;
	KeyLight->CastShadows = true;
	// Never claim to be the sun: the outdoor sky owns the atmosphere sun slot.
	KeyLight->bAtmosphereSunLight = false;

	Backdrop = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("Backdrop"));
	Backdrop->SetupAttachment(Root);
	Backdrop->SetMobility(EComponentMobility::Movable);
	// Thin, and starting beyond the stage, so the stage stays clear and only the distance fades into the backdrop.
	Backdrop->FogDensity = 0.004f;
	Backdrop->FogHeightFalloff = 0.02f;
	Backdrop->StartDistance = 3000.f;
	Backdrop->FogMaxOpacity = 0.9f;
	Backdrop->FogInscatteringLuminance = StageStudioRig::Backdrop;
	Backdrop->bEnableVolumetricFog = true;
	Backdrop->VolumetricFogExtinctionScale = 0.4f;
}

void AStageStudioRig::BeginPlay()
{
	Super::BeginPlay();
	if (UWorld* World = GetWorld())
	{
		HiddenLevel.Hide(*World, *this);
	}
}

void AStageStudioRig::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// A world that is tearing down destroys the level's components too; showing them again would only touch dying objects.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		HiddenLevel.Restore();
	}
	else
	{
		HiddenLevel.Forget();
	}
	Super::EndPlay(EndPlayReason);
}

void AStageStudioRig::SetCubemap(UTextureCube* Cubemap)
{
	bUsesHdri = Cubemap != nullptr;
	if (Cubemap)
	{
		SkyLight->SourceType = SLS_SpecifiedCubemap;
		SkyLight->SetCubemap(Cubemap);
	}
	else
	{
		SkyLight->SourceType = SLS_CapturedScene;
		SkyLight->SetCubemap(nullptr);
	}
	// A movable sky light without real-time capture needs one explicit recapture after its source changes.
	SkyLight->RecaptureSky();
}

void AStageStudioRig::ApplyLook(const FStageEnvironmentLook& Look)
{
	KeyLight->SetIntensity(Look.StudioKeyIntensity);
	KeyLight->SetLightColor(Look.StudioKeyColor);
}
