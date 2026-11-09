// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/LightingFixtureData.h"
#include "StageFixtureLibraryTypes.generated.h"

UENUM(BlueprintType)
enum class EStageFixtureImportResult : uint8
{
	Success,
	/** Not a .gdtf file. */
	UnsupportedFormat,
	FileNotFound,
	FileTooLarge,
	/** The file is already being imported or waiting for the user's review (or a session-only type is already in the library). */
	AlreadyInLibrary,
	/** The library holds StageFixtureLibrary::MaxTypes types. */
	LibraryFull,
	/** The file is not a GDTF file StageCraft can use: damaged, or no mode StageCraft can represent. The report says why. */
	Refused,
	/** The file could not be read, or the library copy could not be written. */
	IOError,
	/** Importing fixtures is switched off by the session rules, or the library is not available. */
	NotAllowed,
	/** A placed fixture still uses one of the type's modes, so the type cannot be removed. */
	InUse,
	/** No such fixture type in the library, or no such review waiting. */
	Unknown,
	/** The user cancelled the review. */
	Cancelled,
};

/** Where an import came from. Restores re-read the library at start and report only their failures. */
UENUM(BlueprintType)
enum class EStageFixtureImportOrigin : uint8
{
	User,
	Restore,
};

/** The outcome of importing one GDTF file (Docs/ADR/0033 §7). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFixtureImportOutcome
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString SourceFile;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	EStageFixtureImportResult Result = EStageFixtureImportResult::Refused;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	EStageFixtureImportOrigin Origin = EStageFixtureImportOrigin::User;

	/** The type's FixtureTypeID once the file was read; invalid before that. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FGuid FixtureTypeId;

	/** A sentence for the status bar, e.g. "Added StageCraft Test Spot (2 modes) to the Library." */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FText Message;

	/** The import report (modes, mapped and default channels, warnings, errors), one line per entry; empty when the file was not read. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString Report;

	/** The catalog items added, one per usable mode, in mode order. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	TArray<TObjectPtr<class ULightingFixtureData>> Modes;

	bool IsSuccess() const { return Result == EStageFixtureImportResult::Success; }
};

/** One DMX mode of a file under review: what the user sees per mode in the import report (Docs/ADR/0033 §8). */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFixtureModeSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString Name;

	/** DMX channels the mode occupies. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	int32 Footprint = 0;

	/** Channels StageCraft drives exactly, drives with a difference, and only sends at their GDTF default. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	int32 Mapped = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	int32 Approximated = 0;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	int32 Defaulted = 0;

	/** The GDTF attributes of the approximated and the default channels, in channel order (at most MaxNamesListed each). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	TArray<FString> ApproximatedNames;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	TArray<FString> DefaultedNames;

	/** False when StageCraft cannot use the mode; Reason then says why and the mode is not added. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	bool bUsable = false;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString Reason;
};

/**
 * A GDTF file read and mapped, waiting for the user to add it, replace the type it updates, or cancel (Docs/ADR/0033 §8, §10). Held
 * by UStageFixtureLibrarySubsystem until confirmed or cancelled; nothing reaches the catalog before that. Plain data, safe to copy.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFixtureImportReview
{
	GENERATED_BODY()

	/** Names this review in ConfirmImport and CancelImport. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FGuid ReviewId;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString SourceFile;

	/** Invalid when the file could not be read far enough to have one. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FGuid FixtureTypeId;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString Manufacturer;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString Name;

	/** At least one mode can be added and the file itself had no error. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	bool bCanImport = false;

	/** The library already holds this FixtureTypeID: confirming replaces it, and placed fixtures follow modes with the same name. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	bool bReplaces = false;

	/** The kind StageCraft inferred; the user may correct it before adding (ADR 0033 §5.6). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	EStageFixtureKind InferredKind = EStageFixtureKind::MovingHeadSpot;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	TArray<FStageFixtureModeSummary> Modes;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	TArray<FString> Warnings;

	/** Errors that refuse the file; empty when bCanImport. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	TArray<FString> Errors;

	/** The full report, as written to the log. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Fixtures")
	FString ReportText;

	int32 CountUsableModes() const { return Modes.FilterByPredicate([](const FStageFixtureModeSummary& Mode) { return Mode.bUsable; }).Num(); }
};
