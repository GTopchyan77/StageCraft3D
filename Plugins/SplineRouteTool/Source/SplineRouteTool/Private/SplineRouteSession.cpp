// Copyright DamenTools internal. Spline Route Tool plugin.

#include "SplineRouteSession.h"
#include "SplineRouteActor.h"
#include "SplineRouteAuthoringSubsystem.h"
#include "SplineRouteGameModeBase.h"
#include "SplineRouteTool.h"
#include "GameFramework/GameModeBase.h"

// Engine cube/sphere primitives are 100u — same baseline ASplineRouteActor uses for HandleSize.
static constexpr float SplineRouteHandleMeshBaseline = 100.0f;

#include "Blueprint/UserWidget.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

ASplineRouteSession::ASplineRouteSession()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("SessionRoot"));
	SetRootComponent(Root);

	PreviewMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlacementPreview"));
	PreviewMesh->SetupAttachment(Root);
	PreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PreviewMesh->SetCanEverAffectNavigation(false);
	PreviewMesh->SetGenerateOverlapEvents(false);
	PreviewMesh->SetCastShadow(false);
	PreviewMesh->SetVisibility(false);
	PreviewMesh->SetMobility(EComponentMobility::Movable);
}

void ASplineRouteSession::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	AGameModeBase* ActiveGameMode = World ? World->GetAuthGameMode() : nullptr;
	if (!Cast<ASplineRouteGameModeBase>(ActiveGameMode))
	{
		UE_LOG(LogSplineRoute, Log,
			TEXT("ASplineRouteSession: active GameMode is %s, not a SplineRoute GameMode — session inactive."),
			ActiveGameMode ? *ActiveGameMode->GetClass()->GetName() : TEXT("(none)"));
		return;
	}

	if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
	{
		Sub->PlaneZ = AuthoringPlaneZ;
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRouteSession] No PlayerController in BeginPlay."));
		return;
	}
	BoundPC = PC;

	if (bManageCursor)
	{
		PC->bShowMouseCursor = true;
		PC->bEnableClickEvents = true;
		PC->bEnableMouseOverEvents = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
	}

	if (InputMappingContext)
	{
		if (ULocalPlayer* LP = PC->GetLocalPlayer())
		{
			if (UEnhancedInputLocalPlayerSubsystem* EIS = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
			{
				EIS->AddMappingContext(InputMappingContext, 0);
			}
		}
	}
	else
	{
		UE_LOG(LogSplineRoute, Warning, TEXT("[SplineRouteSession] InputMappingContext is null — no IMC pushed."));
	}

	SessionInputComponent = NewObject<UEnhancedInputComponent>(this, TEXT("SplineRouteSessionInput"));
	SessionInputComponent->RegisterComponent();
	SessionInputComponent->Priority = SessionInputPriority;

	int32 Bound = 0;
	if (PlaceAction)
	{
		SessionInputComponent->BindAction(PlaceAction, ETriggerEvent::Started, this, &ASplineRouteSession::OnPlacePressed);
		++Bound;
	}
	if (SelectAction)
	{
		SessionInputComponent->BindAction(SelectAction, ETriggerEvent::Started, this, &ASplineRouteSession::OnSelectPressed);
		++Bound;
	}
	if (ReleaseAction)
	{
		SessionInputComponent->BindAction(ReleaseAction, ETriggerEvent::Completed, this, &ASplineRouteSession::OnReleased);
		SessionInputComponent->BindAction(ReleaseAction, ETriggerEvent::Canceled, this, &ASplineRouteSession::OnReleased);
		++Bound;
	}
	if (DragAction)
	{
		SessionInputComponent->BindAction(DragAction, ETriggerEvent::Triggered, this, &ASplineRouteSession::OnDragTriggered);
		++Bound;
	}
	if (DeleteAction)
	{
		SessionInputComponent->BindAction(DeleteAction, ETriggerEvent::Started, this, &ASplineRouteSession::OnDeletePressed);
		++Bound;
	}

	PC->PushInputComponent(SessionInputComponent);

	if (HUDWidgetClass)
	{
		HUDWidgetInstance = CreateWidget<UUserWidget>(PC, HUDWidgetClass);
		if (HUDWidgetInstance)
		{
			HUDWidgetInstance->AddToViewport(0);
		}
	}

	if (PreviewMesh)
	{
		if (PreviewMeshAsset)
		{
			PreviewMesh->SetStaticMesh(PreviewMeshAsset);
		}
		if (PreviewMaterial)
		{
			PreviewMesh->SetMaterial(0, PreviewMaterial);
		}
		PreviewMID = PreviewMesh->CreateAndSetMaterialInstanceDynamic(0);
		PreviewMesh->SetWorldScale3D(FVector(PreviewScale));
		PreviewMesh->SetVisibility(false);
	}

	UE_LOG(LogSplineRoute, Log,
		TEXT("[SplineRouteSession] Initialized on %s — %d/5 actions bound, HUD=%s"),
		*PC->GetName(), Bound, HUDWidgetInstance ? *HUDWidgetInstance->GetName() : TEXT("(none)"));
}

void ASplineRouteSession::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdatePreviewGhost();
}

void ASplineRouteSession::UpdatePreviewGhost()
{
	if (!bShowPlacementPreview || !PreviewMesh)
	{
		return;
	}

	APlayerController* PC = BoundPC.Get();
	UWorld* World = GetWorld();
	USplineRouteAuthoringSubsystem* Sub = World ? World->GetSubsystem<USplineRouteAuthoringSubsystem>() : nullptr;
	if (!PC || !Sub)
	{
		PreviewMesh->SetVisibility(false);
		return;
	}

	if (Sub->IsCursorOverHUD(PC))
	{
		PreviewMesh->SetVisibility(false);
		return;
	}

	FVector PlanePoint;
	if (!Sub->ProjectMouseToPlane(PC, PlanePoint))
	{
		PreviewMesh->SetVisibility(false);
		return;
	}

	// Hide preview if cursor is over an existing handle — LMB is the right tool there, not F-place.
	FHitResult Hit;
	if (PC->GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_Visibility), true, Hit))
	{
		if (const ASplineRouteActor* HitRoute = Cast<ASplineRouteActor>(Hit.GetActor()))
		{
			if (HitRoute->FindControlPointForComponent(Hit.GetComponent()) != INDEX_NONE)
			{
				PreviewMesh->SetVisibility(false);
				return;
			}
		}
	}

	PreviewMesh->SetVisibility(true);
	PreviewMesh->SetWorldLocation(PlanePoint);

	// Track the active route's HandleSize so the ghost shows the exact size of the handle that will be placed.
	// Falls back to PreviewScale only when there's no active route to query.
	const ASplineRouteActor* Route = Sub->GetActiveRoute();
	float AppliedScale = 0.0f;
	if (Route)
	{
		AppliedScale = Route->HandleSize * PreviewScale / SplineRouteHandleMeshBaseline;
		PreviewMesh->SetWorldScale3D(FVector(AppliedScale));
	}
	else
	{
		AppliedScale = PreviewScale;
		PreviewMesh->SetWorldScale3D(FVector(AppliedScale));
	}
	// Throttled log so we can confirm at runtime whether the ghost is reading route HandleSize or falling back.
	static double LastLog = 0.0;
	const double Now = FPlatformTime::Seconds();
	if (Now - LastLog > 1.0)
	{
		LastLog = Now;
		UE_LOG(LogSplineRoute, Log,
			TEXT("[SplineRoute] PreviewGhost scale=%.3f source=%s routeHandle=%.2f previewMult=%.2f"),
			AppliedScale,
			Route ? TEXT("ROUTE") : TEXT("FALLBACK"),
			Route ? Route->HandleSize : -1.0f,
			PreviewScale);
	}

	if (PreviewMID)
	{
		const bool bAllowed = Sub->CanPlaceAt(PlanePoint);
		const FLinearColor Tint = bAllowed ? AllowedColor : BlockedColor;
		// Set both common parameter names so the preview works with any material the user assigns
		// (BaseColor is the convention the plugin's bundled preview material exposes).
		PreviewMID->SetVectorParameterValue(TEXT("BaseColor"), Tint);
		PreviewMID->SetVectorParameterValue(TEXT("Color"), Tint);
	}
}

void ASplineRouteSession::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (APlayerController* PC = BoundPC.Get())
	{
		if (SessionInputComponent)
		{
			PC->PopInputComponent(SessionInputComponent);
		}
		if (InputMappingContext)
		{
			if (ULocalPlayer* LP = PC->GetLocalPlayer())
			{
				if (UEnhancedInputLocalPlayerSubsystem* EIS = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
				{
					EIS->RemoveMappingContext(InputMappingContext);
				}
			}
		}
	}

	if (HUDWidgetInstance)
	{
		HUDWidgetInstance->RemoveFromParent();
		HUDWidgetInstance = nullptr;
	}

	if (SessionInputComponent)
	{
		SessionInputComponent->ClearActionBindings();
		SessionInputComponent = nullptr;
	}

	BoundPC.Reset();
	Super::EndPlay(EndPlayReason);
}

void ASplineRouteSession::OnPlacePressed(const FInputActionValue& /*Value*/)
{
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			Sub->HandlePlaceAtCursor(BoundPC.Get());
		}
	}
}

void ASplineRouteSession::OnSelectPressed(const FInputActionValue& /*Value*/)
{
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			Sub->HandleSelectAtCursor(BoundPC.Get());
		}
	}
}

void ASplineRouteSession::OnReleased(const FInputActionValue& /*Value*/)
{
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			Sub->HandleReleaseDrag();
		}
	}
}

void ASplineRouteSession::OnDragTriggered(const FInputActionValue& /*Value*/)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>();
	if (!Sub)
	{
		return;
	}

	APlayerController* PC = BoundPC.Get();

	// Move ONLY while the Left Shift modifier is held. Plain LMB-drag must not move waypoints —
	// mirrors ASplineRoutePlayerController::OnDragTriggered so both input paths agree, and closes
	// the race where IA_Drag (Triggered) fires on the same LMB press as IA_Select (Started) and
	// re-arms a drag on the point that was just selected.
	if (PC && PC->IsInputKeyDown(EKeys::LeftShift))
	{
		Sub->HandleDrag(PC);
	}
	else
	{
		// Modifier not held (or released mid-hold): stop any in-progress drag so the point freezes where it is.
		Sub->HandleReleaseDrag();
	}
}

void ASplineRouteSession::OnDeletePressed(const FInputActionValue& /*Value*/)
{
	if (UWorld* World = GetWorld())
	{
		if (USplineRouteAuthoringSubsystem* Sub = World->GetSubsystem<USplineRouteAuthoringSubsystem>())
		{
			Sub->HandleDeleteSelected();
		}
	}
}
