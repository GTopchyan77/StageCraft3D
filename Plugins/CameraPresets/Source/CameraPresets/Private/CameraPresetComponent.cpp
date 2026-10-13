// Copyright DamenTools internal. Camera Presets plugin.

#include "CameraPresetComponent.h"
#include "CameraPresets.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

UCameraPresetComponent::UCameraPresetComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UCameraPresetComponent::BeginPlay()
{
	Super::BeginPlay();

	if (AActor* Owner = GetOwner())
	{
		CachedSpringArm = Owner->FindComponentByClass<USpringArmComponent>();
	}

	if (!CachedSpringArm)
	{
		UE_LOG(LogCameraPresets, Error, TEXT("UCameraPresetComponent: owner '%s' has no SpringArmComponent - presets will not function."),
			GetOwner() ? *GetOwner()->GetName() : TEXT("<none>"));
	}
}

bool UCameraPresetComponent::ComputeGridFraming(UObject* BuildManagerRef, FVector& OutPivot, float& OutArmLength, float* OutMaxExtent) const
{
	const AActor* Owner = GetOwner();
	OutPivot = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
	OutArmLength = FallbackArmLength;
	if (OutMaxExtent)
	{
		*OutMaxExtent = FallbackArmLength;
	}

	if (!IsValid(BuildManagerRef))
	{
		UE_LOG(LogCameraPresets, Warning, TEXT("ComputeGridFraming: BuildManagerRef is null/invalid, using fallback framing."));
		return false;
	}

	UClass* Class = BuildManagerRef->GetClass();

	// See header comment: these two names/types must match BP_BuildManager exactly.
	const FStructProperty* GridSizeProp = FindFProperty<FStructProperty>(Class, TEXT("GridSize"));
	const FFloatProperty* CellSizeFloatProp = FindFProperty<FFloatProperty>(Class, TEXT("GridCellSizeXY"));
	const FDoubleProperty* CellSizeDoubleProp = CellSizeFloatProp ? nullptr : FindFProperty<FDoubleProperty>(Class, TEXT("GridCellSizeXY"));

	if (!GridSizeProp || (!CellSizeFloatProp && !CellSizeDoubleProp))
	{
		UE_LOG(LogCameraPresets, Error,
			TEXT("ComputeGridFraming: BuildManagerRef (class %s) is missing an expected property - GridSize(IntPoint): %s, GridCellSizeXY(float/double): %s. Using fallback framing."),
			*Class->GetName(),
			GridSizeProp ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"),
			(CellSizeFloatProp || CellSizeDoubleProp) ? TEXT("OK") : TEXT("MISSING/WRONG TYPE"));
		return false;
	}

	const FIntPoint GridSize = *GridSizeProp->ContainerPtrToValuePtr<FIntPoint>(BuildManagerRef);
	const float CellSize = CellSizeFloatProp
		? CellSizeFloatProp->GetPropertyValue_InContainer(BuildManagerRef)
		: static_cast<float>(CellSizeDoubleProp->GetPropertyValue_InContainer(BuildManagerRef));

	const float GridWorldWidth = GridSize.X * CellSize;
	const float GridWorldDepth = GridSize.Y * CellSize;
	const float MaxExtent = FMath::Max(GridWorldWidth, GridWorldDepth);

	const AActor* BuildManagerActor = Cast<AActor>(BuildManagerRef);
	const FVector GridOrigin = BuildManagerActor ? BuildManagerActor->GetActorLocation() : FVector::ZeroVector;
	OutPivot = GridOrigin + FVector(GridWorldWidth * 0.5f, GridWorldDepth * 0.5f, 0.f);

	float FovDegrees = 90.f;
	if (const UCameraComponent* Camera = Owner ? Owner->FindComponentByClass<UCameraComponent>() : nullptr)
	{
		FovDegrees = Camera->FieldOfView;
	}
	const float HalfFovRad = FMath::DegreesToRadians(FovDegrees * 0.5f);
	OutArmLength = (MaxExtent * 0.5f * PaddingFactor) / FMath::Tan(HalfFovRad);

	return true;
}

FRotator UCameraPresetComponent::GetPresetRotation(ECameraPresetView Preset)
{
	switch (Preset)
	{
	case ECameraPresetView::Top:       return FRotator(-90.f, 90.f, 0.f);
	case ECameraPresetView::Front:     return FRotator(-20.f, 0.f, 0.f);
	case ECameraPresetView::Side:      return FRotator(-20.f, 90.f, 0.f);
	case ECameraPresetView::Isometric: return FRotator(-45.f, 45.f, 0.f);
	default:                           return FRotator::ZeroRotator;
	}
}

void UCameraPresetComponent::SwitchToPreset(UObject* BuildManagerRef, ECameraPresetView Preset, float BlendTime, bool& bEnteredPresetView, bool& bExitedToFreeCam)
{
	bEnteredPresetView = false;
	bExitedToFreeCam = false;

	if (!CachedSpringArm)
	{
		if (AActor* Owner = GetOwner())
		{
			CachedSpringArm = Owner->FindComponentByClass<USpringArmComponent>();
		}
	}

	if (!CachedSpringArm || Preset == ECameraPresetView::None || !GetOwner())
	{
		UE_LOG(LogCameraPresets, Error, TEXT("SwitchToPreset: no SpringArmComponent on owner, or Preset==None. No-op."));
		return;
	}

	const float Alpha = bBlending ? FMath::Clamp(BlendElapsed / BlendDuration, 0.f, 1.f) : 1.f;
	const float EasedAlpha = FMath::InterpEaseInOut(0.f, 1.f, Alpha, 2.f);

	// Current actual (or in-flight-blend) transform becomes the new blend's start point.
	const FVector CurrentPivot = bBlending ? FMath::Lerp(BlendStartPivot, BlendTargetPivot, EasedAlpha) : GetOwner()->GetActorLocation();
	const FQuat CurrentArmRotation = bBlending ? FQuat::Slerp(BlendStartArmRotation, BlendTargetArmRotation, EasedAlpha) : CachedSpringArm->GetComponentRotation().Quaternion();
	const float CurrentArmLength = bBlending ? FMath::Lerp(BlendStartArmLength, BlendTargetArmLength, EasedAlpha) : CachedSpringArm->TargetArmLength;

	FVector TargetPivot;
	FRotator TargetArmRotation;
	float TargetArmLength;

	if (Preset == CurrentPreset)
	{
		// Re-pressing the active preset's key: exit back to free-cam.
		bExitedToFreeCam = true;
		TargetPivot = bHasCachedFreeCam ? CachedFreeCamPivot : CurrentPivot;
		TargetArmRotation = (bHasCachedFreeCam ? CachedFreeCamArmRotation : CurrentArmRotation).Rotator();
		TargetArmLength = bHasCachedFreeCam ? CachedFreeCamArmLength : CurrentArmLength;
		CurrentPreset = ECameraPresetView::None;
		bHasCachedFreeCam = false;
	}
	else
	{
		if (CurrentPreset == ECameraPresetView::None)
		{
			// Entering preset view from free-cam: cache it to restore later.
			bEnteredPresetView = true;
			CachedFreeCamPivot = CurrentPivot;
			CachedFreeCamArmRotation = CurrentArmRotation;
			CachedFreeCamArmLength = CurrentArmLength;
			bHasCachedFreeCam = true;
		}
		// else: already in a different preset - switch directly, no restore, no flags set.

		FVector GridPivot;
		float FramingArmLength;
		ComputeGridFraming(BuildManagerRef, GridPivot, FramingArmLength);

		TargetPivot = GridPivot;
		TargetArmRotation = GetPresetRotation(Preset);
		TargetArmLength = FramingArmLength;
		CurrentPreset = Preset;
	}

	BlendStartPivot = CurrentPivot;
	BlendStartArmRotation = CurrentArmRotation;
	BlendStartArmLength = CurrentArmLength;

	BlendTargetPivot = TargetPivot;
	BlendTargetArmRotation = TargetArmRotation.Quaternion();
	BlendTargetArmLength = TargetArmLength;

	BlendDuration = FMath::Max(BlendTime, KINDA_SMALL_NUMBER);
	BlendElapsed = 0.f;
	bBlending = true;
}

void UCameraPresetComponent::ToggleOrthographic(UObject* BuildManagerRef, bool bEnable)
{
	if (!CachedCamera)
	{
		if (AActor* Owner = GetOwner())
		{
			CachedCamera = Owner->FindComponentByClass<UCameraComponent>();
		}
	}

	if (!CachedCamera)
	{
		UE_LOG(LogCameraPresets, Error, TEXT("ToggleOrthographic: owner '%s' has no CameraComponent - no-op."),
			GetOwner() ? *GetOwner()->GetName() : TEXT("<none>"));
		return;
	}

	if (!bEnable)
	{
		CachedCamera->ProjectionMode = ECameraProjectionMode::Perspective;
		return;
	}

	FVector GridPivot;
	float FramingArmLength;
	float MaxExtent;
	ComputeGridFraming(BuildManagerRef, GridPivot, FramingArmLength, &MaxExtent);

	CachedCamera->ProjectionMode = ECameraProjectionMode::Orthographic;
	CachedCamera->OrthoWidth = FMath::Max(MaxExtent * PaddingFactor, 1.f);
}

bool UCameraPresetComponent::IsOrthographic() const
{
	return CachedCamera && CachedCamera->ProjectionMode == ECameraProjectionMode::Orthographic;
}

void UCameraPresetComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bBlending || !CachedSpringArm || !GetOwner())
	{
		return;
	}

	BlendElapsed += DeltaTime;
	const float Alpha = FMath::Clamp(BlendElapsed / BlendDuration, 0.f, 1.f);
	const float EasedAlpha = FMath::InterpEaseInOut(0.f, 1.f, Alpha, 2.f);

	const FVector NewPivot = FMath::Lerp(BlendStartPivot, BlendTargetPivot, EasedAlpha);
	const FQuat NewArmRotation = FQuat::Slerp(BlendStartArmRotation, BlendTargetArmRotation, EasedAlpha);
	const float NewArmLength = FMath::Lerp(BlendStartArmLength, BlendTargetArmLength, EasedAlpha);

	GetOwner()->SetActorLocation(NewPivot);
	CachedSpringArm->SetWorldRotation(NewArmRotation);
	CachedSpringArm->TargetArmLength = NewArmLength;

	if (Alpha >= 1.f)
	{
		bBlending = false;
	}
}
