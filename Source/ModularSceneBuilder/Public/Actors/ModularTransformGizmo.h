// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ModularTransformGizmo.generated.h"

UENUM(BlueprintType)
enum class EGizmoMode : uint8
{
	Translate,
	Rotate,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGizmoModeChanged, EGizmoMode, NewMode);

/**
 * World-space translate/rotate gizmo for one target actor.
 *
 * Operates on plain AActor transforms, so it never depends on concrete item classes. It rides
 * along with the target via attachment (absolute rotation/scale keep the handles world-aligned)
 * and knows nothing about input: the controller supplies cursor rays for TryBeginDrag/UpdateDrag.
 *
 * Handles render on top of all scene geometry (HandleMaterial has depth testing disabled) and are
 * picked with TraceHandles, which tests only the handle components, so a gizmo buried inside another
 * mesh stays both visible and grabbable. Handles block only the StageCraft "Gizmo" trace channel, so
 * they never interfere with placement, selection or deletion traces. Arrows use engine BasicShapes; rotation rings are
 * generated procedurally because the engine ships no runtime torus mesh.
 *
 * Ticks only while attached, to keep a constant on-screen size.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AModularTransformGizmo : public AActor
{
	GENERATED_BODY()

public:
	AModularTransformGizmo();

	/** Shows the gizmo on the target and follows it. Passing null is equivalent to DetachFromTarget. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Gizmo")
	void AttachToTarget(AActor* NewTarget);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Gizmo")
	void DetachFromTarget();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Gizmo")
	AActor* GetTarget() const { return Target; }

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Gizmo")
	void SetMode(EGizmoMode NewMode);

	/** Space-bar behaviour: switches between Translate and Rotate, like the Unreal Editor's W/E. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Gizmo")
	void ToggleMode();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Gizmo")
	EGizmoMode GetMode() const { return Mode; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Gizmo")
	FOnGizmoModeChanged OnModeChanged;

	/**
	 * Starts a drag if HandleHit is one of this gizmo's visible handles. The ray is the cursor ray
	 * in world space. Returns false (and does nothing) otherwise.
	 */
	bool TryBeginDrag(const FHitResult& HandleHit, const FVector& RayOrigin, const FVector& RayDirection);

	/** Moves/rotates the target to follow the cursor ray. No-op when not dragging. */
	void UpdateDrag(const FVector& RayOrigin, const FVector& RayDirection);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Gizmo")
	void EndDrag();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Gizmo")
	bool IsDragging() const { return DragAxisIndex != INDEX_NONE; }

	/**
	 * Closest visible handle hit by the world-space ray. Ignores all other geometry on purpose: the
	 * handles draw on top of everything, so anything the user can see must also be clickable.
	 */
	bool TraceHandles(const FVector& RayOrigin, const FVector& RayDirection, struct FHitResult& OutHit) const;

protected:
	//~ Begin AActor Interface
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	//~ End AActor Interface

	/** Translation step in cm while dragging. Zero disables snapping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Gizmo|Snapping", meta = (ClampMin = "0.0", Units = "cm"))
	float TranslationSnap = 0.f;

	/** Rotation step in degrees while dragging. Zero disables snapping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "StageCraft|Gizmo|Snapping", meta = (ClampMin = "0.0", ClampMax = "180.0", Units = "deg"))
	float RotationSnapDegrees = 0.f;

	/** World scale per cm of camera distance. Keeps the gizmo roughly constant in screen size. */
	UPROPERTY(EditAnywhere, Category = "StageCraft|Gizmo|Appearance", meta = (ClampMin = "0.0001"))
	float ScreenSizeFactor = 0.0012f;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo|Appearance")
	TObjectPtr<class UStaticMesh> ArrowShaftMesh = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo|Appearance")
	TObjectPtr<class UStaticMesh> ArrowHeadMesh = nullptr;

	/**
	 * Must expose a "GizmoColor" vector parameter. The default, /Game/StageCraft/Gizmo/M_GizmoHandle, is
	 * unlit translucent with Disable Depth Test, so handles are never hidden by meshes. Any replacement
	 * needs the same settings to keep that guarantee. Falls back to the engine GizmoMaterial (depth tested).
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo|Appearance")
	TObjectPtr<class UMaterialInterface> HandleMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo|Appearance")
	FLinearColor AxisColors[3] = { FLinearColor(0.9f, 0.1f, 0.1f), FLinearColor(0.1f, 0.8f, 0.1f), FLinearColor(0.1f, 0.3f, 1.0f) };

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Gizmo|Appearance")
	FLinearColor ActiveAxisColor = FLinearColor(1.0f, 0.85f, 0.0f);

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class USceneComponent> GizmoRoot = nullptr;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> ShaftComponents[3];

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UStaticMeshComponent> HeadComponents[3];

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<class UProceduralMeshComponent> RingComponents[3];

private:
	void BuildRingMeshes();
	void ApplyModeVisibility();
	void SetAxisHighlighted(int32 AxisIndex, bool bHighlighted);
	void UpdateScreenScale();

	/** Axis index (0 = X, 1 = Y, 2 = Z) owning the component, or INDEX_NONE if it is not a visible handle of this gizmo. */
	int32 FindHandleAxis(const UPrimitiveComponent* Component) const;

	/** Cursor ray vs. the plane captured at drag start. */
	bool IntersectDragPlane(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutPoint) const;

	static FVector AxisVector(int32 AxisIndex);

	UPROPERTY(Transient)
	TObjectPtr<class AActor> Target = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> AxisMaterials[3];

	EGizmoMode Mode = EGizmoMode::Translate;

	// Drag state, captured in TryBeginDrag so the drag is stable while the target moves.
	int32 DragAxisIndex = INDEX_NONE;
	FPlane DragPlane;
	FVector DragStartPoint = FVector::ZeroVector;
	FVector DragStartLocation = FVector::ZeroVector;
	FQuat DragStartRotation = FQuat::Identity;
};
