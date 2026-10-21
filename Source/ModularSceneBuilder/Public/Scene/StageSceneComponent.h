// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SpawnSystemComponent.h"
#include "Engine/TimerHandle.h"
#include "Scene/StageSceneTypes.h"
#include "StageSceneComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSceneOperationFinished, const FStageSceneOperationResult&, Result);

/**
 * The local player's scene files: save the stage under a name, load a saved stage in place of the current one, list and
 * delete saved scenes (Docs/ADR/0004-selection-scenes-and-rendering.md §4). Requested through the controller's
 * RequestSaveScene / RequestLoadScene / RequestDeleteScene, which add the input guards and report refusals.
 *
 * Ownership:
 *  - The stage itself (which items exist) stays with UStageSessionSubsystem, which also keeps the current scene name and
 *    the dirty flag that a save clears. This component never caches items.
 *  - Files go through FStageSceneStore. Writing and reading run on background tasks; the result comes back to the game
 *    thread, where this component (held weakly by the task) applies it. One operation at a time (Busy otherwise).
 *
 * Loading replaces the stage: current items are removed, each item in the file is restored with its saved instance id and
 * settings through USpawnSystemComponent::RestoreItem, so the placement rules decide (deny by default). Items whose catalog
 * entry is unknown, or that the rules refuse now (PlacementEvaluator, asked silently so a load raises one summary instead of
 * a toast per item), are skipped and counted. The scene's environment (format version 2) is applied through
 * UStageEnvironmentSubsystem. The undo history is cleared, because its steps name the previous stage.
 *
 * A partial load loses nothing (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §3.1):
 *  - Skipped unknown or refused items are kept here as snapshots and written back by every save and autosave, until another
 *    scene is loaded. Damaged entries cannot be kept; the first save over that scene's file backs the file up first (.bak).
 *  - A load that skipped, kept or repaired anything leaves the stage unsaved, so autosave and the exit prompt cover it.
 *  - A load waits for imported models: it is refused (ImportsPending) while UStageMeshImportSubsystem is still restoring or
 *    importing, because their items would otherwise count as unknown.
 *
 * Autosave (Docs/ADR/0010-edit-rules-autosave-scene-v2.md §3): while the stage has unsaved changes, a looping timer writes it
 * every AutosaveIntervalSeconds to Saved/StageCraft/Scenes/Autosave/<scene name>.json ("(Untitled)" for a stage never saved),
 * on a background task, only when something changed since the last autosave. The timer runs only while the session is dirty
 * (started and stopped by OnDirtyChanged). An autosave never replaces the user's scene file and never marks the stage clean.
 * A successful Save, or discarding the changes on exit, deletes the stage's autosave; after a crash it stays, and
 * RecoverAutosave opens it with its unsaved changes still marked as unsaved.
 *
 * Requires USpawnSystemComponent and USelectionComponent on the same actor. No tick.
 */
UCLASS(ClassGroup = (StageCraft), meta = (BlueprintSpawnableComponent))
class MODULARSCENEBUILDER_API UStageSceneComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStageSceneComponent();

	/**
	 * Starts writing the current stage as Name (replacing a scene of that name). Returns Success when the save has started,
	 * or why it could not (InvalidName, Busy, Unavailable). The outcome arrives on OnSceneOperationFinished.
	 */
	EStageSceneResult SaveScene(const FString& Name);

	/**
	 * Starts reading the scene Name; when it has been read and validated, it replaces the current stage. Returns Success
	 * when the load has started, or why not (InvalidName, NotFound, Busy, CatalogNotReady, Unavailable). Unsaved changes are
	 * discarded: the caller asks the user first (the File menu does). The outcome arrives on OnSceneOperationFinished.
	 */
	EStageSceneResult LoadScene(const FString& Name);

	/** Deletes the saved scene Name from disk. The stage on screen is not changed. Broadcasts the outcome. */
	EStageSceneResult DeleteScene(const FString& Name);

	/** Saved scene names, sorted. Reads the scene folder; meant for menus when they open, not for every frame. */
	TArray<FString> GetSceneNames() const;

	bool SceneExists(const FString& Name) const;

	/** The folder scene files are written to (Saved/StageCraft/Scenes). */
	FString GetScenesDirectory() const;

	bool IsBusy() const { return bOperationInFlight; }

	/** Items of the loaded scene that are not on the stage but are kept for the next save (see the class comment). */
	int32 GetUnplacedItemCount() const { return UnplacedItems.Num(); }

	/**
	 * Starts opening the autosave Name (from GetAutosaveNames) in place of the current stage, like LoadScene. The stage keeps
	 * the scene name it was autosaved under and stays marked as unsaved, because the scene file itself does not have those
	 * changes. The caller asks the user first if the current stage has unsaved changes.
	 */
	EStageSceneResult RecoverAutosave(const FString& Name);

	/** Autosaves that exist on disk (one per scene name, "(Untitled)" for a stage that was never saved), sorted. */
	TArray<FString> GetAutosaveNames() const;

	/** Deletes the current stage's autosave: the user chose to discard its unsaved changes. */
	void DiscardAutosave();

#if !UE_BUILD_SHIPPING
	/** For the StageCraft.Scene.AutosaveNow development command: runs the timer's autosave immediately. */
	void DevAutosaveNow() { Autosave(); }
#endif

	/** File name of the autosave of a stage that has never been saved. */
	static const TCHAR* UntitledAutosaveName;

	/** Seconds between autosaves while the stage has unsaved changes. 0 turns autosave off. */
	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Scene", meta = (ClampMin = "0", Units = "Seconds"))
	float AutosaveIntervalSeconds = 120.f;

	/** User-facing text for a failed operation (empty for Success). */
	static FText DescribeFailure(EStageSceneOperation Operation, EStageSceneResult Result, const FString& Name);

	/**
	 * Asked silently for each item while a scene is applied, so refused items are counted instead of each raising a
	 * refusal. Bound by the owning controller to the GameMode's placement rules. Unbound means no pre-check; the spawn
	 * system's own PlacementValidator still decides every item.
	 */
	FStagePlacementValidator PlacementEvaluator;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Scene")
	FOnStageSceneOperationFinished OnSceneOperationFinished;

protected:
	//~ Begin UActorComponent Interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End UActorComponent Interface

private:
	/** Where a load reads from: the user's saved scenes, or the autosaves (recovery keeps the stage marked as unsaved). */
	enum class ELoadSource : uint8
	{
		SavedScene,
		Autosave,
	};

	FStageSceneDocument CaptureStage(const FString& Name) const;
	void FinishSave(EStageSceneResult Result, const FString& Name, int32 ItemCount, uint64 EditSerialAtCapture, bool bBackedUp);
	EStageSceneResult StartLoad(ELoadSource Source, const FString& Name);
	void FinishLoad(EStageSceneResult Result, const FStageSceneDocument& Document, const struct FStageSceneParseReport& Report, ELoadSource Source);
	FStageSceneOperationResult ApplyDocument(const FStageSceneDocument& Document, const struct FStageSceneParseReport& Report, ELoadSource Source);

	/** "Loaded scene X: 3 items placed (1 not in the Library, 2 invalid settings repaired). ... marked unsaved." */
	static FText MakePartialLoadMessage(const FString& SceneName, const FStageSceneOperationResult& Outcome, int32 Refused, int32 Unknown, int32 Damaged);
	void Broadcast(const FStageSceneOperationResult& Result);
	class UStageSessionSubsystem* GetSession() const;

	/** Starts the autosave timer while the stage is dirty and stops it when it is clean. */
	UFUNCTION()
	void HandleDirtyChanged(bool bIsDirty);

	/** Timer callback: writes the stage to its autosave file if it changed since the last autosave. */
	void Autosave();
	void FinishAutosave(EStageSceneResult Result, const FString& FileName, uint64 EditSerialAtCapture);

	/** The current stage's autosave file name: its scene name, or UntitledAutosaveName. */
	FString GetAutosaveName() const;

	/** Deletes one autosave file if it exists. Small and local, so inline on the game thread. */
	void DeleteAutosaveFile(const FString& FileName) const;

	TWeakObjectPtr<class UStageSessionSubsystem> BoundSession;

	FTimerHandle AutosaveTimer;

	/** The session's edit serial when the last autosave was captured; no new autosave until it changes. */
	uint64 LastAutosaveSerial = 0;

	/** The file the last autosave wrote, so a Save under a new name also removes the old one ("(Untitled)"). */
	FString LastAutosaveName;

	bool bAutosaveInFlight = false;

	/**
	 * Items of the loaded scene that could not be placed (unknown catalog entry, refused by the rules), kept so CaptureStage
	 * writes them back. Plain snapshots: a soft item reference and serialized state, never a live object. Replaced by the next load.
	 */
	TArray<FStageItemSnapshot> UnplacedItems;

	/** The scene whose load dropped damaged entries; the next save under this name backs its file up first. Empty otherwise. */
	FString SceneWithDroppedEntries;

	/** Saved/StageCraft/Scenes/Autosave. Immutable and shared with background tasks, like Store. */
	TSharedPtr<const class FStageSceneStore, ESPMode::ThreadSafe> AutosaveStore;


	UPROPERTY(Transient)
	TObjectPtr<class USpawnSystemComponent> SpawnSystem = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USelectionComponent> Selection = nullptr;

	/** Optional: leaving Place mode before a load keeps the ghost from landing on items that are about to disappear. */
	UPROPERTY(Transient)
	TObjectPtr<class UStagePlacementToolComponent> PlacementTool = nullptr;

	/** Immutable after construction and shared with background tasks, which may outlive this component. */
	TSharedPtr<const class FStageSceneStore, ESPMode::ThreadSafe> Store;

	bool bOperationInFlight = false;
};
