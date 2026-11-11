// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Report/StageBillOfMaterialsSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageMvrExportSubsystem.generated.h"

/**
 * MVR scene export (Docs/ADR/0034 §7, phase MV4): writes the stage as an .mvr file that Vectorworks, grandMA3, Capture and other
 * MVR applications open, and that StageCraft's own import reads back to the same fixtures, patch, IDs and transforms.
 *
 * Flow: StartExport reads the stage on the game thread (StageMvrExportGather: plain values only, well under a frame for a stage's
 * worth of items), then a worker copies the GDTF library files, builds the archive (StageMvrExport::Write) and writes it atomically
 * (StageAtomicFile), and the result comes back on the game thread through OnExportFinished. One export at a time.
 *
 * Who calls: AModularPlayerController::RequestExportMvr after the GameMode allowed it (EvaluateMvrExport); dev console commands.
 * Views and the status bar follow OnExportFinished.
 * Lifetime: one stage (Game and PIE worlds). The worker's result is matched to this instance by weak pointer and a serial; a result
 * that arrives after the world is gone is dropped (the file is still written).
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageMvrExportSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

	/** True for an absolute path ending in .mvr (case-insensitive), the only paths StartExport writes. */
	static bool IsExportPath(const FString& Path);

	/**
	 * Starts writing the stage to Path (absolute, .mvr; replaced when it exists). False, with OutReason, when another export is
	 * running, the path is not an .mvr path, or the stage holds nothing to write; nothing starts then. On true the result arrives on
	 * OnExportFinished.
	 */
	bool StartExport(const FString& Path, FText& OutReason);

	bool IsExporting() const { return bExporting; }

	/** The last finished export; empty before the first. */
	const FStageReportExportResult& GetLastExport() const { return LastExport; }

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|MVR")
	FOnStageReportExportFinished OnExportFinished;

private:
	struct FCounts
	{
		int32 Fixtures = 0;
		int32 Trusses = 0;
		int32 OtherItems = 0;
		int32 Skipped = 0;
	};

	/** Error: why the archive was refused; bWriteFailed: it was built but could not be written. */
	void FinishExport(uint64 Serial, const FString& Path, const FString& Error, bool bWriteFailed, const TArray<FString>& Notes, FCounts Counts);

	FStageReportExportResult LastExport;
	bool bExporting = false;
	uint64 ExportSerial = 0;
};
