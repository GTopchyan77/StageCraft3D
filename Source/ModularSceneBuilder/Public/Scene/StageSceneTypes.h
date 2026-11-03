// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Environment/StageEnvironmentTypes.h"
#include "History/StageItemSnapshot.h"
#include "Show/ShowControlTypes.h"
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
	/** Imported models are still being loaded, so the items made from them cannot be placed yet. */
	ImportsPending,
};

UENUM(BlueprintType)
enum class EStageSceneOperation : uint8
{
	Save,
	Load,
	Delete,
	/** A starter stage was opened as a new untitled stage (Docs/ADR/0024 §6). */
	OpenTemplate,
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

	/** Load: skipped items kept with the stage, so a save writes them back unchanged (unknown or refused, not damaged). Save: how many were written back. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	int32 KeptCount = 0;

	/** Load only: saved values that were invalid and were repaired (non-finite, out of range, unreadable). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	int32 RepairedCount = 0;

	/** User-facing summary, e.g. "Loaded My Show: 24 items (2 skipped: locked)". */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Scene")
	FText Message;

	bool IsSuccess() const { return Result == EStageSceneResult::Success; }
};

/**
 * A stage as written to and read from a scene file: every placed item with its catalog entry, transform, stable instance
 * id and per-instance settings (the SaveGame properties of FStageItemSnapshot), and the outdoor environment. Plain value type: safe to build on the game
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

	/** Sky, time of day, sun path and weather (format version 2). A version 1 file reads as the default environment. Sanitized. */
	FStageEnvironmentSettings Environment;

	/** The show's cue list (format version 3). Version 1 and 2 files read as no cues. Sanitized (StageCueRules). */
	TArray<FShowCue> Cues;

	/**
	 * The community marketplace listing this stage was acquired as (format version 4; Docs/ADR/0022-dcc-bridge.md §4). Set when a
	 * listing is imported and kept by every later save of the stage, Save As included, so a bought stage stays inside the app:
	 * scene export and publishing refuse it. Invalid (the default, and every older file) means the user's own stage.
	 */
	FGuid OriginListingId;

	bool IsFromMarketplace() const { return OriginListingId.IsValid(); }

	/**
	 * The programmer values per fixture ID (format version 6; Docs/ADR/0025-programmer-and-output-mix.md §7). Older files read as
	 * an empty programmer, and their fixtures take their saved output as their home look, so they look as they did.
	 */
	TArray<FShowCueFixtureState> Programmer;

	/** The show fixture groups (format version 6). Sanitized (StageFixtureGroupRules). Older files read as no groups. */
	TArray<FStageFixtureGroup> Groups;

	/** The cue that was playing when the stage was saved (format version 6), so loading resumes the show there. Unset: none. */
	TOptional<float> ActiveCueNumber;
};
