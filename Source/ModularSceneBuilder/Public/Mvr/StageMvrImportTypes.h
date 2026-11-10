// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Where an MVR import is (Docs/ADR/0034 §6): one file at a time. */
enum class EStageMvrImportState : uint8
{
	/** Nothing is being imported. */
	Idle,
	/** The file is being read and checked on a worker. */
	Reading,
	/** Read: the review waits for the user (IMPORT or CANCEL). Nothing has changed yet. */
	Review,
	/** Confirmed: fixture types are added, items stream in and the rig is placed, a slice per frame. */
	Placing,
};

/** One GDTF fixture type an MVR file uses, as the review lists it. */
struct FStageMvrTypeLine
{
	/** "Robe MegaPointe", or the file name when the file could not be read. */
	FString Label;
	/** The GDTFSpec file name in the MVR. */
	FString File;
	/** Fixtures of the scene that use it. */
	int32 Fixtures = 0;
	/** The library already has this FixtureTypeID: its copy is used, the file's is not added. */
	bool bInLibrary = false;
	/** False when the file cannot be used (missing from the MVR, damaged, no usable mode): its fixtures become generic fixtures. */
	bool bUsable = true;
	/** Why it cannot be used, or the modes the scene asks for that it lacks; empty otherwise. */
	FString Problem;
};

/** What an .mvr file holds and what importing it would do, shown before anything changes (the MV3 review). */
struct FStageMvrImportReview
{
	FGuid ReviewId;
	FString SourceFile;
	/** "MVR 1.6 from Vectorworks 2025: 2 layers; 48 fixtures, 6 trusses; 4 GDTF types". */
	FString Description;

	int32 Fixtures = 0;
	int32 Trusses = 0;
	int32 Layers = 0;
	int32 FocusPoints = 0;
	/** Scene objects, supports, video screens and projectors: listed, not placed in this version. */
	int32 OtherObjects = 0;
	/** Fixtures that would be placed as a generic fixture because their type or mode is not usable (an estimate; the plan decides). */
	int32 GenericFixtures = 0;

	TArray<FStageMvrTypeLine> Types;
	/** The parser's and the geometry reader's notes. */
	TArray<FString> Warnings;
	/** Why the file is refused; empty when it can be imported. */
	TArray<FString> Errors;

	bool CanImport() const { return Errors.IsEmpty() && (Fixtures > 0 || Trusses > 0); }
};

/** How an import ended, for the status bar and the dialog. */
struct FStageMvrImportOutcome
{
	FString SourceFile;
	bool bSucceeded = false;
	/** "Imported plot.mvr: 48 fixtures, 6 truss runs, 2 groups." or why nothing was placed. */
	FText Message;
	/** The plan's notes (generic fixtures, renumbered IDs, skipped trusses...), user-facing. */
	TArray<FString> Notes;
	int32 Fixtures = 0;
	int32 Trusses = 0;
	int32 Groups = 0;
};

DECLARE_MULTICAST_DELEGATE(FOnStageMvrImportChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStageMvrImportFinished, const FStageMvrImportOutcome& /*Outcome*/);
