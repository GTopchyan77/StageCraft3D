// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Widgets/SWidget.h"

/** How a menu entry behaves: a command, one choice of a group, an on/off option, or a submenu. */
enum class EStageMenuEntryKind : uint8
{
	Action,
	Radio,
	Toggle,
	SubMenu,
};

/**
 * One entry of a toolbar dropdown (UStageMenuButton), as data.
 *
 * Why data and not FMenuBuilder calls: the panels decide what their menus offer (which choice is checked, what is greyed out,
 * what a pick does) in pure functions that tests can call without a widget or a world, and a scripted run can read and pick
 * entries by label (StageCraft.Workspace.Menu). UStageMenuButton turns the description into the themed Slate popup.
 *
 * A menu is described fresh each time it opens, from the state at that moment, and picking an entry closes it, so nothing in it
 * needs to stay live. Execute is run once per pick; it must capture its owners weakly (the popup is Slate-owned and can outlive them)
 * and send writes through the owner's normal request path.
 */
struct FStageMenuEntry
{
	FText Label;
	FText Tip;
	EStageMenuEntryKind Kind = EStageMenuEntryKind::Action;
	/** Radio and Toggle entries: shown checked. */
	bool bChecked = false;
	/** False greys the entry out; picking it does nothing. */
	bool bEnabled = true;
	/** Runs when the entry is picked; unset for a submenu. */
	TFunction<void()> Execute;
	/** A submenu's entries. */
	TArray<FStageMenuEntry> SubEntries;
};

/** A titled group of entries; sections are separated by their headings, as in the viewport's View menu. */
struct FStageMenuSection
{
	FName Name;
	FText Heading;
	TArray<FStageMenuEntry> Entries;
};

/** A whole dropdown menu. */
struct FStageMenu
{
	TArray<FStageMenuSection> Sections;

	/** Adds a section and returns it, for its entries to be added. */
	FStageMenuSection& AddSection(FName Name, const FText& Heading);

	bool IsEmpty() const;
};

namespace StageMenu
{
	FStageMenuEntry MakeAction(const FText& Label, const FText& Tip, bool bEnabled, TFunction<void()> Execute);
	FStageMenuEntry MakeRadio(const FText& Label, const FText& Tip, bool bChecked, bool bEnabled, TFunction<void()> Execute);
	FStageMenuEntry MakeToggle(const FText& Label, const FText& Tip, bool bChecked, bool bEnabled, TFunction<void()> Execute);
	/** A submenu is greyed out when it has no entries. */
	FStageMenuEntry MakeSubMenu(const FText& Label, const FText& Tip, TArray<FStageMenuEntry> Entries);

	/**
	 * The entry at Path: labels separated by '/', compared ignoring case (a submenu's label, then one of its entries). Sections do
	 * not take part in the path. Null when there is none. Valid while Menu is.
	 */
	const FStageMenuEntry* Find(const FStageMenu& Menu, const FString& Path);

	/** Picks the entry at Path as a click would: false, and nothing runs, if it is missing, greyed out or a submenu. */
	bool Pick(const FStageMenu& Menu, const FString& Path);

	/** One line per entry for logs and scripted checks: "[Heading]", "(x) Lit", "( ) Unlit", "[x] Grid", "Frame", "> Deck", "-" (greyed). */
	FString Describe(const FStageMenu& Menu);

	/** The Slate popup for Menu (FMenuBuilder, closes on pick). Game thread. */
	TSharedRef<SWidget> MakeWidget(const FStageMenu& Menu);
}
