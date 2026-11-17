// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Mvr/StageMvrItemOrigin.h"
#include "Show/ShowControlTypes.h"

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

/**
 * What a re-import does with items of the same plot that the file no longer has (Docs/ADR/0034 §18). The review asks; Remove is
 * what the dialog offers first, because the file is the designer's current plot. Either way it is one undo step.
 */
enum class EStageMvrMissingItems : uint8
{
	Remove,
	Keep,
};

/** An item on the stage that came from an MVR file, as plain values (StageMvrStageReader reads them; the planner compares them). */
struct FStageMvrExistingItem
{
	FGuid MvrId;
	/** The item's StageCraft identity: an update keeps it, so undo steps recorded before still find the item. */
	FGuid InstanceId;
	bool bFixture = false;
	/** The catalog item it is now (a type change is a different one). */
	TWeakObjectPtr<class UBaseItemData> Item;
	FTransform Transform = FTransform::Identity;
	/** Its label as the user sees it (the catalog name when it has none of its own). */
	FString Label;
	int32 FixtureId = 0;
	FStageDMXPatch Patch;
	/** A truss run's path (actor space) and its sections; empty / 1 for anything else. */
	TArray<FVector> TrussPoints;
	int32 Quantity = 1;
	/** Locked in the Outliner or inspector: a re-import keeps where it is (a lock forbids moving and resizing, not deleting). */
	bool bLocked = false;
	FStageMvrItemOrigin Origin;
};

/** What an import needs to know about the stage it goes into, read on the game thread when the import is asked for and confirmed. */
struct FStageMvrStage
{
	/** Items with an MVR UUID. */
	TArray<FStageMvrExistingItem> Items;
	/** Every fixture ID in use on the stage. */
	TSet<int32> UsedFixtureIds;
	/** The truss MVR trusses are built with, and the fixture for fixtures without a usable type. Either may be null. */
	TWeakObjectPtr<class UStageSpanData> TrussItem;
	TWeakObjectPtr<class UBaseItemData> GenericFixture;
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

	/**
	 * Against the stage when the file was read (Docs/ADR/0034 §18): fixtures and trusses new to it, already on it and changed by the
	 * file or not, and items of the same plot (same layer or file) the file no longer has. The confirm compares again.
	 */
	int32 NewItems = 0;
	int32 ChangedItems = 0;
	int32 UnchangedItems = 0;
	int32 MissingItems = 0;
	/** Fixtures and trusses that carry MVR data StageCraft does not use; it is kept on them and written back on export. */
	int32 PreservedItems = 0;

	TArray<FStageMvrTypeLine> Types;
	/** The parser's and the geometry reader's notes. */
	TArray<FString> Warnings;
	/** Why the file is refused; empty when it can be imported. */
	TArray<FString> Errors;

	bool CanImport() const { return Errors.IsEmpty() && (Fixtures > 0 || Trusses > 0); }

	/** Some of the file's items are on the stage already: the import updates instead of only placing. */
	bool IsReimport() const { return ChangedItems + UnchangedItems + MissingItems > 0; }

	/** Whether confirming would change the stage, with missing items handled as Missing says. */
	bool HasChanges(EStageMvrMissingItems Missing) const { return NewItems + ChangedItems > 0 || (Missing == EStageMvrMissingItems::Remove && MissingItems > 0); }
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
	/** Re-import: items placed new, updated in place, removed, and matched but unchanged. */
	int32 Added = 0;
	int32 Updated = 0;
	int32 Removed = 0;
	int32 Unchanged = 0;
};

DECLARE_MULTICAST_DELEGATE(FOnStageMvrImportChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStageMvrImportFinished, const FStageMvrImportOutcome& /*Outcome*/);
