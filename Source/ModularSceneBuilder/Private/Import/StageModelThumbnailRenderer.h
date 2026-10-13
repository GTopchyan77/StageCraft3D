// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Renders a small picture of an imported model for its Library card.
 *
 * The model is shown in a private preview world (FPreviewScene: its own light and sky, nothing from the stage), framed from a
 * three-quarter view, and captured once into a render target that the card draws directly; no GPU readback. The renderer owns
 * the preview world; the render targets belong to the items that show them.
 *
 * Owned by UStageMeshImportSubsystem. Game thread only. Without a renderer (-nullrhi, servers) Render returns null.
 */
class FStageModelThumbnailRenderer
{
public:
	/** Square size of every thumbnail in pixels: the Library tile is 30 px, so this stays sharp at 4x DPI. */
	static constexpr int32 Size = 128;

	FStageModelThumbnailRenderer();
	~FStageModelThumbnailRenderer();

	FStageModelThumbnailRenderer(const FStageModelThumbnailRenderer&) = delete;
	FStageModelThumbnailRenderer& operator=(const FStageModelThumbnailRenderer&) = delete;

	/**
	 * Captures Mesh, drawn entirely with Material (null: the engine default surface material), into a new render target
	 * outered to Outer. Null when rendering is unavailable or the mesh has no bounds.
	 */
	class UTextureRenderTarget2D* Render(class UStaticMesh& Mesh, class UMaterialInterface* Material, UObject& Outer);

private:
	bool EnsureScene();

	TUniquePtr<class FPreviewScene> Scene;

	/** Created with the scene and registered to it; the scene keeps them referenced for the garbage collector. */
	class UStaticMeshComponent* MeshComponent = nullptr;
	class USceneCaptureComponent2D* Capture = nullptr;
};
