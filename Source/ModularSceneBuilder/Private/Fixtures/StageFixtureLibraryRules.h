// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/LightingFixtureData.h"
#include "History/StageItemSnapshot.h"

/**
 * Pure rules of the fixture device library (Docs/ADR/0033 §4.1, §7): where library files live, the stable catalog path of each
 * mode, the library index file and the wording for scenes that name a type this PC does not have.
 *
 * No engine state and no UObjects are created, so every function is safe on any thread and covered by StageCraft.Fixtures.*
 * tests. MakeTypeRef reads a fixture data object and is for the game thread.
 */
namespace StageFixtureLibrary
{
	/** Package root of library fixture types: /Temp is mounted in every build and never saved, like imported models. */
	extern const TCHAR* const PackageRoot;

	/** The format tag a library type carries in ULightingFixtureData::SourceFormat. */
	extern const FName GdtfFormat;

	/** Largest .gdtf accepted, the ZIP reader's archive limit (ADR 0033 §8). */
	constexpr int64 MaxFileSizeBytes = 50ll * 1024 * 1024;

	/** Most fixture types one library holds. Further imports are refused. */
	constexpr int32 MaxTypes = 512;

	/** Longest mode slug; the rest of the mode name is dropped. */
	constexpr int32 MaxSlugLength = 64;

	/** Longest text a scene file's fixtureType fields keep. */
	constexpr int32 MaxRefTextLength = 256;

	/** Saved/StageCraft/Fixtures, or the test data root's. */
	FString GetLibraryDirectory();

	/** The library index, Fixtures.json, in the library directory. */
	FString GetIndexPath();

	/** The library copy of a type's file: <library>/<32 hex digits>.gdtf. */
	FString GetLibraryFilePath(const FGuid& FixtureTypeId);

	/** The type a library file name stands for; false for names the library never writes. */
	bool ParseLibraryFileName(const FString& CleanFileName, FGuid& OutFixtureTypeId);

	/**
	 * Slugs for every mode of a type, in mode order: each name reduced to letters, digits and underscores (other runs become one
	 * underscore), at most MaxSlugLength characters, "Mode" when nothing is left, and a "_2", "_3"... suffix when it repeats an
	 * earlier slug (compared without case). The same names always give the same slugs.
	 */
	TArray<FString> MakeModeSlugs(TConstArrayView<FString> ModeNames);

	/** /Temp/StageCraftFixtures/<32 hex digits>, the package folder of one type. */
	FString MakeTypeFolder(const FGuid& FixtureTypeId);

	/** The catalog path of one mode: <type folder>/<slug>.<slug>. */
	FSoftObjectPath MakeItemPath(const FGuid& FixtureTypeId, const FString& Slug);

	/** True only for a package a library mode could have: the root, 32 hex digits, then one slug. Untrusted input. */
	bool IsFixtureItemPath(const FString& PackageName);

	/** The reference a scene writes for a placed fixture of this data; unset for built-in fixtures. Game thread. */
	FStageFixtureTypeRef MakeTypeRef(const ULightingFixtureData& Fixture);

	/**
	 * One sentence per missing library type among Unplaced, in first-seen order, at most MaxTypes listed and a count of the rest:
	 * "StageCraft Test Spot (Standard) is missing. Import its GDTF file to see these 12 fixtures." Empty when no item names a type.
	 */
	FText DescribeMissingTypes(TConstArrayView<FStageItemSnapshot> Unplaced, int32 MaxListed = 3);

	/** What the library index remembers of one type, beyond the file itself. */
	struct FIndexEntry
	{
		FGuid FixtureTypeId;
		/** The file name the user imported, for display. */
		FString SourceFileName;
		FDateTime ImportedUtc;
		/** The user's correction of the inferred kind (ADR 0033 §5.6); unset keeps the inferred one. */
		TOptional<EStageFixtureKind> Kind;
	};

	/** The index file's text: {"format":"StageCraftFixtureLibrary","version":1,"fixtures":[...]}. */
	FString WriteIndex(TConstArrayView<FIndexEntry> Entries);

	/**
	 * Reads an index. False for text that is not an index of a known version; entries with an invalid id or kind are skipped and
	 * counted in OutSkipped, and repeated ids keep their first entry.
	 */
	bool ReadIndex(const FString& Json, TArray<FIndexEntry>& OutEntries, int32& OutSkipped);
}
