// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageMvrItemOrigin.generated.h"

/**
 * Where an item imported from MVR came from, and what the file said about it (Docs/ADR/0034 §18, MV5). Kept on the item as tagged
 * SaveGame data next to its MVR UUID, so undo snapshots and scene files carry it, scenes written before it load with none, and
 * older builds skip it.
 *
 * Why per-field fingerprints and not the file's values: a re-import must apply only the changes the FILE made (the MVR spec's "apply
 * only intended changes"). Comparing the file with the stage would undo every edit the user made in StageCraft (a renamed fixture,
 * a nudged truss) on every re-import. Comparing the file with what it said last time tells which fields the designer changed;
 * only those are written, the rest of the item stays as the user left it. A hash per field is enough to tell "changed or not"
 * and costs 20 bytes per item. Zero means "not known" (items imported before MV5); those are compared with the stage instead.
 *
 * Preserved is the MVR XML of the object that StageCraft does not use (StageMvrPreserved), so an export writes it back.
 */
USTRUCT()
struct MODULARSCENEBUILDER_API FStageMvrItemOrigin
{
	GENERATED_BODY()

	/** The MVR layer the object was on: with SourceFile, what makes a later file "the same plot" when the object is gone from it. */
	UPROPERTY(SaveGame)
	FGuid Layer;

	/** The .mvr file's name (no folder), as it was imported. */
	UPROPERTY(SaveGame)
	FString SourceFile;

	/** StageMvrImport::FFingerprint at the last import: matrix (and truss geometry), name, patch, GDTF type and mode, focus aim. */
	UPROPERTY(SaveGame)
	uint32 Placement = 0;

	UPROPERTY(SaveGame)
	uint32 Name = 0;

	UPROPERTY(SaveGame)
	uint32 Patch = 0;

	UPROPERTY(SaveGame)
	uint32 Type = 0;

	UPROPERTY(SaveGame)
	uint32 Aim = 0;

	/** "<Keep ...>...</Keep>": the object's XML StageCraft does not read, written back on export. Empty when there is none. */
	UPROPERTY(SaveGame)
	FString Preserved;

	/** True when the fingerprints are known (imported by MV5 or later). */
	bool HasFingerprints() const { return Placement != 0; }

	/** Longest SourceFile and Preserved kept; longer ones are dropped when the item's state is applied (saved state is untrusted). */
	static constexpr int32 MaxSourceFileLength = 256;
	static constexpr int32 MaxPreservedLength = 16 * 1024;
};
