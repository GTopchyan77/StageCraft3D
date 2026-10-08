// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SpawnSystemComponent.h"
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
 * a toast per item), are skipped and counted. The undo history is cleared, because its steps name the previous stage.
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
	FStageSceneDocument CaptureStage(const FString& Name) const;
	void FinishSave(EStageSceneResult Result, const FString& Name, int32 ItemCount, uint64 EditSerialAtCapture);
	void FinishLoad(EStageSceneResult Result, const FStageSceneDocument& Document, const struct FStageSceneParseReport& Report);
	FStageSceneOperationResult ApplyDocument(const FStageSceneDocument& Document, const struct FStageSceneParseReport& Report);
	void Broadcast(const FStageSceneOperationResult& Result);
	class UStageSessionSubsystem* GetSession() const;


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
