// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Vision/StageVisionBom.h"
#include "StageVisionBomSubsystem.generated.h"

/** Where the Vision Inspector is with the current photo. */
enum class EStageVisionBomState : uint8
{
	Idle,
	/** The file is being read and resized on a worker thread. */
	Preparing,
	/** The photo is with the online model. */
	Reading,
	/** A bill of materials is shown. */
	Ready,
	/** The last photo could not be read; GetMessage says why. */
	Failed,
};

/**
 * The Vision Inspector of this stage session (Docs/ADR/0032 §8): the source of truth for the bill of materials read from the current
 * reference photo, and the planner that turns it into a build plan.
 *
 * Ownership and writes:
 *  - The bill is tool state, not stage state: the Vision Inspector panel edits quantities and removes lines through the setters
 *    here; each change fires OnChanged. It never changes the stage: Build in Scene goes through
 *    AModularPlayerController::RequestBuildFromVisionBom (MakeBuildPlan, the sketch's asset streaming, one undo step).
 *  - One photo at a time. The file is decoded on a worker thread that touches no UObject; the online request belongs to the backend.
 *    A newer photo, Cancel, Clear and the end of the world drop any late answer (serial number).
 *  - What the model returns is untrusted input, validated by StageVisionBom::ParseResponseBody and matched to the catalog here.
 *
 * Per stage session: dropped with the world, not saved in scene files. Game and PIE worlds only. Never ticks.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageVisionBomSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Whether a photo can be read now; OutReason says why not. */
	bool CanInspect(FText& OutReason) const;

	/**
	 * Reads a PNG or JPEG photo of a stage: resized to 1568 px at most, sent to the online model, and the answer matched to the
	 * catalog. The outcome arrives on OnChanged. False, with a reason, while another photo is being read, for an unsupported or
	 * missing file, or when the online model is not available.
	 */
	bool Inspect(const FString& FilePath, FText& OutError);

	/** Abandons the photo being read; the previous bill stays. */
	void Cancel();

	/** Empties the bill. */
	void Clear();

	EStageVisionBomState GetState() const { return State; }
	bool IsBusy() const { return State == EStageVisionBomState::Preparing || State == EStageVisionBomState::Reading; }

	const TArray<FStageVisionBomLine>& GetLines() const { return Lines; }
	const FStageVisionBomStage& GetStageEstimate() const { return StageEstimate; }

	/** The last outcome, user-facing ("Read stage.jpg: 14 lines, 3 to check"). */
	const FText& GetMessage() const { return Message; }

	/** The model's own notes on what it could not see; empty when none. */
	const FString& GetModelNotes() const { return ModelNotes; }

	/** File name of the photo the bill was read from. */
	const FString& GetSourceName() const { return SourceName; }

	/** Sets a line's quantity (clamped to 1..MaxQuantity). False for an invalid index or while a photo is being read. */
	bool SetLineQuantity(int32 Index, int32 Quantity);

	/** Removes a line. False for an invalid index or while a photo is being read. */
	bool RemoveLine(int32 Index);

	FStageVisionBomTotals GetTotals() const;
	FStageVisionBomLayout GetLayout() const;

	/** The bill as CSV (StageVisionBom::ToCsv). */
	FString MakeCsv() const;

	/** Writes the CSV to Path (UTF-8). False, with a reason, when the bill is empty or the file cannot be written. */
	bool SaveCsv(const FString& Path, FText& OutError) const;

	/**
	 * The plan Build in Scene places: the deck and truss runs through the sketch generator (with the Sketch window's deck and truss
	 * items), then the matched items. False, with a reason, when nothing can be built.
	 */
	bool MakeBuildPlan(struct FStageSketchBuildPlan& OutPlan, FText& OutReason) const;

	/** Records how Build in Scene ended (the panel shows the message). */
	void ReportBuild(const struct FStageSketchGenerateResult& Result);

	/** Every change of state, bill or message. */
	FSimpleMulticastDelegate OnChanged;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: reads through Backend instead of the configured one (null restores it). */
	void SetBackendForTesting(class UStageVisionBomBackend* InBackend);

	/** Tests: sends an already prepared JPEG, as Inspect does after the worker. */
	bool InspectJpegForTesting(const TArray<uint8>& Jpeg, const FString& Name, FText& OutError);

	/** Tests: matches the bill against Catalog (test worlds have no game instance, so no catalog). */
	void MatchCatalogForTesting(TConstArrayView<const class UBaseItemData*> Catalog);
#endif

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	class UStageVisionBomBackend* GetBackend() const;
	bool SendJpeg(const TArray<uint8>& Jpeg, uint32 Serial);
	void HandlePrepared(uint32 Serial, TSharedRef<struct FStageVisionBomPhotoJob> Job);
	void HandleRead(const FStageVisionBomResult& Result, uint32 Serial);
	void Fail(const FText& Reason);

	UPROPERTY(Transient)
	TObjectPtr<class UStageVisionBomBackend> Backend = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageVisionBomBackend> TestBackend = nullptr;

	EStageVisionBomState State = EStageVisionBomState::Idle;
	TArray<FStageVisionBomLine> Lines;
	FStageVisionBomStage StageEstimate;
	FText Message;
	FString ModelNotes;
	FString SourceName;
	FString PendingName;

	/** Bumped by every photo, Cancel, Clear and Deinitialize, so late worker and online answers are recognised and dropped. */
	uint32 Serial = 0;
};
