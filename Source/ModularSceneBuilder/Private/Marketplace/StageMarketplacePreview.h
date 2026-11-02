// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Scene/StageSceneTypes.h"
#include "UObject/GCObject.h"

/** What Show placed from a previewed scene. */
struct FStageMarketplacePreviewStats
{
	int32 Placed = 0;
	/** Items whose catalog entry this build does not have: counted, never spawned. */
	int32 NotInLibrary = 0;
};

namespace StageMarketplacePreview
{
	/**
	 * Spawns Scene's items into World the way USpawnSystemComponent::RestoreItem does (saved state, item data, finish spawning),
	 * without anything that belongs to the stage: no placement rules, session, selection or history. Items not in Catalog are
	 * counted, not spawned. OutBounds grows by every spawned actor. Separate from FStageMarketplacePreview so that tests can run it
	 * in a GamePreview world without a renderer.
	 */
	FStageMarketplacePreviewStats SpawnScene(class UWorld& World, const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, class UBaseItemData*>& Catalog,
		TArray<TObjectPtr<class AActor>>& OutActors, FBox& OutBounds);

	/** One item of SpawnScene, for callers that spread a scene over several frames (the export job). Adds to Stats. */
	void SpawnItem(class UWorld& World, const FStageItemSnapshot& Snapshot, const TMap<FSoftObjectPath, class UBaseItemData*>& Catalog,
		TArray<TObjectPtr<class AActor>>& OutActors, FBox& OutBounds, FStageMarketplacePreviewStats& Stats);
}

/**
 * The read-only 3D preview of a marketplace listing (Docs/ADR/0021-community-marketplace.md §12.3).
 *
 * The stage is spawned into a private preview world (FPreviewScene, EWorldType::GamePreview: its own light and sky) and captured
 * into a render target that the Marketplace panel draws as an image. It is read-only by construction, not by checks:
 *  - no StageCraft world subsystem exists in a GamePreview world (session, show control, DMX patch, history, environment);
 *  - FPreviewScene never begins play, so the actors never register with anything;
 *  - there is no controller, spawn system, selection or history to edit with.
 * The main stage, its dirty flag, undo history and DMX output are never touched.
 *
 * Rendering is event-driven: a capture happens when the scene, the orbit or the zoom changes, never per frame (the preview world
 * does not tick, so each capture first sends its end-of-frame updates). Owned by the panel widget; game thread only. Without a
 * renderer (-nullrhi) nothing is created and IsAvailable is false.
 */
class FStageMarketplacePreview : public FGCObject
{
public:
	/** Render target size: 16:9, sharp in a floating window at 1x-1.5x DPI. */
	static constexpr int32 Width = 1280;
	static constexpr int32 Height = 720;

	FStageMarketplacePreview();
	virtual ~FStageMarketplacePreview() override;
	FStageMarketplacePreview(const FStageMarketplacePreview&) = delete;
	FStageMarketplacePreview& operator=(const FStageMarketplacePreview&) = delete;

	/** False without a renderer; every other call is then a no-op. */
	bool IsAvailable() const;

	/**
	 * Replaces the shown stage with Scene and frames it. Catalog maps catalog paths to loaded items; an item missing from it is
	 * counted in NotInLibrary and not spawned, as a scene load would not place it either.
	 */
	FStageMarketplacePreviewStats Show(const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, class UBaseItemData*>& Catalog);

	/** Removes the shown stage (the preview world stays for the next Show). */
	void Clear();

	/**
	 * Turns the camera around the stage (degrees) and zooms (positive steps move closer), then captures once. The view coalesces
	 * a frame's input into one call, so a fast mouse costs one capture per frame, not one per event.
	 */
	void MoveView(float DeltaYaw, float DeltaPitch, float ZoomSteps);

	/** Back to the default three-quarter view of the whole stage. */
	void Frame();

	/** The image the panel draws; null until the first Show or without a renderer. */
	class UTextureRenderTarget2D* GetRenderTarget() const { return Target; }

	//~ Begin FGCObject Interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	//~ End FGCObject Interface

private:
	bool EnsureScene();
	void Render();

	TUniquePtr<class FPreviewScene> Scene;
	TObjectPtr<class USceneCaptureComponent2D> Capture = nullptr;
	TObjectPtr<class UStaticMeshComponent> Floor = nullptr;
	TObjectPtr<class UTextureRenderTarget2D> Target = nullptr;

	/** Spawned into the preview world; destroyed by Clear. */
	TArray<TObjectPtr<class AActor>> Actors;

	/** The camera orbits Focus at Distance; Yaw/Pitch in degrees. */
	FVector Focus = FVector::ZeroVector;
	double Radius = 1000.0;
	double Distance = 3000.0;
	double Yaw = -135.0;
	double Pitch = -25.0;
};
