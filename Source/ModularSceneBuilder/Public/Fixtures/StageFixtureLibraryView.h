// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "Fixtures/StageFixtureLibraryTypes.h"

/**
 * What the fixture-library views show (Docs/ADR/0033 §10), as plain text built from the library's data: the import report dialog
 * and the Library cards. Pure and game-thread only (it reads UObject names); the widgets only lay these out, so the tests check
 * the words and choices here.
 */
namespace StageFixtureLibraryView
{
	/** One DMX mode line of the import report. */
	struct FModeRow
	{
		/** "Standard (15 ch)". */
		FText Name;
		/** "11 mapped · 1 approximated · 3 at default", or why the mode cannot be added. */
		FText Detail;
		/** "At default: Focus1, Control1", "Approximated: Blade1A"; empty when there is nothing to name. */
		TArray<FText> Notes;
		bool bUsable = false;
	};

	/** The import report dialog's content for one review. */
	struct FReviewText
	{
		/** "StageCraft Test Spot". */
		FText Title;
		/** "Manufacturer: StageCraft · TestSpot.gdtf". */
		FText Source;
		/** "Adds 2 modes to the Library.", "Replaces StageCraft Test Spot in the Library: 2 modes.", or why it cannot be added. */
		FText Summary;
		TArray<FModeRow> Modes;
		TArray<FText> Warnings;
		TArray<FText> Errors;
		/** ADD, or REPLACE for a type the library holds. */
		FText ConfirmLabel;
		bool bCanConfirm = false;
	};

	FReviewText DescribeReview(const FStageFixtureImportReview& Review);

	/** One tile of the Library: a catalog item, or one GDTF fixture type standing for all its modes. */
	struct FCard
	{
		/** The item a click places: the item itself, or the type's first mode. */
		UBaseItemData* Item = nullptr;
		/** Set for a GDTF fixture type (the card offers REMOVE); invalid for every other item. */
		FGuid FixtureTypeId;
		/** Overrides the item's own name and subtitle for a type card ("StageCraft Test Spot", "GDTF · 2 modes"); empty otherwise. */
		FText Name;
		FText Detail;
		int32 ModeCount = 0;

		bool IsFixtureType() const { return FixtureTypeId.IsValid(); }
	};

	/**
	 * The Library's cards in catalog order: one per item, except that the modes of a library fixture type become one card, at the
	 * position of its first mode in the catalog. Null items are skipped.
	 */
	TArray<FCard> MakeCards(TConstArrayView<UBaseItemData*> Catalog, TConstArrayView<FStageFixtureLibraryType> Types);
}
