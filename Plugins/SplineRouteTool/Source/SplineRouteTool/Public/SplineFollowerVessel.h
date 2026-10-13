// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Camera/CameraTypes.h" // EViewTargetBlendFunction — a UPROPERTY type, so UHT needs the full type
#include "VesselCatalog.h" // FDolphinCompanionSettings — a UFUNCTION param, so UHT needs the full type
#include "SplineFollowerVessel.generated.h"

class UStaticMeshComponent;
class UStaticMesh;
class USplineFollowerComponent;
class ASplineRouteActor;
class ADolphinCompanion;
class ACharacter;
class UArrowComponent;
class UCameraComponent;

/** One of the 5 editor-placed camera viewpoints a vessel Blueprint can be given. Cycled at runtime via "C". */
UENUM(BlueprintType)
enum class EVesselCameraSlot : uint8
{
	/** Relabel per-vessel in CameraSlotNames — this enum only fixes the cycling order. */
	Slot1 UMETA(DisplayName = "Slot 1"),
	Slot2 UMETA(DisplayName = "Slot 2"),
	Slot3 UMETA(DisplayName = "Slot 3"),
	Slot4 UMETA(DisplayName = "Slot 4"),
	Slot5 UMETA(DisplayName = "Slot 5")
};

/**
 * Runtime-spawnable vessel: mesh + follower component. Designed for the vessel-picker workflow,
 * where the operator picks an entry in the HUD and clicks "Spawn" to drop one onto the active route.
 *
 * Plain AActor — kept simple and undecorated on purpose. Wake used to be chased by making this class
 * itself Character-shaped (STATE.md 2026-08-17, tenth/thirteenth entries), which meant fighting
 * USplineFollowerComponent's kinematic motion with a disabled CharacterMovementComponent tick and
 * never produced a confirmed positive wake result on its own. A hand-rebuilt native companion
 * (AWakeCompanion) was tried next and still didn't show wake even with every property matched by
 * hand. Wake is now handled by spawning the actual, confirmed-100%-working BP_Ship_Cargo Blueprint
 * itself (not a rebuilt approximation) as a hidden passenger, attached in BeginPlay — see
 * SpawnWakeCompanion(). This class goes back to being exactly what it was before the whole detour:
 * a mesh driven kinematically by USplineFollowerComponent, nothing else.
 */
UCLASS()
class SPLINEROUTETOOL_API ASplineFollowerVessel : public AActor
{
	GENERATED_BODY()

public:
	ASplineFollowerVessel();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	TObjectPtr<UStaticMeshComponent> MeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	TObjectPtr<USplineFollowerComponent> Follower;

	/** Cached catalog scale set by Configure(). Runtime size adjustments multiply this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	FVector BaseScale = FVector::OneVector;

	/** Multiplier last applied by SetSizeMultiplier(). Used by the scenario serializer to save this vessel's actual size. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	float CurrentSizeMultiplier = 1.0f;

	/** Height offset resolved by Configure() in unscaled mesh-space units. Runtime size multipliers re-scale this onto the follower. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	float BaseHeightOffset = 0.0f;

	/** Corrective mesh rotation set by Configure() (e.g. DCC up-axis fixups). Applied to MeshComponent, not the actor root. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	FRotator VesselRotationOffset = FRotator::ZeroRotator;

	/** Catalog entry index this vessel was spawned from. Used by the scenario serializer. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	int32 CatalogIndex = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel")
	int32 SpawnOrder = 0;

	/** Cosmetic dolphin spawned in BeginPlay when the catalog entry asks for one. Null otherwise. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel|Companion")
	TObjectPtr<ADolphinCompanion> CompanionActor;

	/** Companion settings copied from the catalog entry. Cosmetic, so deliberately never serialized into a scenario. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel|Companion")
	FDolphinCompanionSettings DolphinCompanion;

	/**
	 * The actual BP_Ship_Cargo Blueprint, spawned hidden and attached in BeginPlay purely to trigger
	 * Waterline's wake — see SpawnWakeCompanion(). Typed ACharacter (BP_Ship_Cargo's own base), not a
	 * dedicated companion class — there's nothing project-specific left to type it as.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vessel|Companion")
	TObjectPtr<ACharacter> WakeCompanionActor;

	/**
	 * 5 editor-placed camera viewpoints, indexed by EVesselCameraSlot. Each arrow is a visible-in-editor,
	 * hidden-in-game gizmo Gevor drags into position per vessel Blueprint; its child camera follows it.
	 * VisibleAnywhere so each of the 5 is individually selectable/draggable in the Blueprint's Viewport
	 * tab, exactly like any other native component — being stored in an array (for clean indexing by
	 * EVesselCameraSlot) doesn't change how each one registers/edits, since that's per-instance-name, not
	 * per-UPROPERTY-shape.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Vessel|Cameras")
	TArray<TObjectPtr<UArrowComponent>> CameraArrows;

	/** Camera attached to each CameraArrows entry. Only one is ever Active at a time — see ActivateCameraSlot. */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = "Vessel|Cameras")
	TArray<TObjectPtr<UCameraComponent>> Cameras;

	/** Per-slot display label, parallel to Cameras/CameraArrows. Freely relabeled per vessel Blueprint — not baked into the enum. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel|Cameras")
	TArray<FText> CameraSlotNames;

	/** Blend duration used whenever this vessel's cameras are cut to/from. Tune per vessel Blueprint (e.g. snappier for a tugboat). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel|Cameras", meta = (ClampMin = "0.1"))
	float CameraBlendTime = 0.75f;

	/** Blend easing used alongside CameraBlendTime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel|Cameras")
	TEnumAsByte<EViewTargetBlendFunction> CameraBlendFunction = VTBlend_EaseInOut;

	/**
	 * Exponent controlling how sharp CameraBlendFunction's curve is (SetViewTargetWithBlend's BlendExp
	 * param) — NOT safe to leave at the engine default of 0. FMath::InterpEaseInOut(A, B, Alpha, Exp)
	 * computes Pow(Alpha, Exp) internally; Exp=0 makes Pow(x, 0) == 1 for any x > 0, which pins the
	 * blend at exactly the 50% midpoint for almost the entire duration and only reaches the true
	 * start/end pose in the last instant — this is what produced the reported "pause, then snap" /
	 * "wrong direction then correct" glitches, not a component-activation timing issue. 2.0 is a
	 * standard, smooth ease exponent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vessel|Cameras", meta = (ClampMin = "0.01"))
	float CameraBlendExp = 2.0f;

	/**
	 * Activate Cameras[SlotIndex] and deactivate every other camera on this vessel. Pass an out-of-range
	 * index (e.g. -1) to deactivate all of them, used when cycling wraps back to the free camera.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vessel|Cameras")
	void ActivateCameraSlot(int32 SlotIndex);

	/** Number of camera slots on this vessel (always 5) — read this instead of hardcoding the count. */
	UFUNCTION(BlueprintCallable, Category = "Vessel|Cameras")
	int32 GetNumCameraSlots() const { return Cameras.Num(); }

	/** Bounds-checked read of CameraSlotNames. Returns FText::GetEmpty() if Slot is somehow out of range. */
	UFUNCTION(BlueprintCallable, Category = "Vessel|Cameras")
	FText GetCameraSlotDisplayName(EVesselCameraSlot Slot) const;

	/**
	 * Apply scale + route + speed + height offset in one shot.
	 * Call between SpawnActorDeferred and FinishSpawning so the follower's BeginPlay sees a valid Route.
	 *
	 * InMesh is optional (may be nullptr) — catalog vessels now bake their mesh directly into the
	 * Blueprint's MeshComponent default, so the normal call passes nullptr and the Blueprint's own mesh
	 * is used as-is. Only pass a non-null mesh to explicitly override it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vessel")
	void Configure(UStaticMesh* InMesh, FVector InRelativeScale, ASplineRouteActor* InRoute, float InSpeed, float InHeightOffset, float InTurnRate = 90.0f, FRotator InRotationOffset = FRotator::ZeroRotator, float InReverseApproachSeconds = 3.0f, float InReverseMinSpeedFactor = 0.1f);

	/**
	 * Stash the catalog entry's dolphin settings. Kept separate from Configure() rather than bloating its
	 * signature further; call it alongside Configure(), before FinishSpawning, so BeginPlay can act on it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vessel|Companion")
	void SetDolphinCompanionSettings(const FDolphinCompanionSettings& InSettings);

	/** Multiply BaseScale (the catalog's RelativeScale) by Multiplier and apply to the mesh. */
	UFUNCTION(BlueprintCallable, Category = "Vessel")
	void SetSizeMultiplier(float Multiplier);

	/** World-space length of the mesh along its longest bounding-box axis at its current scale. Falls back to 1000.0 if no mesh is set. */
	UFUNCTION(BlueprintCallable, Category = "Vessel")
	float GetMeshLength() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Resolve the soft companion class and spawn/attach it. No-op unless the catalog entry enabled one. */
	void SpawnDolphinCompanion();

	/** Spawn a hidden BP_Ship_Cargo instance and attach it to this vessel's root. Always runs (not catalog-gated). */
	void SpawnWakeCompanion();
};
