// Copyright DamenTools internal. Placement Gizmo plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/EngineTypes.h"
#include "WarehouseGizmo.generated.h"

UENUM(BlueprintType)
enum class EGizmoMode:uint8
{
	GM_TRANSLATE  UMETA(DisplayName = "Gizmo Mode Translate")
	, GM_ROTATE  UMETA(DisplayName = "Gizmo Mode Rotate")
	, GM_SCALE  UMETA(DisplayName = "Gizmo Mode Scale")
};

UENUM(BlueprintType)
enum class EGizmoRotationMode:uint8
{
	ROTATE_NONE  UMETA(DisplayName = "Rotate None")
	, ROTATE_ROLL  UMETA(DisplayName = "Rotate Roll")
	, ROTATE_PITCH  UMETA(DisplayName = "Rotate Pitch")
	, ROTATE_YAW  UMETA(DisplayName = "Rotate Yaw")
};

UENUM(BlueprintType)
enum class EGizmoScaleMode:uint8
{
	SCALE_NONE  UMETA(DisplayName = "Scale None")
	, SCALE_X  UMETA(DisplayName = "Scale X")
	, SCALE_Y  UMETA(DisplayName = "Scale Y")
};

UENUM(BlueprintType)
enum class EGizmoTranslateDirection:uint8
{
	TRANSLATE_NONE  UMETA(DisplayName = "Translate None")
	, TRANSLATE_X  UMETA(DisplayName = "Translate X")
	, TRANSLATE_Y  UMETA(DisplayName = "Translate Y")
	, TRANSLATE_Z  UMETA(DisplayName = "Translate Z")
};


UCLASS()
class PLACEMENTGIZMO_API AWarehouseGizmo : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AWarehouseGizmo();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class USceneComponent> EditRoot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> TranslateRoot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> ArrowX = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> ArrowY = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> ArrowZ = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class USceneComponent> ScalesRoot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> ScaleX = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> ScaleY = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class USceneComponent> RotatorRoot = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> RotatorX = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> RotatorY = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, meta = (AllowPrivateAccess = true))
	TObjectPtr<class UStaticMeshComponent> RotatorZ = nullptr;

public:
	bool IsGizmoAttached();

	void UpdateRotatorRoot(APlayerCameraManager* InPlayerCamerManager);

private:
	void EnableEditor(bool InEnable);

	void UpdateGizmo();

	bool IsInteractionInProgress();

	void ClearInteraction(APlayerController* InPC);

	void SetMouseToNewLocationAfterDrag(APlayerController* InPC);

	void ProcessInteraction();

	void UpdateState();

	void DetectMouseInteraction();

	void ProcessMouseInteraction();

public:
	UFUNCTION(BlueprintCallable)
	void SetGizmoMode(EGizmoMode InGizmoMode);

	UFUNCTION(BlueprintCallable)
	EGizmoMode GetGizmoMode();

	UFUNCTION(BlueprintCallable)
	bool AttachGizmo(AActor* InAttachTo);

	UFUNCTION(BlueprintCallable)
	void SetTranslateDirection(EGizmoTranslateDirection InTranslateDirection, APlayerController* InPC);

	UFUNCTION(BlueprintCallable)
	void SetRotationMode(EGizmoRotationMode InRotationMode, APlayerController* InPC);

	UFUNCTION(BlueprintCallable)
	void SetScaleMode(EGizmoScaleMode InScaleMode, APlayerController* InPC);

	UFUNCTION(BlueprintImplementableEvent)
	void SelectInteraction();

	UFUNCTION(BlueprintImplementableEvent)
	void ProcessGizmo();

public:
	UPROPERTY(BlueprintReadWrite)
	EGizmoTranslateDirection TranslateDirection = EGizmoTranslateDirection::TRANSLATE_NONE;

	UPROPERTY(BlueprintReadWrite)
	EGizmoRotationMode RotationMode = EGizmoRotationMode::ROTATE_NONE;

	UPROPERTY(BlueprintReadWrite)
	EGizmoScaleMode ScaleMode = EGizmoScaleMode::SCALE_NONE;

	EGizmoMode GizmoMode = EGizmoMode::GM_TRANSLATE;

	UPROPERTY(BlueprintReadWrite)
	float RotatorOrientation = 0.f;

	UPROPERTY(BlueprintReadWrite)
	bool bWasUserInput = false;

	FHitResult HitResult;

	UPROPERTY(BlueprintReadWrite)
	bool bIsAnyInteraction = false;

	UPROPERTY(BlueprintReadWrite)
	FVector HitLoactionSaved = FVector::ZeroVector;

public:
	UPROPERTY(BlueprintReadOnly)
	TObjectPtr<class AActor> ParentActor = nullptr;

	UPROPERTY(EditAnywhere)
	TSubclassOf<class AActor> ParentActorSubClass = nullptr;

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	TArray<FName> AllowTagOnly;

	/**
	 * Loosely-typed reference to the BP_BuildManager_C instance, set once by
	 * PC_Demo_PlayerCntroller when it spawns this gizmo. BP_BuildManager has no
	 * native C++ base (same constraint documented on PreciseTransformLibrary),
	 * so this can only be a generic UObject* - the Blueprint implementation of
	 * ProcessGizmo() passes it straight into PreciseTransformLibrary's
	 * reflection-based UFUNCTIONs (e.g. RepositionPlacedObject) rather than
	 * this plugin reimplementing any grid-aware logic itself.
	 */
	UPROPERTY(BlueprintReadWrite)
	TObjectPtr<UObject> BuildManagerRef = nullptr;

	UFUNCTION(BlueprintCallable)
	void SetBuildManagerRef(UObject* InBuildManagerRef);

	/**
	 * Maps TranslateDirection to a unit axis mask (1,0,0 / 0,1,0 / 0,0,1 /
	 * zero for NONE), so the Blueprint-side drag math in ProcessGizmo() can
	 * component-wise blend the cursor-tracked location with the object's
	 * current location without needing its own enum-comparison logic - the
	 * actual drag math (which is what varies per-project) stays in Blueprint,
	 * this just exposes the enum in a Blueprint-arithmetic-friendly shape.
	 */
	UFUNCTION(BlueprintPure)
	FVector GetTranslateAxisMask() const;
};
