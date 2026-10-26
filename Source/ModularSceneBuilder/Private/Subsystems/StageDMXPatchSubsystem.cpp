// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageDMXPatchSubsystem.h"

#include "Actors/LightingFixtureActor.h"
#include "Engine/World.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageDMXPatchSubsystem)

bool UStageDMXPatchSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Like the show it serves: play sessions only, where fixtures register.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageDMXPatchSubsystem::Deinitialize()
{
	Fixtures.Reset();
	UniverseIndex.Reset();
	OnPatchChanged.Clear();
	Super::Deinitialize();
}

void UStageDMXPatchSubsystem::RegisterFixture(ALightingFixtureActor* Fixture)
{
	if (!Fixture || Fixtures.Contains(Fixture))
	{
		return;
	}

	// Patched before it joins the registry, so FindNextFreePatch does not see its own (absent) range.
	if (bAutoPatchNewFixtures && !Fixture->GetPatch().IsPatched() && Fixture->GetDMXFootprint() > 0)
	{
		const FStageDMXPatch Free = FindNextFreePatch(Fixture->GetDMXFootprint());
		if (Free.IsPatched())
		{
			Fixture->SetPatch(Free); // Reports back through NotifyPatchChanged, which ignores it: it is not registered yet.
		}
		else
		{
			UE_LOG(LogStageCraft, Warning, TEXT("DMX patch: no room for %s (%d channels); it stays unpatched."),
				*Fixture->GetName(), Fixture->GetDMXFootprint());
		}
	}

	Fixtures.Add(Fixture);
	UE_LOG(LogStageCraft, Log, TEXT("DMX patch: fixture %d (%s) at %d.%03d, %d channels."),
		Fixture->GetFixtureId(), *Fixture->GetName(), Fixture->GetPatch().Universe, Fixture->GetPatch().Address, Fixture->GetDMXFootprint());
	HandlePatchChanged();
}

void UStageDMXPatchSubsystem::UnregisterFixture(ALightingFixtureActor* Fixture)
{
	if (Fixtures.Remove(Fixture) > 0)
	{
		HandlePatchChanged();
	}
}

void UStageDMXPatchSubsystem::NotifyPatchChanged(ALightingFixtureActor& Fixture)
{
	if (Fixtures.Contains(&Fixture))
	{
		HandlePatchChanged();
	}
}

void UStageDMXPatchSubsystem::HandlePatchChanged()
{
	bIndexStale = true;
	++PatchRevision;
	OnPatchChanged.Broadcast();
}

FStageDMXPatch UStageDMXPatchSubsystem::FindNextFreePatch(int32 Footprint, int32 StartUniverse, int32 StartAddress) const
{
	FStageDMXPatch Start;
	Start.Universe = StartUniverse;
	Start.Address = StartAddress;
	return StageDMXPatching::FindFree(MakeSlots(), Footprint, Start);
}

TArray<int32> UStageDMXPatchSubsystem::FindPatchConflicts() const
{
	TArray<int32> FixtureIds;
	for (const int32 Key : StageDMXPatching::FindConflicts(MakeSlots()))
	{
		FixtureIds.Add(Fixtures[Key]->GetFixtureId());
	}
	FixtureIds.Sort();
	return FixtureIds;
}

TArray<int32> UStageDMXPatchSubsystem::GetPatchedUniverses() const
{
	RebuildIndexIfStale();
	TArray<int32> Universes;
	UniverseIndex.GetKeys(Universes);
	Universes.Sort();
	return Universes;
}

TConstArrayView<TWeakObjectPtr<ALightingFixtureActor>> UStageDMXPatchSubsystem::GetFixturesInUniverse(int32 Universe) const
{
	RebuildIndexIfStale();
	const TArray<TWeakObjectPtr<ALightingFixtureActor>>* Found = UniverseIndex.Find(Universe);
	return Found ? TConstArrayView<TWeakObjectPtr<ALightingFixtureActor>>(*Found) : TConstArrayView<TWeakObjectPtr<ALightingFixtureActor>>();
}

TArray<FStageDMXUniverseUsage> UStageDMXPatchSubsystem::GetUniverseUsage() const
{
	return StageDMXPatching::SummarizeUsage(MakeSlots());
}

TArray<FStageDMXPatchAssignment> UStageDMXPatchSubsystem::PlanUnpatched() const
{
	TArray<ALightingFixtureActor*> Unpatched;
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && !Fixture->GetPatch().IsPatched() && Fixture->GetDMXFootprint() > 0)
		{
			Unpatched.Add(Fixture.Get());
		}
	}
	Unpatched.Sort([](const ALightingFixtureActor& A, const ALightingFixtureActor& B) { return A.GetFixtureId() < B.GetFixtureId(); });

	// Each fills the first gap from 1.001, so unpatched fixtures close the holes before new universes are opened.
	TArray<FStageDMXPatchAssignment> Plan;
	TArray<FStageDMXPatchSlot> Occupied = MakeSlots(Unpatched);
	FStageDMXPatch Start;
	Start.Universe = 1;
	Start.Address = 1;
	for (ALightingFixtureActor* Fixture : Unpatched)
	{
		const FStageDMXPatch Free = StageDMXPatching::FindFree(Occupied, Fixture->GetDMXFootprint(), Start);
		if (!Free.IsPatched())
		{
			return {};
		}
		Plan.Add({ Fixture, Free });
		Occupied.Add({ INDEX_NONE, Free, Fixture->GetDMXFootprint() });
	}
	return Plan;
}

TArray<FStageDMXPatchAssignment> UStageDMXPatchSubsystem::PlanConsecutive(TConstArrayView<ALightingFixtureActor*> ToPatch, const FStageDMXPatch& Start) const
{
	TArray<ALightingFixtureActor*> Ordered;
	for (ALightingFixtureActor* Fixture : ToPatch)
	{
		if (IsValid(Fixture) && Fixtures.Contains(Fixture) && Fixture->GetDMXFootprint() > 0)
		{
			Ordered.AddUnique(Fixture);
		}
	}
	Ordered.Sort([](const ALightingFixtureActor& A, const ALightingFixtureActor& B) { return A.GetFixtureId() < B.GetFixtureId(); });

	TArray<FStageDMXPatchSlot> Moving;
	for (int32 Index = 0; Index < Ordered.Num(); ++Index)
	{
		Moving.Add({ Index, FStageDMXPatch(), Ordered[Index]->GetDMXFootprint() });
	}

	TArray<FStageDMXPatchAssignment> Plan;
	for (const FStageDMXPatchPlanEntry& Entry : StageDMXPatching::PlanConsecutive(MakeSlots(Ordered), Moving, Start))
	{
		Plan.Add({ Ordered[Entry.Key], Entry.Patch });
	}
	return Plan;
}

TArray<FStageDMXPatchAssignment> UStageDMXPatchSubsystem::PlanConflictRepair() const
{
	return ToAssignments(StageDMXPatching::PlanConflictRepair(MakeSlots()));
}

TArray<FStageDMXPatchSlot> UStageDMXPatchSubsystem::MakeSlots(TConstArrayView<ALightingFixtureActor*> Skip) const
{
	TArray<FStageDMXPatchSlot> Slots;
	Slots.Reserve(Fixtures.Num());
	for (int32 Index = 0; Index < Fixtures.Num(); ++Index)
	{
		const ALightingFixtureActor* Fixture = Fixtures[Index].Get();
		if (Fixture && !Skip.Contains(Fixture))
		{
			Slots.Add({ Index, Fixture->GetPatch(), Fixture->GetDMXFootprint() });
		}
	}
	return Slots;
}

TArray<FStageDMXPatchAssignment> UStageDMXPatchSubsystem::ToAssignments(TConstArrayView<FStageDMXPatchPlanEntry> Plan) const
{
	TArray<FStageDMXPatchAssignment> Assignments;
	Assignments.Reserve(Plan.Num());
	for (const FStageDMXPatchPlanEntry& Entry : Plan)
	{
		Assignments.Add({ Fixtures[Entry.Key], Entry.Patch });
	}
	return Assignments;
}

void UStageDMXPatchSubsystem::RebuildIndexIfStale() const
{
	if (!bIndexStale)
	{
		return;
	}
	bIndexStale = false;
	UniverseIndex.Reset();
	for (const TWeakObjectPtr<ALightingFixtureActor>& Fixture : Fixtures)
	{
		if (Fixture.IsValid() && Fixture->GetPatch().IsPatched())
		{
			UniverseIndex.FindOrAdd(Fixture->GetPatch().Universe).Add(Fixture);
		}
	}
}
