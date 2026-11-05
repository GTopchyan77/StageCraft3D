// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/ShowControlTypes.h"

/** One fixture's row of an imported fixture schedule, as written in the file. */
struct FStagePatchScheduleRow
{
	/** 1-based line in the file, for messages. */
	int32 Line = 0;
	int32 FixtureId = 0;
	/** Universe and address both empty in the file = unpatch (0.000). */
	FStageDMXPatch Patch;
	/** Set only when the file has the column. */
	TOptional<bool> bInvertPan;
	TOptional<bool> bInvertTilt;
	TOptional<bool> bSwapPanTilt;
};

/** Why a line or row was not taken. */
struct FStagePatchScheduleIssue
{
	/** 1-based line, or 0 for the whole file. */
	int32 Line = 0;
	FText Reason;
};

/** What a file held: the rows that read cleanly and the lines that did not. */
struct FStagePatchScheduleParse
{
	TArray<FStagePatchScheduleRow> Rows;
	TArray<FStagePatchScheduleIssue> Errors;
	/** False when no "Fixture ID" header row was found (or the file was refused whole); Rows is then empty. */
	bool bHeaderFound = false;
};

/** A stage fixture as the planner sees it. */
struct FStagePatchScheduleFixture
{
	int32 FixtureId = 0;
	/** Channels of its DMX mode; 0 = takes no DMX. */
	int32 Footprint = 0;
	FStageDMXPatch Patch;
	FStageDMXPatchOptions Options;
};

/** One fixture's new patch (and options) from the schedule. */
struct FStagePatchScheduleChange
{
	int32 FixtureId = 0;
	FStageDMXPatch Patch;
	FStageDMXPatchOptions Options;
	/** Whether the file set any option for this fixture (otherwise Options are the fixture's own). */
	bool bOptionsFromFile = false;
};

/** What applying a schedule would do. Nothing in it has been applied. */
struct FStagePatchSchedulePlan
{
	TArray<FStagePatchScheduleChange> Changes;
	/** Rows not taken, with the reason (unknown or duplicate fixture, a patch that does not fit). */
	TArray<FStagePatchScheduleIssue> Rejected;
	/** Rows that match the stage already. */
	int32 Unchanged = 0;
	/** Fixture IDs that would overlap another fixture or run past 512 once the plan is applied (allowed, as everywhere). */
	TArray<int32> Conflicts;
};

/**
 * A fixture schedule (a spreadsheet of fixture ID, universe and address) read into a patch plan (Docs/ADR/0019 §10.2): the
 * reverse of the patch sheet CSV (StagePatchSheet::ToCsv), which is the file it is meant to read back. Pure, so it is tested
 * without a world; the file is user input and every field is validated.
 */
namespace StagePatchSchedule
{
	/** Larger files are refused whole: a schedule of every fixture of a large show is well under 1 MB. */
	inline constexpr int32 MaxFileCharacters = 1024 * 1024;
	inline constexpr int32 MaxRows = 10000;
	inline constexpr int32 MaxFieldCharacters = 512;

	/**
	 * Reads Text: a UTF-8 byte order mark, CRLF or LF, comma or semicolon separated (taken from the header row), fields optionally
	 * in double quotes ("" for a quote). Lines before the header row (the sheet's title and summary) are skipped; the header row is
	 * the first with a "Fixture ID" field. Columns are found by name, in any order: Fixture ID, Universe and Address are
	 * required; Invert Pan, Invert Tilt and Swap Pan/Tilt are optional (Yes / No, True / False, 1 / 0, On / Off; empty is No).
	 * A leading apostrophe (the sheet's guard against spreadsheet formulas) is ignored. Empty lines are skipped. A row with a
	 * bad field is reported and left out; the rest still read.
	 */
	MODULARSCENEBUILDER_API FStagePatchScheduleParse Parse(FStringView Text);

	/**
	 * Checks Rows against the stage's fixtures. Rejected, each with its line: a fixture ID no fixture has; a fixture ID on more
	 * than one row (every such row: the file is ambiguous); a patch for a fixture that takes no DMX; an address its footprint
	 * does not fit at (StageDMXPatching::Fits). Rows equal to the stage are counted as Unchanged. Conflicts are computed over the
	 * whole stage as it would be after the plan.
	 */
	MODULARSCENEBUILDER_API FStagePatchSchedulePlan Plan(TConstArrayView<FStagePatchScheduleRow> Rows, TConstArrayView<FStagePatchScheduleFixture> Fixtures);
}
