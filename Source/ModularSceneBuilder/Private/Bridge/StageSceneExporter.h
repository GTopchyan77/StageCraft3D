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
 *  2. A scene acquired from the marketplace (FStageSceneDocument::OriginListingId) leaves only when its publisher allowed it:
 *     UStageMarketplaceSubsystem::CheckExportRights reads the library package kept at acquire time (ADR 0022 §11.1). Without a
 *     marketplace subsystem, or without rights, it is refused (NotOriginal).
 *  3. Only catalog items the user may place are exported: free items and owned ones (exact entitlement), as a scene load allows.
 *  4. The items are spawned into a private GamePreview world (StageMarketplacePreview::SpawnScene: no session, rules or play).
 *  5. Procedural parts (decks, stairs, arc truss) become transient static meshes, because the engine exporter writes static,
 *     instanced and skeletal meshes only.
 *  6. The engine's runtime glTF exporter writes the file next to its target and the file is then moved into place, so a reader
 *     never sees a half-written file. The world is destroyed before returning; the transient meshes are unreferenced then and
 *     left to the garbage collector (nothing in between can run a collection: the export is one synchronous call).
 *
 * Game thread (UObjects, the exporter). Synchronous: user-initiated, measured and logged per export. A class of static functions
 * rather than a namespace, so the elaborated types below name the global engine types (Rules: Code Standards).
 */
/** Where an export spent its time (milliseconds, wall clock), for the log and StageCraft.Bench.Export (STATE.md #55). */
struct FStageSceneExportTimings
{
	/** Reading and parsing the scene file. */
	double LoadMs = 0.0;
	/** Reading the library package for a marketplace stage's export rights. */
	double RightsMs = 0.0;
	/** Creating the private preview world. */
	double WorldMs = 0.0;
	/** Spawning the items (including synchronous loads of their classes). */
	double SpawnMs = 0.0;
	/** Baking procedural parts into transient static meshes. */
	double ConvertMs = 0.0;
	/** The engine's glTF conversion and file write. */
	double GltfMs = 0.0;
	/** Moving the file into place and reading its size. */
	double FinalizeMs = 0.0;
	/** Destroying the preview world. */
	double TeardownMs = 0.0;
	/** The SHA-1 of the file, for a send. */
	double HashMs = 0.0;

	double GetTotalMs() const { return LoadMs + RightsMs + WorldMs + SpawnMs + ConvertMs + GltfMs + FinalizeMs + TeardownMs + HashMs; }
	FString ToString() const;
};

/** What one spawned actor contributes to the file (FStageSceneExporter::InspectActor). */
struct FStageActorExportContent
{
	bool bHasGeometry = false;
	int32 Lights = 0;
};

/** Everything an export counts, for its result and message (ADR 0022 §13). */
struct FStageSceneExportCounts
{
	/** Items spawned and written to the file. */
	int32 Exported = 0;
	/** Not owned, not in this Library, or broken content (no actor class): never spawned. */
	int32 NotInLibrary = 0;
	/** Hidden on stage by the user. */
	int32 Hidden = 0;
	/** Damaged entries of the scene file, dropped when it was read. */
	int32 InvalidInFile = 0;
	/** Exported items that produced no geometry. */
	int32 WithoutGeometry = 0;
	/** Lights written (fixture beams). */
	int32 Lights = 0;
};

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

	/**
	 * What Actor will put into the file, judged as the glTF exporter will: visible, not hidden in game, static meshes with a mesh
	 * (instanced ones with instances) and lights. Run after ConvertProceduralMeshes, whose copies count as geometry.
	 */
	static FStageActorExportContent InspectActor(const class AActor& Actor);

	/** Adds Actor's lights to Counts, and counts and logs it when it produced no geometry. Shared by both export paths. */
	static void AddActorContent(const class AActor& Actor, FStageSceneExportCounts& Counts);

	/**
	 * Steps 3-6 for a scene already read (tests and the saved-scene path). Needs a renderer; without one, Unavailable.
	 * InvalidItemsInFile is the parse report's count of entries dropped when the file was read, so the result can name them.
	 */
	static FStageBridgeResultInfo ExportDocument(const FStageSceneDocument& Scene, const TMap<FSoftObjectPath, class UBaseItemData*>& Catalog, const FString& Path,
		FStageSceneExportTimings* OutTimings = nullptr, int32 InvalidItemsInFile = 0);

	/**
	 * Steps 1-6: the scene SceneName saved in Scenes, for the profile and library of GameInstance, to Path (absolute, ending in
	 * .glb). Refusals that need no export (bad request, missing, unreadable, from the marketplace) come before anything is loaded.
	 */
	static FStageBridgeResultInfo ExportSavedScene(class UGameInstance& GameInstance, const class FStageSceneStore& Scenes, const FString& SceneName, const FString& Path,
		FStageSceneExportTimings* OutTimings = nullptr);

	// --- Shared with FStageSceneExportJob, so both paths decide and word things the same way ---

	/** InvalidRequest unless SceneName is a valid scene name and Path an absolute .glb path; otherwise Success. */
	static FStageBridgeResultInfo ValidateRequest(const FString& SceneName, const FString& Path);
	/** Unavailable when this process cannot render (no mesh data to read); otherwise Success. */
	static FStageBridgeResultInfo CheckRenderer();
	/** NotFound / Unreadable for a scene that could not be loaded. */
	static FStageBridgeResultInfo DescribeLoadFailure(EStageSceneResult LoadResult, const FString& SceneName);
	/** NotOriginal with the marketplace's reason (or a general one). */
	static FStageBridgeResultInfo DescribeNoRights(const FString& SceneName, const FText& Reason);
	static FStageBridgeResultInfo DescribeNothingToExport(const FString& SceneName);
	static FStageBridgeResultInfo DescribeExportFailed(const FString& SceneName, const TArray<FString>& Errors);
	static FStageBridgeResultInfo DescribeWriteFailed(const FString& Path);
	/** Success with every count of Counts, and a message naming each thing left out (ADR 0022 §13). */
	static FStageBridgeResultInfo DescribeExported(const FString& SceneName, const FStageSceneExportCounts& Counts, const FString& Path);
	/** " Left out: 2 hidden on stage, 1 not owned or not in your Library." for the counts in Result; empty when nothing was left out. */
	static FText DescribeLeftOut(const FStageBridgeResultInfo& Result);

	/** The glTF options: geometry and fixture lights (ADR 0022 §2, §13). A transient object; use it within the frame. */
	static class UGLTFExportOptions* MakeExportOptions();
	/** Logs the engine exporter's errors and warnings, capped. */
	static void LogExportMessages(const struct FGLTFExportMessages& Messages);
	/** Where the file is written before it is moved to Path. */
	static FString GetExportTempPath(const FString& Path);

	/**
	 * The private world an export spawns into: GamePreview, actors initialized, play never begun (nothing registers with the
	 * stage, the show or DMX), and without the systems an export never uses (physics, navigation, AI, effects, audio, world
	 * partition), which are most of a world's creation cost (StageCraft.Bench.Export). Destroy it with DestroyExportWorld.
	 */
	static class UWorld* CreateExportWorld();
	static void DestroyExportWorld(class UWorld* World);
};
