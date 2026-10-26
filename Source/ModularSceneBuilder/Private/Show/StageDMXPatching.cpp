// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXPatching.h"

#include "Algo/BinarySearch.h"

namespace
{
	/** A patched slot's occupied channels. Last may lie past 512 (a range that does not fit). */
	struct FDMXRange
	{
		int32 Key = 0;
		int32 Universe = 0;
		int32 First = 0;
		int32 Last = 0;
	};

	bool IsValidFootprint(int32 Footprint)
	{
		return Footprint > 0 && Footprint <= StageDMXPatching::UniverseSize;
	}

	bool MakeRange(const FStageDMXPatchSlot& Slot, FDMXRange& OutRange)
	{
		if (!Slot.Patch.IsPatched() || Slot.Footprint <= 0)
		{
			return false;
		}
		OutRange = { Slot.Key, Slot.Patch.Universe, Slot.Patch.Address, Slot.Patch.Address + Slot.Footprint - 1 };
		return true;
	}

	bool RangeLess(const FDMXRange& A, const FDMXRange& B)
	{
		if (A.Universe != B.Universe)
		{
			return A.Universe < B.Universe;
		}
		if (A.First != B.First)
		{
			return A.First < B.First;
		}
		return A.Key < B.Key;
	}

	/** Sorted by RangeLess, so a universe's ranges are contiguous. */
	TArray<FDMXRange> MakeSortedRanges(TConstArrayView<FStageDMXPatchSlot> Slots)
	{
		TArray<FDMXRange> Ranges;
		Ranges.Reserve(Slots.Num());
		for (const FStageDMXPatchSlot& Slot : Slots)
		{
			FDMXRange Range;
			if (MakeRange(Slot, Range))
			{
				Ranges.Add(Range);
			}
		}
		Ranges.Sort(RangeLess);
		return Ranges;
	}

	/** FindFree over ranges already sorted by RangeLess. */
	FStageDMXPatch FindFreeInSorted(TConstArrayView<FDMXRange> Sorted, int32 Footprint, const FStageDMXPatch& Start)
	{
		FStageDMXPatch Result;
		if (!IsValidFootprint(Footprint))
		{
			return Result;
		}

		int32 Universe = FMath::Clamp(Start.Universe, 1, StageDMXPatching::MaxUniverse);
		int32 Candidate = FMath::Clamp(Start.Address, 1, StageDMXPatching::UniverseSize);

		// The first range of Universe: ranges are sorted, so a binary search finds where the universe begins.
		int32 Index = Algo::LowerBoundBy(Sorted, Universe, [](const FDMXRange& Range) { return Range.Universe; });
		for (; Universe <= StageDMXPatching::MaxUniverse; ++Universe, Candidate = 1)
		{
			for (; Index < Sorted.Num() && Sorted[Index].Universe == Universe; ++Index)
			{
				const FDMXRange& Range = Sorted[Index];
				if (Range.Last < Candidate)
				{
					continue;
				}
				if (Range.First > Candidate + Footprint - 1)
				{
					break; // The gap before this range is wide enough.
				}
				Candidate = Range.Last + 1;
			}

			if (StageDMXPatching::Fits(Candidate, Footprint))
			{
				Result.Universe = Universe;
				Result.Address = Candidate;
				return Result;
			}

			// Skip the rest of this universe's ranges (after the break above) before moving on.
			while (Index < Sorted.Num() && Sorted[Index].Universe == Universe)
			{
				++Index;
			}
		}
		return Result;
	}

	void InsertSorted(TArray<FDMXRange>& Sorted, const FDMXRange& Range)
	{
		Sorted.Insert(Range, Algo::UpperBound(Sorted, Range, RangeLess));
	}

	FStageDMXPatch NextAfter(const FStageDMXPatch& Patch, int32 Footprint)
	{
		FStageDMXPatch Next;
		Next.Universe = Patch.Universe;
		Next.Address = Patch.Address + Footprint;
		if (Next.Address > StageDMXPatching::UniverseSize)
		{
			Next.Universe = Patch.Universe + 1;
			Next.Address = 1;
		}
		return Next;
	}
}

bool StageDMXPatching::Fits(int32 Address, int32 Footprint)
{
	return IsValidFootprint(Footprint) && Address >= 1 && Address <= GetLastStartAddress(Footprint);
}

int32 StageDMXPatching::GetLastStartAddress(int32 Footprint)
{
	return IsValidFootprint(Footprint) ? UniverseSize - Footprint + 1 : 0;
}

FStageDMXPatch StageDMXPatching::FindFree(TConstArrayView<FStageDMXPatchSlot> Occupied, int32 Footprint, FStageDMXPatch Start)
{
	return FindFreeInSorted(MakeSortedRanges(Occupied), Footprint, Start);
}

TArray<int32> StageDMXPatching::FindConflicts(TConstArrayView<FStageDMXPatchSlot> Slots)
{
	const TArray<FDMXRange> Sorted = MakeSortedRanges(Slots);
	TSet<int32> Conflicts;

	// Within a universe, a range overlaps an earlier one exactly when it starts at or before the furthest end seen so far, and
	// then it overlaps the range that reaches that end. Any earlier range it overlaps is marked by the same rule in turn.
	int32 Universe = 0;
	int32 FurthestLast = 0;
	int32 FurthestKey = 0;
	for (const FDMXRange& Range : Sorted)
	{
		if (Range.Universe != Universe)
		{
			Universe = Range.Universe;
			FurthestLast = 0;
		}

		if (Range.First <= FurthestLast)
		{
			Conflicts.Add(Range.Key);
			Conflicts.Add(FurthestKey);
		}
		if (Range.Last > UniverseSize)
		{
			Conflicts.Add(Range.Key);
		}
		if (Range.Last > FurthestLast)
		{
			FurthestLast = Range.Last;
			FurthestKey = Range.Key;
		}
	}

	TArray<int32> Result = Conflicts.Array();
	Result.Sort();
	return Result;
}

TArray<FStageDMXPatchPlanEntry> StageDMXPatching::PlanConsecutive(TConstArrayView<FStageDMXPatchSlot> Occupied,
	TConstArrayView<FStageDMXPatchSlot> ToPatch, FStageDMXPatch Start)
{
	TArray<FDMXRange> Sorted = MakeSortedRanges(Occupied);
	TArray<FStageDMXPatchPlanEntry> Plan;
	Plan.Reserve(ToPatch.Num());

	FStageDMXPatch Cursor = Start;
	for (const FStageDMXPatchSlot& Slot : ToPatch)
	{
		if (Slot.Footprint <= 0)
		{
			continue;
		}

		const FStageDMXPatch Free = FindFreeInSorted(Sorted, Slot.Footprint, Cursor);
		if (!Free.IsPatched())
		{
			return {};
		}
		Plan.Add({ Slot.Key, Free });
		InsertSorted(Sorted, { Slot.Key, Free.Universe, Free.Address, Free.Address + Slot.Footprint - 1 });
		Cursor = NextAfter(Free, Slot.Footprint);
	}
	return Plan;
}

TArray<FStageDMXPatchPlanEntry> StageDMXPatching::PlanConflictRepair(TConstArrayView<FStageDMXPatchSlot> Slots)
{
	const TArray<FDMXRange> Sorted = MakeSortedRanges(Slots);

	// Greedy per universe in address order: a range stays when it fits and starts after everything kept before it.
	TArray<FDMXRange> Kept;
	TArray<FDMXRange> Moving;
	int32 Universe = 0;
	int32 KeptLast = 0;
	for (const FDMXRange& Range : Sorted)
	{
		if (Range.Universe != Universe)
		{
			Universe = Range.Universe;
			KeptLast = 0;
		}
		if (Range.Last <= UniverseSize && Range.First > KeptLast)
		{
			Kept.Add(Range);
			KeptLast = Range.Last;
		}
		else
		{
			Moving.Add(Range);
		}
	}

	TArray<FStageDMXPatchPlanEntry> Plan;
	for (const FDMXRange& Range : Moving)
	{
		const int32 Footprint = Range.Last - Range.First + 1;
		FStageDMXPatch Start;
		Start.Universe = Range.Universe;
		Start.Address = 1;
		const FStageDMXPatch Free = FindFreeInSorted(Kept, Footprint, Start);
		if (!Free.IsPatched())
		{
			return {};
		}
		Plan.Add({ Range.Key, Free });
		InsertSorted(Kept, { Range.Key, Free.Universe, Free.Address, Free.Address + Footprint - 1 });
	}
	return Plan;
}

TArray<FStageDMXUniverseUsage> StageDMXPatching::SummarizeUsage(TConstArrayView<FStageDMXPatchSlot> Slots)
{
	const TArray<FDMXRange> Sorted = MakeSortedRanges(Slots);
	const TSet<int32> Conflicts(FindConflicts(Slots));

	TArray<FStageDMXUniverseUsage> Result;
	TBitArray<> Used;
	for (const FDMXRange& Range : Sorted)
	{
		if (Result.IsEmpty() || Result.Last().Universe != Range.Universe)
		{
			Result.AddDefaulted_GetRef().Universe = Range.Universe;
			Used.Init(false, UniverseSize);
		}

		FStageDMXUniverseUsage& Usage = Result.Last();
		++Usage.Fixtures;
		Usage.Conflicts += Conflicts.Contains(Range.Key) ? 1 : 0;
		for (int32 Channel = Range.First; Channel <= FMath::Min(Range.Last, UniverseSize); ++Channel)
		{
			if (!Used[Channel - 1])
			{
				Used[Channel - 1] = true;
				++Usage.UsedChannels;
			}
		}
	}
	return Result;
}
