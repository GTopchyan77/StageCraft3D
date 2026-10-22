// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sketch/StageSketchTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageSketchSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageSketchChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageSketchGenerationFinished, const FStageSketchGenerateResult&, Result);

/** A photo import started, ended or changed state, or the reference photo under the canvas changed. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageSketchImportChanged);

/**
 * The 2D-to-3D generator of this stage session (Docs/ADR/0015-sketch-to-stage.md §2.1): the source of truth for the sketch drawn
 * in the Sketch window (strokes, settings, the catalog item used for each element), and the planner that turns it into decks,
 * truss runs and fixtures.
 *
 * Ownership and writes:
 *  - The sketch is tool state, not stage state: the Sketch window edits it through the validated setters here (every stroke and
 *    setting is sanitized; the sketch holds at most StageSketchMath::MaxStrokes strokes). Each change fires OnSketchChanged.
 *  - It never changes the stage. Generating goes through AModularPlayerController::RequestGenerateFromSketch, which asks for the
 *    plan (MakePlan) and its assets (RequestPlanAssets), then spawns it through the player's spawn system and the GameMode rules
 *    (StageSketchBuilder) and records one undo step. ReportGeneration tells the views how it ended.
 *  - One generation is in flight at a time; its asset request is cancelled when the world ends.
 *  - Photo import (ImportImage) only adds strokes, like drawing them: the file is decoded and traced on a worker thread that touches
 *    no UObject, and the result is applied on the game thread if that import is still current. One import at a time; the online
 *    request and any late worker result are dropped when the world ends. The reference photo is a transient texture owned here.
 *
 * Per stage session: the sketch is dropped with the world and is not saved in scene files. Game and PIE worlds only. Never ticks
 * and never iterates the world; the catalog is read from UStageItemSubsystem.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageSketchSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	// --- Sketch ---

	const TArray<FStageSketchStroke>& GetStrokes() const { return Strokes; }

	/** Adds a stroke after StageSketchMath::SanitizeStroke. False, and nothing added, when it is unusable or the sketch is full. */
	bool AddStroke(const FStageSketchStroke& Stroke);

	/** False for an invalid index. */
	bool RemoveStroke(int32 Index);

	/** Removes the newest stroke. False when the sketch is empty. */
	bool RemoveLastStroke();

	/** Empties the sketch. No event when it already was. */
	void ClearStrokes();

	/** The newest stroke within Radius (cm) of Point, or INDEX_NONE. */
	int32 FindStrokeAt(const FVector2D& Point, double Radius) const;

	/** Strokes that become Element. */
	int32 CountStrokes(EStageSketchElement Element) const;

	// --- Settings ---

	const FStageSketchSettings& GetSettings() const { return Settings; }

	/** Stores the settings after StageSketchMath::SanitizeSettings. No event when nothing changed. */
	void SetSettings(const FStageSketchSettings& NewSettings);

	// --- Catalog items ---

	/** Catalog items that can build Element, in catalog order: deck items, truss spans, lighting fixtures. */
	TArray<class UBaseItemData*> GetCandidateItems(EStageSketchElement Element) const;

	/** The item that will build Element: the one chosen with SetItem while it is still a candidate, else the first candidate. */
	class UBaseItemData* GetItem(EStageSketchElement Element) const;

	/** Chooses the item for Element. False, and nothing changed, unless Item is one of its candidates. */
	bool SetItem(EStageSketchElement Element, class UBaseItemData* Item);

	// --- Generation ---

	/** The plan for the current sketch, settings and items (StageSketchGenerator::BuildPlan). Floor at world Z 0. */
	FStageSketchBuildPlan MakePlan() const;

	/**
	 * Keeps Plan as the generation in flight and streams its items' Game bundles; OnReady runs once they are in memory, possibly
	 * before this returns. The callback takes the plan with TakeReadyPlan. False, and nothing kept, while another generation is
	 * in flight or for an empty plan.
	 */
	bool RequestPlanAssets(const FStageSketchBuildPlan& Plan, FSimpleDelegate OnReady);

	bool IsGenerationPending() const { return PendingPlan.IsSet(); }

	/** The plan whose assets are ready, handed over once; unset before they are ready or after it was taken. */
	TOptional<FStageSketchBuildPlan> TakeReadyPlan();

	/** Records how a generation ended and broadcasts it (the Sketch window shows the message). */
	void ReportGeneration(const FStageSketchGenerateResult& Result);

	const FStageSketchGenerateResult& GetLastResult() const { return LastResult; }

	// --- Photo / scan import (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.6) ---

	/**
	 * Reads a PNG or JPEG drawing of the stage seen from above and adds what it shows to the sketch as ordinary strokes (closed
	 * outlines = decks, lines = truss, dots and small circles = fixtures), scaled so the image is Settings.PhotoWidth wide and centred
	 * on the plan origin; the photo is shown faintly under the canvas. Decoding and tracing run on a worker thread; the outcome
	 * arrives on OnImportChanged. Reader Claude asks the online model first and falls back to the local reading when it fails.
	 * False, with a reason, while another import runs, for an unsupported or missing file, or when Claude is asked for but not
	 * available. Nothing is built: the user checks the strokes and generates as usual.
	 */
	bool ImportImage(const FString& FilePath, EStageSketchReader Reader, FText& OutError);

	EStageSketchImportState GetImportState() const { return ImportState; }
	bool IsImportPending() const { return ImportState != EStageSketchImportState::Idle; }

	/** Abandons an import in progress; nothing is added. */
	void CancelImport();

	/** The last import's outcome, user-facing ("Traced plan.jpg: 2 decks, 3 truss lines, 6 fixtures"). */
	const FText& GetImportMessage() const { return ImportMessage; }
	bool WasImportSuccessful() const { return bImportSucceeded; }

	/** Imports finished so far (succeeded, failed or cancelled), so a view can tell a new outcome from one it has shown. */
	uint32 GetImportsFinished() const { return ImportsFinished; }

	/** Whether Claude can read a drawing now; OutReason says why not. */
	bool CanReadOnline(FText& OutReason) const;

	/** The last imported photo, shown under the canvas; null when there is none. */
	class UTexture2D* GetReferenceImage() const { return ReferenceImage; }

	/** Where the photo lies on the plan, in cm (X up the image, Y across it). */
	const FBox2D& GetReferenceBounds() const { return ReferenceBounds; }

	bool IsReferenceVisible() const { return bReferenceVisible && ReferenceImage != nullptr; }
	void SetReferenceVisible(bool bVisible);

	/** Removes the photo from under the canvas (the strokes stay). */
	void ClearReference();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Sketch")
	FOnStageSketchImportChanged OnImportChanged;

	/** Any change to the strokes, the settings or the chosen items. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Sketch")
	FOnStageSketchChanged OnSketchChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Sketch")
	FOnStageSketchGenerationFinished OnGenerationFinished;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	void HandlePlanAssetsLoaded();

	/** The worker's result, on the game thread; ignored unless Serial is still the import in progress. */
	void HandleImageRead(uint32 Serial, TSharedRef<struct FStageSketchImportJob> Job);
	void HandleOnlineReading(const struct FStageSketchVisionResult& Result, uint32 Serial);

	/** Adds the local reading of Job; OnlineFailure says why it is used instead of the online one (empty when it was asked for). */
	void ApplyLocalReading(const struct FStageSketchImportJob& Job, const FText& OnlineFailure);

	/** Adds imported strokes after sanitizing, up to the sketch's limit, with one OnSketchChanged. Returns the strokes added. */
	TArray<FStageSketchStroke> AddImportedStrokes(TConstArrayView<FStageSketchStroke> NewStrokes);

	void ShowReference(const struct FStageSketchImportJob& Job);
	void FinishImport(bool bSucceeded, const FText& Message);

	class UStageItemSubsystem* GetCatalog() const;

	TArray<FStageSketchStroke> Strokes;
	FStageSketchSettings Settings;

	/** Chosen items, one per EStageSketchElement; null means "the first candidate". Catalog items, kept alive by the catalog too. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UBaseItemData>> ChosenItems;

	TOptional<FStageSketchBuildPlan> PendingPlan;
	bool bPendingPlanReady = false;
	FSimpleDelegate PendingCallback;
	TSharedPtr<struct FStreamableHandle> LoadHandle;

	FStageSketchGenerateResult LastResult;

	// --- Photo import ---
	EStageSketchImportState ImportState = EStageSketchImportState::Idle;

	/** Bumped by every new import and by cancelling, so a late result of an abandoned one is dropped. */
	uint32 ImportSerial = 0;
	bool bImportOnline = false;
	FText ImportMessage;
	bool bImportSucceeded = false;
	uint32 ImportsFinished = 0;

	/** The import waiting for the online reading, kept for its local fallback. */
	TSharedPtr<struct FStageSketchImportJob> OnlineJob;

	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> ReferenceImage = nullptr;

	FBox2D ReferenceBounds = FBox2D(ForceInit);
	bool bReferenceVisible = true;

	/** Made the first time Claude is asked to read a photo; null until then. */
	UPROPERTY(Transient)
	TObjectPtr<class UStageSketchVisionBackend> VisionBackend = nullptr;
};
