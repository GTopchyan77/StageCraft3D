// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interaction/StageTransformRules.h"
#include "Scene/StageSceneTypes.h"

/** What Parse had to fix or drop while reading a scene file. */
struct FStageSceneParseReport
{
	/** Items dropped because their data was invalid (bad id, path, transform or state). */
	int32 InvalidItems = 0;

	/** Items whose instance id repeated an earlier one in the file; they were given a fresh id. */
	int32 RepairedIds = 0;

	/** Cues, fixtures in cues and cue attributes dropped because they were invalid (format version 3). Logged; the rest loads. */
	int32 InvalidCueEntries = 0;
};

/**
 * Reads, writes, validates and versions stage scene files (Docs/ADR/0004-selection-scenes-and-rendering.md §4).
 *
 * Files: <Directory>/<Name>.json, one per scene, written only by an explicit Save. JSON so a scene is readable, diffable
 * and repairable by hand; each item's per-instance settings are its SaveGame properties as tagged (name-keyed) property
 * data in base64, the same bytes undo snapshots hold.
 *
 * A scene file is untrusted input. Parse rejects a file that is not a scene or has an unknown version, caps the item count
 * and per-item state size, and drops items with invalid ids, paths or transforms (non-finite, out of range, degenerate
 * rotation); scale is clamped like every other scale write. Whether an item's catalog entry exists and may be placed is
 * decided later, when the scene is applied, by the catalog and the placement rules. A file that fails to parse is never
 * renamed or deleted: it is the user's data.
 *
 * Pure logic plus file I/O: no world, no UObjects. Every function is safe on any thread, because the store holds only an
 * immutable directory path; saving and loading run on background tasks.
 */
class FStageSceneStore
{
public:
	/**
	 * The version Save writes. Bump it whenever the format changes, and teach Parse to migrate the older versions it still
	 * reads (Docs/ADR/0010-edit-rules-autosave-scene-v2.md §4):
	 *  - 1: items only.
	 *  - 2: adds "environment" (sky, time of day, sun path, weather). A version 1 file loads with the default environment.
	 *  - 3: adds "cues" (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.5). Version 1 and 2 files load with no cues.
	 */
	static constexpr int32 FormatVersion = 3;
	/** The oldest version Parse still migrates. Older or newer files are refused as VersionMismatch and left untouched. */
	static constexpr int32 MinReadableVersion = 1;
	static constexpr int32 MaxItems = 10000;
	static constexpr int32 MaxStateBytes = 64 * 1024;
	static constexpr int32 MaxLabelLength = 256;
	static constexpr int32 MaxCues = 2000;
	static constexpr int32 MaxFixturesPerCue = 4096;
	static constexpr int32 MaxAttributesPerFixture = 256;
	/** The same limit every transform write uses (StageTransformRules::MaxCoordinate). */
	static constexpr double MaxCoordinate = StageTransformRules::MaxCoordinate;

	explicit FStageSceneStore(FString InDirectory);

	/** The same rules as workspace layout names: 1-64 letters, digits, space, '-', '_', '(' and ')'; no reserved device names. */
	static bool IsValidSceneName(const FString& Name);

	/** Names of the saved scenes, sorted case-insensitively. Files with names a save would never write are ignored. */
	TArray<FString> ListScenes() const;

	bool SceneExists(const FString& Name) const;

	FString GetScenePath(const FString& Name) const;

	const FString& GetDirectory() const { return Directory; }

	/** Writes Document under Document.Name atomically (temporary file, then move), replacing a scene of the same name. */
	EStageSceneResult Save(const FStageSceneDocument& Document) const;

	/** Reads and validates the scene Name. OutDocument.Name is Name (the file name is authoritative). */
	EStageSceneResult Load(const FString& Name, FStageSceneDocument& OutDocument, FStageSceneParseReport& OutReport) const;

	EStageSceneResult Delete(const FString& Name) const;

	static FString Serialize(const FStageSceneDocument& Document);
	static EStageSceneResult Parse(const FString& Contents, FStageSceneDocument& OutDocument, FStageSceneParseReport& OutReport);

private:
	FString Directory;
};
