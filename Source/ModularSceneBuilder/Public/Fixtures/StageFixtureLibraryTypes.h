// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageFixtureLibraryTypes.generated.h"

UENUM(BlueprintType)
enum class EStageFixtureImportResult : uint8
{
	Success,
	/** Not a .gdtf file. */
	UnsupportedFormat,
	FileNotFound,
	FileTooLarge,
	/** The library already holds this FixtureTypeID, or the file is being imported. Replacing a type comes with the Library UI. */
	AlreadyInLibrary,
	/** The library holds StageFixtureLibrary::MaxTypes types. */
	LibraryFull,
	/** The file is not a GDTF file StageCraft can use: damaged, or no mode StageCraft can represent. The report says why. */
	Refused,
	/** The file could not be read, or the library copy could not be written. */
	IOError,
	/** Importing fixtures is switched off by the session rules, or the library is not available. */
	NotAllowed,
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
