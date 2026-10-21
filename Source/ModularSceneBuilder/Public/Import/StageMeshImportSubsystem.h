// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Import/StageMeshImportTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tasks/Pipe.h"
#include "StageMeshImportSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageMeshImportStarted, const FString&, SourceFile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageMeshImportFinished, const FStageMeshImportOutcome&, Outcome);

/**
 * Imports 3D model files (.glb, .gltf, .obj, and .fbx where the build has an FBX importer) while the app runs, and adds each one
 * to the Library as a placeable prop (ADR 0006).
 *
 * - Importing uses Unreal's Interchange framework with a pipeline built here: static meshes only, every mesh in a file combined
 *   into one, materials as instances. Assets are created under StageMeshImport::PackageRoot (/Temp) and never saved.
 * - One file imports at a time, in request order; requests made meanwhile queue. The game thread is never blocked: Interchange
 *   translates and builds on worker threads and the result is collected on the game thread.
 * - Successful imports are registered with UStageItemSubsystem (OnCatalogChanged), so the Library shows them at once and Place
 *   mode can arm them without a restart.
 * - Imported files are remembered in Saved/StageCraft/Imports.json and imported again at the next start, so scenes that use them
 *   load (each item's path is derived from its source file). A file that has moved is reported and kept in the list.
 *
 * Who calls: AModularPlayerController::RequestImportMeshFiles (after the GameMode allows it), and the dev console commands.
 * Lifetime: the whole run (GameInstance). Game thread only.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageMeshImportSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/**
	 * Queues files for import. Files that cannot be imported (format, missing, too large, already imported or queued) are refused
	 * now and returned; nothing is broadcast for them. Accepted files report through OnImportStarted / OnImportFinished.
	 */
	TArray<FStageMeshImportOutcome> RequestImport(const TArray<FString>& Files);

	/**
	 * Extensions this build can import, lower case without dots. Empty when the importer is unavailable. Asked from Interchange
	 * each time: its translators register only after engine init, later than this subsystem starts.
	 */
	TArray<FString> GetSupportedExtensions() const;

	/** True when at least one of the files has a supported extension (drop targets use it to accept or refuse a drag). */
	bool CanImportAny(const TArray<FString>& Files) const;

	/** Files waiting or importing now. */
	int32 GetPendingCount() const { return Queue.Num() + (Active.IsValid() ? 1 : 0); }

	/**
	 * Last run's import list is still being read, or files are waiting or importing. Until this is false, the catalog may
	 * still lack imported models that a scene places (a scene load waits for it; Docs/ADR/0013 §3.1).
	 */
	bool HasImportsInProgress() const { return ImportListState == EImportListState::Reading || GetPendingCount() > 0; }

	/** Every model imported so far this run. */
	const TArray<TObjectPtr<class UStageImportedItemData>>& GetImportedItems() const { return ImportedItems; }

	/** Where the list of imported files is kept between runs. */
	static FString GetImportListPath();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Import")
	FOnStageMeshImportStarted OnImportStarted;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Import")
	FOnStageMeshImportFinished OnImportFinished;

private:
	struct FQueuedImport
	{
		FString SourceFile;
		EStageMeshImportOrigin Origin = EStageMeshImportOrigin::User;
	};

	/** Validates one file for the queue; a refusal fills OutRefusal. */
	bool ValidateRequest(const FString& NormalizedFile, EStageMeshImportOrigin Origin, FStageMeshImportOutcome& OutRefusal) const;
	bool IsKnownFile(const FString& NormalizedFile) const;
	void Enqueue(const FString& NormalizedFile, EStageMeshImportOrigin Origin);

	/** Makes Interchange use its runtime import path in a standalone game run from editor binaries. */
	void UseRuntimeImportInGame();
	/** The Interchange pipeline for one file: static meshes only, plus the format's unit and axis correction. */
	class UInterchangePipelineBase* MakePipeline(const FString& SourceFile);
	void StartNextImport();
	void FinishActiveImport(uint64 Serial);
	FStageMeshImportOutcome CollectActiveImport();
	class UStageImportedItemData* RegisterItem(class UStaticMesh& Mesh, const FString& SourceFile);

	void LoadImportList();
	void HandleImportListLoaded(StageMeshImport::EImportListRead Read, TArray<FString> Files);
	void RememberFile(const FString& SourceFile);
	void SaveImportList();

	TWeakObjectPtr<class UStageItemSubsystem> ItemSubsystem;

	/** Pipeline of the import in flight; Interchange duplicates it when the import starts, this keeps the source alive until then. */
	UPROPERTY(Transient)
	TObjectPtr<class UInterchangePipelineBase> Pipeline = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageImportedItemData>> ImportedItems;

	/** Meshes, materials and textures of every import. Catalog items reference their mesh softly, so this keeps them alive. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> ImportedAssets;

	TArray<FQueuedImport> Queue;

	/** The import in flight (Interchange result, source file, serial). Defined in the .cpp to keep Interchange out of this header. */
	TSharedPtr<struct FStageActiveMeshImport> Active;

	/**
	 * Uniform "clay" material the thumbnails are rendered with (DefaultGame.ini). Thumbnails show shape, not the model's own
	 * materials, whose shaders may still be compiling right after import and whose brightness varies too much to read on a
	 * small dark tile. Unset or missing: the engine's default surface material.
	 */
	UPROPERTY(Config)
	TSoftObjectPtr<class UMaterialInterface> ThumbnailMaterial;
	TSharedPtr<struct FStreamableHandle> ThumbnailMaterialHandle;

	/** Renders Library thumbnails of imported models in a private preview world; created with the first import. */
	TSharedPtr<class FStageModelThumbnailRenderer> ThumbnailRenderer;
	uint64 ImportSerial = 0;

	/** Files imported successfully, in import order, as saved for the next run. */
	TArray<FString> RememberedFiles;

	/** Whether RememberedFiles may be written to the import list file. */
	enum class EImportListState : uint8
	{
		/** Last run's list is still being read; writing now would overwrite it. */
		Reading,
		Writable,
		/** The file exists but could not be read: it is never overwritten this session, so new imports are not remembered. */
		ReadOnly,
	};
	EImportListState ImportListState = EImportListState::Reading;

	/** Writes of the import list run in order on this pipe, so an older list never overwrites a newer one. */
	UE::Tasks::FPipe ImportListPipe{ UE_SOURCE_LOCATION };
};
