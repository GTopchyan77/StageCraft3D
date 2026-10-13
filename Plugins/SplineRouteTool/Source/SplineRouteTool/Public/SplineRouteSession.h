// Copyright DamenTools internal. Spline Route Tool plugin.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SplineRouteSession.generated.h"

class UInputMappingContext;
class UInputAction;
class UUserWidget;
class UEnhancedInputComponent;
class APlayerController;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
class UMaterialInstanceDynamic;
struct FInputActionValue;

/**
 * Drop-in session actor for the runtime spline-route authoring tool.
 *
 * Place a single instance in any level to enable the tool without modifying the
 * host GameMode, PlayerController, or Pawn. BeginPlay is a no-op unless the
 * world's active GameMode is (or derives from) ASplineRouteGameModeBase — this
 * makes the actor harmless if left in a level shared with another GameMode
 * (e.g. the Warehouse Planner's GM_Demo_GameMode). When the GameMode matches,
 * on BeginPlay this actor:
 *   - Pushes the IMC onto the local player's Enhanced Input subsystem.
 *   - Creates its own UEnhancedInputComponent, registers it, pushes it onto the
 *     player controller's input stack, and binds Place/Release/Drag/Delete
 *     actions to USplineRouteAuthoringSubsystem.
 *   - Creates and adds the HUD widget to the viewport.
 *   - Forwards AuthoringPlaneZ to the authoring subsystem.
 *   - Optionally enables the mouse cursor and switches input mode to GameAndUI.
 *
 * On EndPlay it cleans up the IMC, input component, and HUD widget.
 */
UCLASS(BlueprintType, Blueprintable)
class SPLINEROUTETOOL_API ASplineRouteSession : public AActor
{
	GENERATED_BODY()

public:
	ASplineRouteSession();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	TObjectPtr<UInputMappingContext> InputMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	TObjectPtr<UInputAction> PlaceAction;

	/** Select an existing handle under the cursor (sets it as the drag target). Defaults to LMB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	TObjectPtr<UInputAction> SelectAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	TObjectPtr<UInputAction> ReleaseAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	TObjectPtr<UInputAction> DragAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	TObjectPtr<UInputAction> DeleteAction;

	/** HUD widget added to the viewport on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|HUD")
	TSubclassOf<UUserWidget> HUDWidgetClass;

	/** Z value of the click-to-place plane forwarded to USplineRouteAuthoringSubsystem. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Authoring")
	float AuthoringPlaneZ = 0.0f;

	/** If true, the session sets cursor visibility + GameAndUI input mode on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	bool bManageCursor = true;

	/** Priority used when pushing the session's input component onto the player controller. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Input")
	int32 SessionInputPriority = 100;

	/** Whether the cursor-follow placement preview ghost is shown at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Preview")
	bool bShowPlacementPreview = true;

	/** Mesh used for the placement preview ghost. Usually the same waypoint mesh as the route handles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Preview")
	TObjectPtr<UStaticMesh> PreviewMeshAsset;

	/** Material applied to the preview mesh. Must expose a vector parameter named "Color". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Preview")
	TObjectPtr<UMaterialInterface> PreviewMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Preview")
	FLinearColor AllowedColor = FLinearColor(0.05f, 1.0f, 0.2f, 0.85f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Preview")
	FLinearColor BlockedColor = FLinearColor(1.0f, 0.1f, 0.1f, 0.85f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Route|Session|Preview", meta = (ClampMin = "1.0"))
	float PreviewScale = 1.2f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spline Route|Session|Preview", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> PreviewMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PreviewMID;

	UPROPERTY(Transient)
	TObjectPtr<UEnhancedInputComponent> SessionInputComponent;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> HUDWidgetInstance;

	UPROPERTY(Transient)
	TWeakObjectPtr<APlayerController> BoundPC;

	void OnPlacePressed(const FInputActionValue& Value);
	void OnSelectPressed(const FInputActionValue& Value);
	void OnReleased(const FInputActionValue& Value);
	void OnDragTriggered(const FInputActionValue& Value);
	void OnDeletePressed(const FInputActionValue& Value);

	void UpdatePreviewGhost();
};
