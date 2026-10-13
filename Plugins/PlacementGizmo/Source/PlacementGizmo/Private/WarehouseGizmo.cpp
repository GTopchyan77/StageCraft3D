// Copyright DamenTools internal. Placement Gizmo plugin.

#include "WarehouseGizmo.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/StaticMeshComponent.h"

// Sets default values
AWarehouseGizmo::AWarehouseGizmo()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	EditRoot = CreateDefaultSubobject<USceneComponent>("EditRoot");
	checkf(nullptr != EditRoot, TEXT("The EditRoot is nullptr."));
	SetRootComponent(EditRoot);

	TranslateRoot = CreateDefaultSubobject<UStaticMeshComponent>("TranslateRoot");
	checkf(nullptr != TranslateRoot, TEXT("The TranslateRoot is nullptr."));
	TranslateRoot->SetupAttachment(EditRoot);

	ArrowX = CreateDefaultSubobject<UStaticMeshComponent>("ArrowX");
	checkf(nullptr != ArrowX, TEXT("The ArrowX is nullptr."));
	ArrowX->SetupAttachment(TranslateRoot);

	ArrowY = CreateDefaultSubobject<UStaticMeshComponent>("ArrowY");
	checkf(nullptr != ArrowY, TEXT("The ArrowY is nullptr."));
	ArrowY->SetupAttachment(TranslateRoot);

	ArrowZ = CreateDefaultSubobject<UStaticMeshComponent>("ArrowZ");
	checkf(nullptr != ArrowZ, TEXT("The ArrowZ is nullptr."));
	ArrowZ->SetupAttachment(TranslateRoot);

	ScalesRoot = CreateDefaultSubobject<USceneComponent>("ScaleRoot");
	checkf(nullptr != ScalesRoot, TEXT("The ScaleRoot is nullptr."));
	ScalesRoot->SetupAttachment(EditRoot);

	ScaleX = CreateDefaultSubobject<UStaticMeshComponent>("ScaleX");
	checkf(nullptr != ScaleX, TEXT("The ScaleX is nullptr."));
	ScaleX->SetupAttachment(ScalesRoot);

	ScaleY = CreateDefaultSubobject<UStaticMeshComponent>("ScaleY");
	checkf(nullptr != ScaleY, TEXT("The ScaleY is nullptr."));
	ScaleY->SetupAttachment(ScalesRoot);

	RotatorRoot = CreateDefaultSubobject<USceneComponent>("RotatorRoot");
	checkf(nullptr != RotatorRoot, TEXT("The RotatorRoot is nullptr."));
	RotatorRoot->SetupAttachment(EditRoot);

	RotatorX = CreateDefaultSubobject<UStaticMeshComponent>("RotatorX");
	checkf(nullptr != RotatorX, TEXT("The RotatorX is nullptr."));
	RotatorX->SetupAttachment(RotatorRoot);

	RotatorY = CreateDefaultSubobject<UStaticMeshComponent>("RotatorY");
	checkf(nullptr != RotatorY, TEXT("The RotatorY is nullptr."));
	RotatorY->SetupAttachment(RotatorRoot);

	RotatorZ = CreateDefaultSubobject<UStaticMeshComponent>("RotatorZ");
	checkf(nullptr != RotatorZ, TEXT("The RotatorZ is nullptr."));
	RotatorZ->SetupAttachment(RotatorRoot);
}

// Called when the game starts or when spawned
void AWarehouseGizmo::BeginPlay()
{
	Super::BeginPlay();

	EnableEditor(false);
}

// Called every frame
void AWarehouseGizmo::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UpdateState();
}

void AWarehouseGizmo::UpdateState()
{
	if (IsValid(ParentActor))
	{
		UpdateGizmo();
		DetectMouseInteraction();
		ProcessMouseInteraction();
	}
}

void AWarehouseGizmo::SetGizmoMode(EGizmoMode InGizmoMode)
{
	GizmoMode = InGizmoMode;
	if (IsValid(EditRoot) && IsValid(TranslateRoot) && IsValid(RotatorRoot) && IsValid(ScalesRoot))
	{
		EditRoot->SetVisibility(false, true);

		switch (InGizmoMode)
		{
			case EGizmoMode::GM_TRANSLATE:
				TranslateRoot->SetVisibility(true, true);
				break;
			case EGizmoMode::GM_ROTATE:
				RotatorRoot->SetVisibility(true, true);
				break;
			case EGizmoMode::GM_SCALE:
				ScalesRoot->SetVisibility(true, true);
				break;
		}
	}
}

EGizmoMode AWarehouseGizmo::GetGizmoMode()
{
	return GizmoMode;
}

void AWarehouseGizmo::EnableEditor(bool InEnable)
{
	SetActorHiddenInGame(!InEnable);
}

void AWarehouseGizmo::UpdateGizmo()
{
	UWorld* L_World = GetWorld();
	if (!IsValid(L_World) || !IsValid(ParentActor))
	{
		return;
	}

	APlayerCameraManager* L_PlayerCamerManager =  UGameplayStatics::GetPlayerCameraManager(L_World, 0);

	if (!IsValid(L_PlayerCamerManager))
	{
		return;
	}

	//The validity of the ParentActor is checked when the function is called

	switch (GizmoMode)
	{
		case EGizmoMode::GM_ROTATE:
			UpdateRotatorRoot(L_PlayerCamerManager);
			break;
		case EGizmoMode::GM_SCALE:
			if (IsValid(ScalesRoot))
			{
				FRotator L_ParentActorRotation = ParentActor->GetActorRotation();
				ScalesRoot->SetWorldRotation(L_ParentActorRotation);
			}
			break;
	}
}

bool AWarehouseGizmo::IsGizmoAttached()
{
	if (!IsValid(ParentActor))
	{
		return false;
	}

	if (AllowTagOnly.Num() != 0)
	{
		for (FName Tag : AllowTagOnly)
		{
			if (ParentActor->ActorHasTag(Tag))
			{
				return true;
			}
		}
		return false;
	}

	return true;
}

bool AWarehouseGizmo::AttachGizmo(AActor* InAttachTo)
{
	if (IsValid(InAttachTo))
	{
		bool IsValidAttachTo = false;
		if (AllowTagOnly.Num() != 0)
		{
			for (FName Tag : AllowTagOnly)
			{
				if (InAttachTo->ActorHasTag(Tag))
				{
					IsValidAttachTo = true;
					break;
				}
			}
		}
		else
		{
			IsValidAttachTo = true;
		}

		if (IsValidAttachTo)
		{
			ParentActor = InAttachTo;
			FVector L_ParentLocation = ParentActor->GetActorLocation();
			SetActorLocation(L_ParentLocation);
			EnableEditor(true);

			UE_LOG(LogTemp, Warning, TEXT("Gizmo attached to actor: %s at location %s"),
			 *ParentActor->GetName(), *L_ParentLocation.ToString());
			return true;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Gizmo NOT attached: Tag mismatch"));
			return false;
		}
	}

	// No valid actor to attach to (e.g. the selection was cleared) - detach
	// and hide instead of leaving a stale gizmo visible at the last location.
	ParentActor = nullptr;
	EnableEditor(false);
	UE_LOG(LogTemp, Warning, TEXT("Gizmo NOT attached: Invalid Actor"));
	return false;
}

void AWarehouseGizmo::DetectMouseInteraction()
{
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!IsValid(PC))
	{
		return;
	}

	if (PC->IsInputKeyDown(EKeys::LeftMouseButton))
	{
		bool IsInProgress = IsInteractionInProgress();

		if (!IsInProgress && !bWasUserInput)
		{
			bWasUserInput = true;
			SelectInteraction();
		}
	}
	else
	{
		if (bWasUserInput)
		{
			bWasUserInput = false;
			ClearInteraction(PC);
			if (bIsAnyInteraction)
			{
				FLatentActionInfo L_LatentInfo;
				UKismetSystemLibrary::DelayUntilNextTick(this, L_LatentInfo);
				SetMouseToNewLocationAfterDrag(PC);
				bIsAnyInteraction = false;
			}
		}
	}
}

void AWarehouseGizmo::UpdateRotatorRoot(APlayerCameraManager* InPlayerCamerManager)
{
	if (!IsValid(RotatorRoot))
	{
		return;
	}

	FVector L_Cameraloc = InPlayerCamerManager->GetCameraLocation();
	FVector L_ActorLocation = GetActorLocation();
	FVector L_Distance = UKismetMathLibrary::Normal(L_Cameraloc - L_ActorLocation, 0.0001);
	FRotator L_RotFromX = UKismetMathLibrary::MakeRotFromX(L_Distance);

	if (L_RotFromX.Yaw <= -90.f && L_RotFromX.Yaw >= -180.f)
	{
		RotatorOrientation = -180.f;
	}
	else if (L_RotFromX.Yaw <= 0.f && L_RotFromX.Yaw >= -90.f)
	{
		RotatorOrientation = -90.f;
	}
	else if (L_RotFromX.Yaw <= 90.f && L_RotFromX.Yaw > 0.f)
	{
		RotatorOrientation = 0.f;
	}
	else
	{
		RotatorOrientation = 90.f;
	}

	RotatorRoot->SetRelativeRotation(FRotator(0, RotatorOrientation, 0));
}

void AWarehouseGizmo::ProcessMouseInteraction()
{
	if (IsInteractionInProgress() && bIsAnyInteraction)
	{
		ProcessInteraction();
	}
}

void AWarehouseGizmo::ProcessInteraction()
{
	//The validity of the ParentActor is checked when the function is called in Tick

	// Adapted for DamenTools: placed objects are BP_PlaceableObject_Base_Class_C
	// (a custom Blueprint actor), not AStaticMeshActor as in the source project -
	// find the mesh component generically instead of casting to a specific actor class.
	UStaticMeshComponent* L_ActorMesh = ParentActor->FindComponentByClass<UStaticMeshComponent>();
	auto SavedMobility = EComponentMobility::Movable;

	if (IsValid(L_ActorMesh))
	{
		SavedMobility = L_ActorMesh->Mobility;
		L_ActorMesh->SetMobility(EComponentMobility::Movable);
	}

	ProcessGizmo();

	if (IsValid(L_ActorMesh))
	{
		L_ActorMesh->SetMobility(SavedMobility);
	}
}

bool AWarehouseGizmo::IsInteractionInProgress()
{
	if (GizmoMode == EGizmoMode::GM_TRANSLATE)
	{
		return TranslateDirection != EGizmoTranslateDirection::TRANSLATE_NONE;
	}
	else if (GizmoMode == EGizmoMode::GM_ROTATE)
	{
		return RotationMode != EGizmoRotationMode::ROTATE_NONE;
	}
	else if (GizmoMode == EGizmoMode::GM_SCALE)
	{
		return ScaleMode != EGizmoScaleMode::SCALE_NONE;
	}
	return false;
}

void AWarehouseGizmo::SetTranslateDirection(EGizmoTranslateDirection InTranslateDirection, APlayerController* InPC)
{
	if (TranslateDirection != InTranslateDirection)
	{
		TranslateDirection = InTranslateDirection;
		if (TranslateDirection == EGizmoTranslateDirection::TRANSLATE_NONE)
		{
			InPC->ResetIgnoreLookInput();
		}
		else
		{
			InPC->SetIgnoreLookInput(true);
		}
	}
}

void AWarehouseGizmo::SetRotationMode(EGizmoRotationMode InRotationMode, APlayerController* InPC)
{
	if (RotationMode != InRotationMode)
	{
		RotationMode = InRotationMode;
		if (RotationMode == EGizmoRotationMode::ROTATE_NONE)
		{
			InPC->ResetIgnoreLookInput();
		}
		else
		{
			InPC->SetIgnoreLookInput(true);
		}
	}
}

void AWarehouseGizmo::SetScaleMode(EGizmoScaleMode InScaleMode, APlayerController* InPC)
{
	if (ScaleMode != InScaleMode)
	{
		ScaleMode = InScaleMode;
		if (ScaleMode == EGizmoScaleMode::SCALE_NONE)
		{
			InPC->ResetIgnoreLookInput();
		}
		else
		{
			InPC->SetIgnoreLookInput(true);
		}
	}
}

void AWarehouseGizmo::ClearInteraction(APlayerController* InPC)
{
	//The validity of the PC is checked when the function is called

	switch (GizmoMode)
	{
		case EGizmoMode::GM_TRANSLATE:
			if (TranslateDirection != EGizmoTranslateDirection::TRANSLATE_NONE)
			{
				SetTranslateDirection(EGizmoTranslateDirection::TRANSLATE_NONE, InPC);
			}
			break;

		case EGizmoMode::GM_ROTATE:
			if (RotationMode != EGizmoRotationMode::ROTATE_NONE)
			{
				SetRotationMode(EGizmoRotationMode::ROTATE_NONE, InPC);
			}
			break;

		case EGizmoMode::GM_SCALE:
			if (ScaleMode != EGizmoScaleMode::SCALE_NONE)
			{
				SetScaleMode(EGizmoScaleMode::SCALE_NONE, InPC);
			}
			break;
	}
}

void AWarehouseGizmo::SetMouseToNewLocationAfterDrag(APlayerController* InPC)
{
	//The validity of the PC is checked when the function is called

	FTransform L_ActorTransform = GetActorTransform();
	FVector L_NewPosition = UKismetMathLibrary::TransformLocation(L_ActorTransform, HitLoactionSaved);
	FVector2D L_ScreenPosition;

	UGameplayStatics::ProjectWorldToScreen(InPC, L_NewPosition, L_ScreenPosition);
	FVector2D L_ViewportSize =  UWidgetLayoutLibrary::GetViewportSize(GetWorld());

	int32 L_MousePosX = UKismetMathLibrary::FTrunc(UKismetMathLibrary::FClamp(L_ScreenPosition.X, 0.0, L_ViewportSize.X));
	int32 L_MousePosY = UKismetMathLibrary::FTrunc(UKismetMathLibrary::FClamp(L_ScreenPosition.Y, 0.0, L_ViewportSize.Y));

	InPC->SetMouseLocation(L_MousePosX, L_MousePosY);
}

void AWarehouseGizmo::SetBuildManagerRef(UObject* InBuildManagerRef)
{
	BuildManagerRef = InBuildManagerRef;
}

FVector AWarehouseGizmo::GetTranslateAxisMask() const
{
	switch (TranslateDirection)
	{
		case EGizmoTranslateDirection::TRANSLATE_X:
			return FVector(1.f, 0.f, 0.f);
		case EGizmoTranslateDirection::TRANSLATE_Y:
			return FVector(0.f, 1.f, 0.f);
		case EGizmoTranslateDirection::TRANSLATE_Z:
			return FVector(0.f, 0.f, 1.f);
		default:
			return FVector::ZeroVector;
	}
}
