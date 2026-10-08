// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "History/StageItemSnapshot.h"
#include "StageSceneTypes.generated.h"

/** Outcome of a scene file operation (Docs/ADR/0004-selection-scenes-and-rendering.md §4). */
UENUM(BlueprintType)
enum class EStageSceneResult : uint8
{
	Success,
	/** Empty, too long, or contains characters that are not allowed in a file name. */
	InvalidName,
	NotFound,
	/** The file is not a scene this build can read (bad JSON or missing fields). It is left untouched on disk. */
	Corrupt,
	/** The file was written by an incompatible version. It is left untouched on disk. */
	VersionMismatch,
	WriteFailed,
	/** Another save or load is still running; scene operations run one at a time. */
	Busy,
	/** The item Library has not finished loading, so the items in a scene cannot be checked yet. */
	CatalogNotReady,
	/** No stage session in this world (no session subsystem, spawn system or world). */
	Unavailable,
};

UENUM(BlueprintType)
enum class EStageSceneOperation : uint8
{
	Save,
	Load,
	Delete,
};

/** Reported once per finished scene operation, for the status bar and menus. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSceneOperationResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	EStageSceneOperation Operation = EStageSceneOperation::Save;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	EStageSceneResult Result = EStageSceneResult::Success;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	FString SceneName;

	/** Items written (save) or placed (load). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	int32 ItemCount = 0;

	/** Load only: items in the file that were not placed (unknown catalog item, invalid data, refused by the rules). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	int32 SkippedCount = 0;

	/** User-facing summary, e.g. "Loaded My Show: 24 items (2 skipped: locked)". */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	FText Message;

	bool IsSuccess() const { return Result == EStageSceneResult::Success; }
};

/**
 * A stage as written to and read from a scene file: every placed item with its catalog entry, transform, stable instance
 * id and per-instance settings (the SaveGame properties of FStageItemSnapshot). Plain value type: safe to build on the game
 * thread and hand to a background task for writing, or to build on a background task while reading.
 */
struct FStageSceneDocument
{
	/** User-facing name; also the file name. */
	FString Name;

	/** The level the scene was built in (informational; loading into another level is allowed and logged). */
	FString LevelName;

	FDateTime SavedAtUtc;

	TArray<FStageItemSnapshot> Items;
};
