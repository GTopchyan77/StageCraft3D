// Copyright DamenTools internal. Gizmo Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WarehouseToolsHostSubsystem.generated.h"

class UInteractiveToolsContext;
class UCombinedTransformGizmo;
class UTransformProxy;
class APlayerController;
class FWarehouseToolsQueriesAPI;
class FWarehouseToolsTransactionsAPI;

/**
 * Runtime host for the engine's Interactive Tools Framework (the same framework
 * Modeling Mode uses), so the engine's own UCombinedTransformGizmo can run in a
 * game/PIE world instead of a hand-built gizmo. The ITF is a Runtime module; what
 * it lacks outside the editor is a host that supplies scene/camera state, feeds it
 * mouse input and ticks it - that is what this subsystem does. Only created for
 * Game/PIE worlds.
 */
UCLASS()
class GIZMOTOOL_API UWarehouseToolsHostSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	UInteractiveToolsContext* GetToolsContext() const { return ToolsContext; }

	/**
	 * True while the mouse is over a visible transform-gizmo handle, or a gizmo drag is in progress.
	 * BP_BuildManager.PressedLMB uses this to leave the selection alone when the click is meant for
	 * the gizmo: the gizmo's handles deliberately ignore every collision channel, so the Build Tool's
	 * own Visibility trace goes straight through them to whatever is behind.
	 */
	UFUNCTION(BlueprintPure, Category = "GizmoTool", meta = (WorldContext = "WorldContextObject"))
	static bool IsTransformGizmoUnderCursor(const UObject* WorldContextObject);

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInteractiveToolsContext> ToolsContext = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UCombinedTransformGizmo> TransformGizmo = nullptr;

	/** What the gizmo manipulates. Deliberately not bound to the actor's components: every change is routed through our own move rules instead. */
	UPROPERTY(Transient)
	TObjectPtr<UTransformProxy> GizmoProxy = nullptr;

	/** BP_BuildManager instance (Blueprint-only class, resolved by class name). */
	TWeakObjectPtr<UObject> BuildManager;

	TWeakObjectPtr<AActor> GizmoTarget;
	FTransform LastSyncedTargetTransform = FTransform::Identity;

	// Per-drag state: moves are only re-applied when the snapped cell or the height changes,
	// and yaw is applied as deltas (the free-rotation model turns the mesh, not the actor).
	bool bInGizmoEdit = false;
	FIntPoint DragLastCell = FIntPoint::ZeroValue;
	double DragLastZ = 0.0;
	double LastProxyYaw = 0.0;
	bool bLeftMouseWasDown = false;
	FVector2D LastMousePosition = FVector2D::ZeroVector;

	// TSharedPtr (not TUniquePtr): its deleter is bound where the object is created, so the
	// UHT-generated code can destroy this subsystem without seeing the complete API types.
	TSharedPtr<FWarehouseToolsQueriesAPI> QueriesAPI;
	TSharedPtr<FWarehouseToolsTransactionsAPI> TransactionsAPI;

	void CreateTransformGizmo();
	void ApplyUndimmedGizmoMaterials();
	void UpdateGizmoModeFromKeys(APlayerController* PC);
	void UpdateGizmoViewContext(APlayerController* PC);
	void FeedMouseInput(APlayerController* PC);
	void SyncGizmoToSelection();
	AActor* PollSelectedPlaceableObject();
	bool IsGizmoUnderCursorInternal() const;
	void OnGizmoBeginEdit(UTransformProxy* Proxy);
	void OnGizmoTransformChanged(UTransformProxy* Proxy, FTransform NewTransform);
	void OnGizmoEndEdit(UTransformProxy* Proxy);
	void ShutdownToolsContext();
};
