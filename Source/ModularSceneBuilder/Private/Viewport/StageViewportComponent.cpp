// Copyright Epic Games, Inc. All Rights Reserved.

#include "Viewport/StageViewportComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "ModularSceneBuilder.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/StageCameraPawn.h"
#include "Player/StageCraftGameViewportClient.h"
#include "Settings/StageCraftUserSettings.h"
#include "Viewport/StageGridActor.h"
#include "Viewport/StageLineViewCameraModifier.h"
#include "Viewport/StageViewMath.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageViewportComponent)

namespace StageViewport
{
	/** Fraction of the visible height between the view centre and the floor when a front or side view is entered from above. */
	constexpr double FloorLiftFraction = 0.3;

	/** Aspect used before the viewport has a size (headless runs). */
	constexpr double FallbackAspect = 16.0 / 9.0;

	bool IsVertical(EStageViewProjection Projection)
	{
		return StageViewMath::GetGridPlane(Projection) != StageViewMath::EGridPlane::XY;
	}
}

UStageViewportComponent::UStageViewportComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	GridActorClass = AStageGridActor::StaticClass();
}

void UStageViewportComponent::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* Controller = GetPlayerController();
	if (!Controller || !Controller->IsLocalController())
	{
		return; // A view belongs to a local player only.
	}

	if (GridActorClass)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = Controller;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		GridActor = GetWorld()->SpawnActor<AStageGridActor>(GridActorClass, FTransform::Identity, SpawnParams);
	}

	if (UStageSnappingComponent* SnappingComponent = Controller->FindComponentByClass<UStageSnappingComponent>())
	{
		Snapping = SnappingComponent;
		SnappingComponent->OnSnapSettingsChanged.AddUniqueDynamic(this, &ThisClass::HandleSnapSettingsChanged);
	}
	Controller->OnPossessedPawnChanged.AddUniqueDynamic(this, &ThisClass::HandlePossessedPawnChanged);

	RefreshGrid();
}

void UStageViewportComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UStageSnappingComponent* SnappingComponent = Snapping.Get())
	{
		SnappingComponent->OnSnapSettingsChanged.RemoveDynamic(this, &ThisClass::HandleSnapSettingsChanged);
	}
	Snapping.Reset();

	if (APlayerController* Controller = GetPlayerController())
	{
		Controller->OnPossessedPawnChanged.RemoveDynamic(this, &ThisClass::HandlePossessedPawnChanged);
	}

	// The game viewport client outlives this world (level travel, PIE restart); hand it back lit.
	if (ViewMode != EStageViewMode::Lit)
	{
		if (UStageCraftGameViewportClient* ViewportClient = GetStageViewportClient())
		{
			ViewportClient->ApplyStageViewMode(EStageViewMode::Lit);
		}
		ViewMode = EStageViewMode::Lit;
	}

	if (GridActor)
	{
		GridActor->Destroy();
		GridActor = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

APlayerController* UStageViewportComponent::GetPlayerController() const
{
	return Cast<APlayerController>(GetOwner());
}

AStageCameraPawn* UStageViewportComponent::GetCameraPawn() const
{
	const APlayerController* Controller = GetPlayerController();
	return Controller ? Cast<AStageCameraPawn>(Controller->GetPawn()) : nullptr;
}

UStageCraftGameViewportClient* UStageViewportComponent::GetStageViewportClient() const
{
	const UWorld* World = GetWorld();
	return World ? Cast<UStageCraftGameViewportClient>(World->GetGameViewport()) : nullptr;
}

bool UStageViewportComponent::IsOrthographic() const
{
	return StageViewMath::IsOrthographic(Projection);
}

// --- Projection ---

bool UStageViewportComponent::SetProjection(EStageViewProjection NewProjection)
{
	if (NewProjection == Projection)
	{
		return true;
	}

	APlayerController* Controller = GetPlayerController();
	AStageCameraPawn* Pawn = GetCameraPawn();
	if (!Controller || !Controller->PlayerCameraManager || !Pawn)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: projection %s refused: no camera manager or StageCraft camera pawn."),
			*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(NewProjection));
		return false;
	}

	const EStageViewProjection PreviousProjection = Projection;
	if (!StageViewMath::IsOrthographic(NewProjection))
	{
		// Back to the camera the user left, exactly.
		Projection = NewProjection;
		ApplyCameraProjection();
		const FTransform Restore = SavedPerspectiveView.Get(FTransform(Controller->GetControlRotation(), Pawn->GetActorLocation()));
		Pawn->ExitFixedView(Restore.Rotator());
		Pawn->TeleportView(Restore.GetLocation());
		SavedPerspectiveView.Reset();
	}
	else
	{
		FVector Focus;
		if (PreviousProjection == EStageViewProjection::Perspective)
		{
			// Frame what the perspective view centred on, at the same scale there.
			const FVector ViewLocation = Pawn->GetActorLocation();
			const FRotator ViewRotation = Controller->GetControlRotation();
			SavedPerspectiveView = FTransform(ViewRotation, ViewLocation);
			Focus = FindPerspectiveFocus(ViewLocation, ViewRotation);
			OrthoWidth = StageViewMath::FitOrthoWidth(FVector::Dist(ViewLocation, Focus), Controller->PlayerCameraManager->GetFOVAngle());
		}
		else
		{
			Focus = GetOrthoFocus();
		}

		// Seen from above, the centre is on the floor; a front or side view would then show half of it below ground. The
		// visible height is OrthoWidth or OrthoWidth / aspect depending on the player's aspect constraint (and the view cannot
		// be measured until it renders), so lift by the smaller: the floor then stays on screen either way.
		if (StageViewport::IsVertical(NewProjection) && !StageViewport::IsVertical(PreviousProjection))
		{
			FVector2D Size;
			const double Aspect = GetViewportSize(Size) ? Size.X / Size.Y : StageViewport::FallbackAspect;
			Focus.Z += FMath::Min(OrthoWidth, OrthoWidth / Aspect) * StageViewport::FloorLiftFraction;
		}

		Projection = NewProjection;
		ApplyOrthographic(Focus);
	}

	RequestCameraCut();
	RefreshGrid();
	UE_LOG(LogStageCraft, Log, TEXT("Viewport projection %s -> %s (ortho width %.0f cm)."), *UEnum::GetValueAsString(PreviousProjection),
		*UEnum::GetValueAsString(Projection), OrthoWidth);
	OnViewChanged.Broadcast();
	return true;
}

FVector UStageViewportComponent::FindPerspectiveFocus(const FVector& ViewLocation, const FRotator& ViewRotation) const
{
	// The floor plane, not a world trace: deterministic, and the stage floor is where a plan view should centre.
	const FVector Direction = ViewRotation.Vector();
	if (Direction.Z < -UE_KINDA_SMALL_NUMBER && ViewLocation.Z > 0.0)
	{
		const double Distance = -ViewLocation.Z / Direction.Z;
		if (Distance <= StageViewMath::MaxOrthoWidth * 2.0)
		{
			return ViewLocation + Direction * Distance;
		}
	}
	return ViewLocation + Direction * FallbackFocusDistance;
}

FVector UStageViewportComponent::GetOrthoFocus() const
{
	const AStageCameraPawn* Pawn = GetCameraPawn();
	return Pawn ? Pawn->GetActorLocation() + StageViewMath::GetProjectionRotation(Projection).Vector() * OrthoStandoff : FVector::ZeroVector;
}

void UStageViewportComponent::ApplyOrthographic(const FVector& Focus)
{
	AStageCameraPawn* Pawn = GetCameraPawn();
	if (!Pawn)
	{
		return;
	}
	const FRotator Rotation = StageViewMath::GetProjectionRotation(Projection);
	Pawn->EnterFixedView(Rotation);
	Pawn->TeleportView(Focus - Rotation.Vector() * OrthoStandoff);
	ApplyCameraProjection();
}

void UStageViewportComponent::ApplyCameraProjection()
{
	APlayerController* Controller = GetPlayerController();
	APlayerCameraManager* CameraManager = Controller ? Controller->PlayerCameraManager.Get() : nullptr;
	if (!CameraManager)
	{
		return;
	}
	CameraManager->bIsOrthographic = IsOrthographic();
	CameraManager->DefaultOrthoWidth = static_cast<float>(OrthoWidth);

	// The controller passes the view rotation through ProcessViewRotation every tick, which limits pitch to +-89.9 by
	// default: Top would look down 0.1 degree off vertical. The pawn limits pitch itself in Perspective.
	if (IsOrthographic())
	{
		if (!SavedPitchLimits.IsSet())
		{
			SavedPitchLimits = FVector2D(CameraManager->ViewPitchMin, CameraManager->ViewPitchMax);
		}
		CameraManager->ViewPitchMin = -90.f;
		CameraManager->ViewPitchMax = 90.f;
	}
	else if (SavedPitchLimits.IsSet())
	{
		CameraManager->ViewPitchMin = static_cast<float>(SavedPitchLimits->X);
		CameraManager->ViewPitchMax = static_cast<float>(SavedPitchLimits->Y);
		SavedPitchLimits.Reset();
	}
	// Explicit planes (near 0, far at world max) with the camera OrthoStandoff back: the auto planes would move the view
	// origin towards the pawn, which is not a target here (ADR F5).
	CameraManager->bAutoCalculateOrthoPlanes = false;
	CameraManager->bUpdateOrthoPlanes = false;
}

void UStageViewportComponent::HandlePossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn)
{
	// A new pawn starts free; hold it to the current axis view.
	if (IsOrthographic())
	{
		if (AStageCameraPawn* Pawn = Cast<AStageCameraPawn>(NewPawn))
		{
			Pawn->EnterFixedView(StageViewMath::GetProjectionRotation(Projection));
		}
	}
}

// --- Orthographic navigation ---

bool UStageViewportComponent::GetViewportSize(FVector2D& OutSize) const
{
	const APlayerController* Controller = GetPlayerController();
	int32 SizeX = 0;
	int32 SizeY = 0;
	if (Controller)
	{
		Controller->GetViewportSize(SizeX, SizeY);
	}
	OutSize = FVector2D(SizeX, SizeY);
	return SizeX > 0 && SizeY > 0;
}

bool UStageViewportComponent::MeasureWorldPerPixel(FVector2D& OutWorldPerPixel) const
{
	const APlayerController* Controller = GetPlayerController();
	FVector2D Size;
	if (!Controller || !GetViewportSize(Size))
	{
		return false;
	}

	// An orthographic deprojection is linear: one pixel right and one pixel down from the centre give the pixel size.
	const FVector2D Center = Size * 0.5;
	FVector Origin, Right, Down, Direction;
	if (!Controller->DeprojectScreenPositionToWorld(Center.X, Center.Y, Origin, Direction)
		|| !Controller->DeprojectScreenPositionToWorld(Center.X + 1.0, Center.Y, Right, Direction)
		|| !Controller->DeprojectScreenPositionToWorld(Center.X, Center.Y + 1.0, Down, Direction))
	{
		return false;
	}
	OutWorldPerPixel = FVector2D(FVector::Dist(Origin, Right), FVector::Dist(Origin, Down));
	return OutWorldPerPixel.X > 0.0 && OutWorldPerPixel.Y > 0.0;
}

bool UStageViewportComponent::ZoomOrtho(float Steps, const FVector2D& CursorPosition)
{
	FVector2D Size;
	FVector2D WorldPerPixel;
	if (!IsOrthographic() || !GetViewportSize(Size) || !MeasureWorldPerPixel(WorldPerPixel))
	{
		return false;
	}

	const double NewWidth = StageViewMath::StepOrthoWidth(OrthoWidth, Steps);
	if (NewWidth == OrthoWidth)
	{
		return true; // At a zoom limit.
	}

	// The cursor's offset from the centre in world units (right, up), measured on the current view.
	const FVector2D CursorOffset((CursorPosition.X - Size.X * 0.5) * WorldPerPixel.X, (Size.Y * 0.5 - CursorPosition.Y) * WorldPerPixel.Y);
	const FVector2D Shift = StageViewMath::ComputeOrthoZoomShift(CursorOffset, OrthoWidth, NewWidth);
	OrthoWidth = NewWidth;
	ShiftOrthoView(Shift);
	ApplyCameraProjection();
	OnViewChanged.Broadcast();
	return true;
}

bool UStageViewportComponent::PanOrtho(const FVector2D& MouseDelta)
{
	FVector2D WorldPerPixel;
	if (!IsOrthographic() || !MeasureWorldPerPixel(WorldPerPixel))
	{
		return false;
	}
	// Not broadcast: a pan fires per mouse move and changes nothing the toolbar shows. The camera's own transform
	// notification already refreshes the placement preview.
	ShiftOrthoView(StageViewMath::ComputePanShift(WorldPerPixel, MouseDelta));
	return true;
}

void UStageViewportComponent::ShiftOrthoView(const FVector2D& Shift)
{
	AStageCameraPawn* Pawn = GetCameraPawn();
	if (!Pawn || Shift.IsNearlyZero())
	{
		return;
	}
	const FRotationMatrix Axes(StageViewMath::GetProjectionRotation(Projection));
	Pawn->TeleportView(Pawn->GetActorLocation() + Axes.GetUnitAxis(EAxis::Y) * Shift.X + Axes.GetUnitAxis(EAxis::Z) * Shift.Y);
}

// --- View mode ---

bool UStageViewportComponent::SetViewMode(EStageViewMode NewMode)
{
	if (NewMode == ViewMode)
	{
		return true;
	}

	UStageCraftGameViewportClient* ViewportClient = GetStageViewportClient();
	if (!ViewportClient)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: view mode %s refused: GameViewportClientClassName is not StageCraftGameViewportClient."),
			*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(NewMode));
		return false;
	}

	UStageLineViewCameraModifier* Modifier = NewMode == EStageViewMode::Lines ? GetOrCreateLineModifier() : LineModifier.Get();
	if (NewMode == EStageViewMode::Lines && !Modifier)
	{
		return false; // Reported by GetOrCreateLineModifier.
	}

	ViewportClient->ApplyStageViewMode(NewMode);
	if (Modifier)
	{
		if (NewMode == EStageViewMode::Lines)
		{
			Modifier->EnableModifier();
		}
		else
		{
			Modifier->DisableModifier(/*bImmediate*/ true);
		}
	}

	RequestCameraCut();
	ViewMode = NewMode;
	OnViewChanged.Broadcast();
	return true;
}

UStageLineViewCameraModifier* UStageViewportComponent::GetOrCreateLineModifier()
{
	if (UStageLineViewCameraModifier* Existing = LineModifier.Get())
	{
		return Existing;
	}

	APlayerController* Controller = GetPlayerController();
	APlayerCameraManager* CameraManager = Controller ? Controller->PlayerCameraManager.Get() : nullptr;
	// One small material, loaded on the user's first switch to Lines rather than for every session.
	UMaterialInterface* Material = LineViewMaterial.LoadSynchronous();
	if (!CameraManager || !Material)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: the line view is unavailable: %s."), *GetNameSafe(GetOwner()),
			!CameraManager ? TEXT("no camera manager") : *FString::Printf(TEXT("material '%s' did not load (DefaultGame.ini LineViewMaterial)"), *LineViewMaterial.ToString()));
		return nullptr;
	}

	UStageLineViewCameraModifier* Modifier = Cast<UStageLineViewCameraModifier>(CameraManager->AddNewCameraModifier(UStageLineViewCameraModifier::StaticClass()));
	if (Modifier)
	{
		Modifier->SetMaterial(Material);
		Modifier->DisableModifier(/*bImmediate*/ true);
	}
	LineModifier = Modifier;
	return Modifier;
}

// --- Grid ---

bool UStageViewportComponent::IsGridVisible() const
{
	const UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	return !Settings || Settings->IsViewportGridVisible();
}

void UStageViewportComponent::SetGridVisible(bool bVisible)
{
	UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	if (!Settings)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("%s: GameUserSettingsClassName is not StageCraftUserSettings; the grid preference cannot be stored."), *GetNameSafe(GetOwner()));
		return;
	}
	if (Settings->SetViewportGridVisible(bVisible))
	{
		Settings->SaveSettings();
		RefreshGrid();
		OnViewChanged.Broadcast();
	}
}

void UStageViewportComponent::HandleSnapSettingsChanged()
{
	RefreshGrid();
}

void UStageViewportComponent::RefreshGrid()
{
	if (!GridActor)
	{
		return;
	}
	if (!IsGridVisible())
	{
		GridActor->HideGrid();
		return;
	}
	const UStageSnappingComponent* SnappingComponent = Snapping.Get();
	const double GridSnap = SnappingComponent ? SnappingComponent->GetGridSnap() : 0.0;
	const StageViewMath::EGridPlane Plane = StageViewMath::GetGridPlane(Projection);
	const double Offset = StageViewMath::GetGridPlaneOffset(Projection, GridBackDistance) + (Plane == StageViewMath::EGridPlane::XY ? GridFloorClearance : 0.0);
	GridActor->ShowGrid(Plane, Offset, GridSnap > 0.0 ? GridSnap : UnsnappedGridCell);
}

void UStageViewportComponent::RequestCameraCut() const
{
	// A projection or view-mode switch is a cut: temporal AA history from the old view would ghost into the new one, and
	// without a cut auto exposure climbs back over seconds after the Lines mode, which runs with eye adaptation off (the
	// renderer forces the exposure target on a cut, PostProcessEyeAdaptation.cpp).
	const APlayerController* Controller = GetPlayerController();
	if (Controller && Controller->PlayerCameraManager)
	{
		Controller->PlayerCameraManager->SetGameCameraCutThisFrame();
	}
}
