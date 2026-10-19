// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Viewport/StageViewportTypes.h"
#include "StageViewportComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageViewChanged);

/**
 * The local player's view of the stage (Docs/ADR/0008-viewport-toolbar-projections-and-view-modes.md): projection
 * (Perspective or an orthographic axis view), view mode (Lit / Unlit / Lines) and the reference grid. Lives on
 * AModularPlayerController with the other per-player tools; the viewport toolbar and the dev console are its views.
 *
 * Applying state is delegated to the objects that own it:
 *  - projection: the player camera manager (bIsOrthographic, DefaultOrthoWidth; ADR F5) and AStageCameraPawn's fixed view;
 *  - view mode: UStageCraftGameViewportClient::ApplyStageViewMode, plus UStageLineViewCameraModifier for Lines;
 *  - grid: AStageGridActor, sized by UStageSnappingComponent's grid snap (redrawn on OnSnapSettingsChanged).
 *
 * Orthographic navigation (controller input routes here while IsOrthographic): PanOrtho for a right-button drag, ZoomOrtho
 * for the wheel. Everything is event-driven; nothing ticks. Every change is broadcast once on OnViewChanged.
 *
 * Projection and view mode are not saved: every session starts in Lit Perspective. Grid visibility is a per-machine
 * preference (UStageCraftUserSettings; this component is its only writer).
 */
UCLASS(ClassGroup = (StageCraft), Config = Game, meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageViewportComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageViewportComponent();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Viewport")
	EStageViewProjection GetProjection() const { return Projection; }

	/**
	 * Switches the projection at once. Perspective -> ortho frames what the perspective view centred on at the same scale
	 * and remembers the perspective camera; ortho -> ortho keeps the centre and zoom; ortho -> Perspective restores the
	 * remembered camera. Returns false (nothing changes) without a camera manager or a StageCraft camera pawn.
	 */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Viewport")
	bool SetProjection(EStageViewProjection NewProjection);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Viewport")
	bool IsOrthographic() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Viewport")
	EStageViewMode GetViewMode() const { return ViewMode; }

	/** Returns false (mode unchanged) when the viewport is not a StageCraft viewport (e.g. PIE without the config) or, for Lines, when its material is missing. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Viewport")
	bool SetViewMode(EStageViewMode NewMode);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Viewport")
	bool IsGridVisible() const;

	/** Stores and saves the preference, then shows or hides the grid. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Viewport")
	void SetGridVisible(bool bVisible);

	/** Width of the orthographic view in cm (also kept while in Perspective, for the next ortho switch). */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Viewport")
	float GetOrthoWidth() const { return static_cast<float>(OrthoWidth); }

	/**
	 * Orthographic only: zooms by wheel notches (positive = in) and keeps the world point under CursorPosition (viewport
	 * pixels) where it is. Returns false in Perspective or without a viewport.
	 */
	bool ZoomOrtho(float Steps, const FVector2D& CursorPosition);

	/** Orthographic only: grab-pans by a mouse delta in pixels (X right, Y up). Returns false in Perspective or without a viewport. */
	bool PanOrtho(const FVector2D& MouseDelta);

	/** The grid actor, for diagnostics. Null before BeginPlay. */
	const class AStageGridActor* GetGridActor() const { return GridActor; }

	/** Projection, view mode, grid or ortho zoom changed. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Viewport")
	FOnStageViewChanged OnViewChanged;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

	/** The CAD line-view post-process material (DefaultGame.ini). Loaded on the first switch to Lines. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "StageCraft|Viewport")
	TSoftObjectPtr<class UMaterialInterface> LineViewMaterial;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport")
	TSubclassOf<class AStageGridActor> GridActorClass;

	/** How far behind the view centre an orthographic camera stands, so the whole layout is in front of the near plane. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport", meta = (ClampMin = "1000.0", Units = "cm"))
	double OrthoStandoff = 20000.0;

	/** How far beyond the origin the vertical grid of a front or side view stands, as seen from the camera. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport", meta = (ClampMin = "0.0", Units = "cm"))
	double GridBackDistance = 5000.0;

	/** The floor grid floats this far above Z = 0, so it does not z-fight with a stage floor whose top is exactly at 0. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport", meta = (ClampMin = "0.0", Units = "cm"))
	double GridFloorClearance = 1.0;

	/** Grid cell when grid snap is Off (the grid is still drawn, at this size). */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport", meta = (ClampMin = "1.0", Units = "cm"))
	double UnsnappedGridCell = 100.0;

	/** View centre distance used when the perspective view does not look down at the floor. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Viewport", meta = (ClampMin = "100.0", Units = "cm"))
	double FallbackFocusDistance = 2000.0;

private:
	UFUNCTION()
	void HandleSnapSettingsChanged();

	UFUNCTION()
	void HandlePossessedPawnChanged(class APawn* PreviousPawn, class APawn* NewPawn);

	class APlayerController* GetPlayerController() const;
	class AStageCameraPawn* GetCameraPawn() const;
	class UStageCraftGameViewportClient* GetStageViewportClient() const;

	/** The point an orthographic view centres on: where the camera looks at the floor, or FallbackFocusDistance ahead. */
	FVector FindPerspectiveFocus(const FVector& ViewLocation, const FRotator& ViewRotation) const;

	/** The centre of the current orthographic view. */
	FVector GetOrthoFocus() const;

	/** Points the pawn and the camera manager at the current projection around Focus. */
	void ApplyOrthographic(const FVector& Focus);
	void ApplyCameraProjection();

	/** Moves the orthographic camera by Shift (right, up) in its view plane. */
	void ShiftOrthoView(const FVector2D& Shift);

	bool GetViewportSize(FVector2D& OutSize) const;

	/**
	 * Size of one screen pixel in world units (right, up), measured by deprojecting neighbouring pixels of the current view.
	 * Measured rather than derived from OrthoWidth, so it holds for any ULocalPlayer::AspectRatioAxisConstraint.
	 */
	bool MeasureWorldPerPixel(FVector2D& OutWorldPerPixel) const;
	class UStageLineViewCameraModifier* GetOrCreateLineModifier();

	/** Marks the next frame as a camera cut: no temporal history across a projection or view-mode switch, exposure snaps to target. */
	void RequestCameraCut() const;
	void RefreshGrid();

	EStageViewProjection Projection = EStageViewProjection::Perspective;
	EStageViewMode ViewMode = EStageViewMode::Lit;
	double OrthoWidth = 2000.0;

	/** The perspective camera when an orthographic view was entered, restored on the way back. */
	TOptional<FTransform> SavedPerspectiveView;

	/** The camera manager's pitch limits (X min, Y max) before an orthographic view widened them to +-90 for Top. */
	TOptional<FVector2D> SavedPitchLimits;

	UPROPERTY(Transient)
	TObjectPtr<class AStageGridActor> GridActor = nullptr;

	/** Owned by the camera manager's modifier list; weak here. */
	TWeakObjectPtr<class UStageLineViewCameraModifier> LineModifier;

	TWeakObjectPtr<class UStageSnappingComponent> Snapping;
};
