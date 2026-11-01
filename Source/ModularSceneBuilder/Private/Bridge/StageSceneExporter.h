// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"
#include "Scene/StageSceneTypes.h"

/**
 * Turns a saved scene into a glTF binary file for other 3D apps (Docs/ADR/0022-dcc-bridge.md §1-4).
 *
 *  1. The scene is read from its file, never from the live stage, so the gizmo, ghosts, grid and unsaved edits are never part of
 *     an export, and the stage is never touched.
 *  2. A scene acquired from the marketplace is refused (FStageSceneDocument::OriginListingId): only the user's own stages leave.
 *  3. Only catalog items the user may place are exported: free items and owned ones (exact entitlement), as a scene load allows.
 *  4. The items are spawned into a private GamePreview world (StageMarketplacePreview::SpawnScene: no session, rules or play).
 *  5. Procedural parts (decks, stairs, arc truss) become transient static meshes, because the engine exporter writes static,
 *     instanced and skeletal meshes only.
 *  6. The engine's runtime glTF exporter writes the file next to its target and the file is then moved into place, so a reader
 *     never sees a half-written file. The world and its transient meshes are destroyed before returning.
 *
 * Game thread (UObjects, the exporter). Synchronous: user-initiated, measured and logged per export. A class of static functions
 * rather than a namespace, so the elaborated types below name the global engine types (Rules: Code Standards).
 */
class FStageSceneExporter
{
public:
	static constexpr const TCHAR* FileExtension = TEXT(".glb");

	/** Catalog items the profile may place (no entitlement needed, or owned exactly), keyed by catalog path. */
	static TMap<FSoftObjectPath, class UBaseItemData*> CollectExportableItems(const class UStageItemSubsystem& Items, const class UStageProfileSubsystem& Profile);

	/**
	 * Adds a static mesh component with a transient static mesh for every UProceduralMeshComponent of Actor that has geometry, at
	 * the same place and with the same materials (the procedural one stays; the exporter skips it). Returns how many were converted.
	 */
	static int32 ConvertProceduralMeshes(class AActor& Actor);

	/** Steps 3-6 for a scene already read (tests and the saved-scene path). Needs a renderer; without one, Unavailable. */
	static FStageBridgeResultInfo ExportDocument(const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, class UBaseItemData*>& Catalog, const FString& Path);

	/**
	 * Steps 1-6: the scene SceneName saved in Scenes, for the profile and library of GameInstance, to Path (absolute, ending in
	 * .glb). Refusals that need no export (bad request, missing, unreadable, from the marketplace) come before anything is loaded.
	 */
	static FStageBridgeResultInfo ExportSavedScene(class UGameInstance& GameInstance, const class FStageSceneStore& Scenes, const FString& SceneName, const FString& Path);
};
