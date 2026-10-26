// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/ShowControlTypes.h"

/** One fixture as the patch math sees it. Key identifies it to the caller (a fixture ID, an index). */
struct FStageDMXPatchSlot
{
	int32 Key = 0;
	FStageDMXPatch Patch;
	/** Channels the fixture's mode uses; 0 = the fixture takes no DMX. */
	int32 Footprint = 0;
};

/** A patch the planner chose for the slot with Key. */
struct FStageDMXPatchPlanEntry
{
	int32 Key = 0;
	FStageDMXPatch Patch;
};

/** How full one universe is, for the Patch panel's overview. */
struct FStageDMXUniverseUsage
{
	int32 Universe = 0;
	/** Channels at least one fixture uses (overlaps count once). */
	int32 UsedChannels = 0;
	int32 Fixtures = 0;
	/** Fixtures in this universe that overlap another or run past channel 512. */
	int32 Conflicts = 0;
};

/**
 * Pure DMX address allocation (Docs/ADR/0019-dmx-patch-and-network-input.md §2). No world, no actors: UStageDMXPatchSubsystem
 * feeds it slots and applies the results through the controller, and the tests call it directly.
 *
 * A fixture occupies [Address, Address + Footprint - 1] in its universe. Overlaps are allowed, as on a console, but reported;
 * a range running past channel 512 is reported too (the channels past 512 are never sent or read).
 * Every function is deterministic: equal input, equal output, whatever the order of the slots.
 */
namespace StageDMXPatching
{
	inline constexpr int32 UniverseSize = 512;
	inline constexpr int32 MaxUniverse = 63999;

	/** Whether a fixture of Footprint channels fits at Address (1..512) without running past channel 512. */
	MODULARSCENEBUILDER_API bool Fits(int32 Address, int32 Footprint);

	/** The last start address a fixture of Footprint channels fits at, or 0 for an impossible footprint. */
	MODULARSCENEBUILDER_API int32 GetLastStartAddress(int32 Footprint);

	/**
	 * The first range of Footprint free channels at or after Start (universe, then address), searching later universes when
	 * Start's is full. Unpatched slots and slots without a footprint occupy nothing. Unpatched result for an impossible
	 * footprint or when no universe up to MaxUniverse has room.
	 */
	MODULARSCENEBUILDER_API FStageDMXPatch FindFree(TConstArrayView<FStageDMXPatchSlot> Occupied, int32 Footprint, FStageDMXPatch Start);

	/**
	 * Keys of the patched slots that overlap another slot of their universe or run past channel 512, ascending. A sweep over
	 * the slots sorted by address: O(n log n).
	 */
	MODULARSCENEBUILDER_API TArray<int32> FindConflicts(TConstArrayView<FStageDMXPatchSlot> Slots);

	/**
	 * Patches ToPatch in their order, each at the first free fit after the previous one, starting at Start: "patch these
	 * fixtures from 2.001". Occupied are the fixtures that keep their patch (the ones being patched must not be in it).
	 * Slots without a footprint are skipped. All or nothing: empty when any of them finds no room.
	 */
	MODULARSCENEBUILDER_API TArray<FStageDMXPatchPlanEntry> PlanConsecutive(TConstArrayView<FStageDMXPatchSlot> Occupied,
		TConstArrayView<FStageDMXPatchSlot> ToPatch, FStageDMXPatch Start);

	/**
	 * New patches that remove every conflict: in each overlap the fixture that starts first (lower key on a tie) keeps its
	 * address and the others move to the first free fit, from their own universe on; ranges past channel 512 move too.
	 * Unpatched and footprint-less slots are left alone. Empty when there is nothing to repair or no room for every move.
	 */
	MODULARSCENEBUILDER_API TArray<FStageDMXPatchPlanEntry> PlanConflictRepair(TConstArrayView<FStageDMXPatchSlot> Slots);

	/** Usage of every universe with at least one patched slot, ascending by universe. */
	MODULARSCENEBUILDER_API TArray<FStageDMXUniverseUsage> SummarizeUsage(TConstArrayView<FStageDMXPatchSlot> Slots);
}
