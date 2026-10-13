// Copyright DamenTools internal. Camera Presets plugin.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CameraPresetComponent.generated.h"

class USpringArmComponent;
class UCameraComponent;

UENUM(BlueprintType)
enum class ECameraPresetView : uint8
{
	None		UMETA(DisplayName = "None"),
	Top			UMETA(DisplayName = "Top"),
	Front		UMETA(DisplayName = "Front"),
	Side		UMETA(DisplayName = "Side"),
	Isometric	UMETA(DisplayName = "Isometric")
};

/**
 * Drives a smooth, fixed-duration blend of the owning actor's SpringArmComponent
 * between quick-switch camera presets (Top/Front/Side/Isometric) for the
 * Warehouse Planner, each framed to fit a BP_BuildManager instance's grid.
 *
 * Attach to BP_Demo_Camera (or any actor with a USpringArmComponent as a
 * direct component). All interpolation math and preset-transform
 * calculation lives here in C++ — Blueprint's only job is: bind a key,
 * call SwitchToPreset, and Branch on its two out bools (toggle walls /
 * free-cam-suspend bool on whichever one comes back true; do nothing if
 * both are false, which means this call just switched directly between
 * two already-active presets).
 *
 * BP_BuildManager is a Blueprint-only class (no native base) — reading its
 * grid extents requires the same deliberate reflection bridge documented
 * on SpaceCalculatorLibrary::GetSpaceUsage (see the SpaceCalculator
 * plugin). ComputeGridFraming expects EXACTLY these two UPROPERTYs on
 * BuildManagerRef's class, by these exact names/types:
 *   - GridSize       : FIntPoint        (grid dimensions, in cells)
 *   - GridCellSizeXY : float or double  (cell edge length, cm — Blueprint
 *                                        "float" vars can be FFloatProperty
 *                                        or FDoubleProperty depending on
 *                                        their Real Number setting; both
 *                                        are accepted, same as SpaceCalculator)
 * If either is missing/renamed, framing falls back to FallbackArmLength
 * centered on the camera's own current location (logged as an error, not
 * a silent wrong answer) rather than failing the whole preset switch.
 */
UCLASS(ClassGroup = (Camera), meta = (BlueprintSpawnableComponent))
class CAMERAPRESETS_API UCameraPresetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCameraPresetComponent();

	/**
	 * Blends smoothly to Preset, framed to fit BuildManagerRef's grid.
	 *
	 * - From free-cam (no active preset): caches the current camera
	 *   transform so it can be restored later, then blends in.
	 * - While already in a DIFFERENT preset: blends directly preset-to-
	 *   preset, no restore.
	 * - Re-called with the CURRENTLY-ACTIVE preset: blends back to the
	 *   cached free-cam transform.
	 * - Called again mid-blend: retargets smoothly from the current
	 *   interpolated transform, not the previous target.
	 *
	 * @param BuildManagerRef       A BP_BuildManager_C instance (or anything exposing GridSize/GridCellSizeXY).
	 * @param Preset                Which preset to blend to. None is a no-op.
	 * @param BlendTime             Total blend duration in seconds.
	 * @param bEnteredPresetView    Out: true if this call just left free-cam for a preset (cached the free-cam transform).
	 * @param bExitedToFreeCam      Out: true if this call just left a preset back to free-cam. Never true at the same time as bEnteredPresetView.
	 */
	UFUNCTION(BlueprintCallable, Category = "CameraPresets")
	void SwitchToPreset(UObject* BuildManagerRef, ECameraPresetView Preset, float BlendTime, bool& bEnteredPresetView, bool& bExitedToFreeCam);

	UFUNCTION(BlueprintPure, Category = "CameraPresets")
	bool IsInPresetView() const { return CurrentPreset != ECameraPresetView::None; }

	UFUNCTION(BlueprintPure, Category = "CameraPresets")
	ECameraPresetView GetCurrentPreset() const { return CurrentPreset; }

	/**
	 * Switches the owner's CameraComponent between perspective and orthographic projection.
	 * Entering orthographic (bEnable=true) frames OrthoWidth to fit BuildManagerRef's grid,
	 * reusing ComputeGridFraming's grid-reading (same reflection-based GridSize/
	 * GridCellSizeXY read as SwitchToPreset - see class comment). Leaving orthographic
	 * (bEnable=false) just restores Perspective; it does not touch the SpringArm or
	 * any preset/free-cam state, so it can be combined freely with SwitchToPreset.
	 *
	 * @param BuildManagerRef  A BP_BuildManager_C instance (or anything exposing GridSize/GridCellSizeXY). Ignored when bEnable is false.
	 * @param bEnable          True to switch to orthographic, false to switch back to perspective.
	 */
	UFUNCTION(BlueprintCallable, Category = "CameraPresets")
	void ToggleOrthographic(UObject* BuildManagerRef, bool bEnable);

	UFUNCTION(BlueprintPure, Category = "CameraPresets")
	bool IsOrthographic() const;

	/** Extra headroom multiplied onto each preset's computed framing distance (1.0 = tight fit around the grid). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CameraPresets")
	float PaddingFactor = 1.2f;

	/** Framing distance used when BuildManagerRef's grid can't be read. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "CameraPresets")
	float FallbackArmLength = 5000.f;

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/**
	 * Reads GridSize/GridCellSizeXY off BuildManagerRef via reflection; computes the grid's
	 * world-space center and a framing arm length for it. Returns false (fallback values
	 * filled) if the properties can't be read.
	 * @param OutMaxExtent  Optional: the raw grid extent (max of world width/depth, cm) the
	 *                      arm length was derived from - the FOV/tan conversion baked into
	 *                      OutArmLength is perspective-specific, so callers that need the
	 *                      extent itself (e.g. orthographic OrthoWidth) should read this
	 *                      instead of re-deriving it from OutArmLength.
	 */
	bool ComputeGridFraming(UObject* BuildManagerRef, FVector& OutPivot, float& OutArmLength, float* OutMaxExtent = nullptr) const;

	static FRotator GetPresetRotation(ECameraPresetView Preset);

	UPROPERTY(Transient)
	TObjectPtr<USpringArmComponent> CachedSpringArm;

	UPROPERTY(Transient)
	TObjectPtr<UCameraComponent> CachedCamera;

	ECameraPresetView CurrentPreset = ECameraPresetView::None;

	bool bBlending = false;
	float BlendElapsed = 0.f;
	float BlendDuration = 0.6f;

	FVector BlendStartPivot = FVector::ZeroVector;
	FQuat BlendStartArmRotation = FQuat::Identity;
	float BlendStartArmLength = 0.f;

	FVector BlendTargetPivot = FVector::ZeroVector;
	FQuat BlendTargetArmRotation = FQuat::Identity;
	float BlendTargetArmLength = 0.f;

	bool bHasCachedFreeCam = false;
	FVector CachedFreeCamPivot = FVector::ZeroVector;
	FQuat CachedFreeCamArmRotation = FQuat::Identity;
	float CachedFreeCamArmLength = 0.f;
};
