// Copyright DamenTools internal. Spline Route Tool plugin.

#include "DolphinCompanion.h"
#include "SplineRouteTimeSubsystem.h"
#include "SplineRouteTool.h"

#include "Animation/AnimSequenceBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

ADolphinCompanion::ADolphinCompanion()
{
	PrimaryActorTick.bCanEverTick = true;

	MeshComponent = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	SetRootComponent(MeshComponent);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetMobility(EComponentMobility::Movable);
	// Decoration only — never let it push the vessel around or feed the physics scene.
	MeshComponent->SetGenerateOverlapEvents(false);
}

void ADolphinCompanion::Configure(FVector InRelativeOffset, float InScale, float InBobAmplitude, float InBobSpeed, FRotator InMeshRotationOffset)
{
	BaseRelativeLocation = InRelativeOffset;
	BobAmplitude = FMath::Max(0.0f, InBobAmplitude);
	BobSpeed = FMath::Max(0.0f, InBobSpeed);
	MeshRotationOffset = InMeshRotationOffset;
	bConfigured = true;

	// The spawner passes this same scale in the FinishSpawning transform, so setting it here just makes
	// Configure() self-contained for Blueprint callers that configure an already-spawned companion.
	SetActorScale3D(FVector(FMath::Max(0.01f, InScale)));
}

void ADolphinCompanion::BeginPlay()
{
	Super::BeginPlay();

	if (!bConfigured && GetRootComponent())
	{
		// Level-placed instance: bob around wherever the designer parked it rather than snapping to origin.
		BaseRelativeLocation = GetRootComponent()->GetRelativeLocation();
	}

	if (MeshComponent)
	{
		// Applied here (not Configure()) so level-placed instances relying only on the UPROPERTY default
		// still get the fixup — matches ASplineFollowerVessel's habit of correcting the mesh, not the root.
		MeshComponent->SetRelativeRotation(MeshRotationOffset);
	}

	if (MeshComponent && SwimAnimation)
	{
		MeshComponent->PlayAnimation(SwimAnimation, /*bLooping=*/true);
	}
	else
	{
		UE_LOG(LogSplineRoute, Warning,
			TEXT("[SplineRoute] DolphinCompanion %s has no SwimAnimation assigned — it will render in its bind pose."),
			*GetName());
	}
}

void ADolphinCompanion::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (BobAmplitude <= 0.0f || BobSpeed <= 0.0f)
	{
		return;
	}

	// Advance on scaled time, not real time, so the bob slows and freezes with the rest of the sim when the
	// operator drags the global Time Scale slider — same convention as USplineFollowerComponent.
	BobTime += DeltaSeconds * GetEffectiveTimeScale();
	const float BobZ = BobAmplitude * FMath::Sin(2.0f * PI * BobSpeed * BobTime);
	SetActorRelativeLocation(BaseRelativeLocation + FVector(0.0f, 0.0f, BobZ));
}

float ADolphinCompanion::GetEffectiveTimeScale() const
{
	if (const UWorld* World = GetWorld())
	{
		if (const UGameInstance* GI = World->GetGameInstance())
		{
			if (const USplineRouteTimeSubsystem* TimeSys = GI->GetSubsystem<USplineRouteTimeSubsystem>())
			{
				return TimeSys->GetTimeScale();
			}
		}
	}
	return 1.0f;
}
