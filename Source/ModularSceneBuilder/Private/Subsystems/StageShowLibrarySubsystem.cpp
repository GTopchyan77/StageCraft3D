// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageShowLibrarySubsystem.h"

#include "Engine/World.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageShowLibrarySubsystem)

bool UStageShowLibrarySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageShowLibrarySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// A new stage starts with the starter palettes (Docs/ADR/0028 §5); a loaded stage replaces them with its own (SetPalettes).
	Palettes = StagePaletteRules::MakeStarterPalettes();
}

void UStageShowLibrarySubsystem::Deinitialize()
{
	Groups.Reset();
	Palettes.Reset();
	Super::Deinitialize();
}

const FStageFixtureGroup* UStageShowLibrarySubsystem::FindGroup(const FGuid& GroupId) const
{
	return Groups.FindByPredicate([&GroupId](const FStageFixtureGroup& Group) { return Group.Id == GroupId; });
}

FStageFixtureGroup* UStageShowLibrarySubsystem::FindGroupMutable(const FGuid& GroupId)
{
	return Groups.FindByPredicate([&GroupId](const FStageFixtureGroup& Group) { return Group.Id == GroupId; });
}

FGuid UStageShowLibrarySubsystem::AddGroup(const FText& Name, TConstArrayView<int32> FixtureIds)
{
	TArray<int32> Kept = StageFixtureGroupRules::SanitizeFixtureIds(FixtureIds);
	if (Kept.IsEmpty() || Groups.Num() >= StageFixtureGroupRules::MaxGroups)
	{
		return FGuid();
	}
	FStageFixtureGroup& Group = Groups.AddDefaulted_GetRef();
	Group.Id = FGuid::NewGuid();
	Group.Name = StageFixtureGroupRules::SanitizeName(Name);
	if (Group.Name.IsEmpty())
	{
		Group.Name = FText::Format(NSLOCTEXT("StageCraftGroups", "UnnamedGroup", "Group {0}"), FText::AsNumber(Groups.Num()));
	}
	Group.FixtureIds = MoveTemp(Kept);
	const FGuid Id = Group.Id;
	OnGroupsChanged.Broadcast();
	return Id;
}

bool UStageShowLibrarySubsystem::SetGroupFixtures(const FGuid& GroupId, TConstArrayView<int32> FixtureIds)
{
	FStageFixtureGroup* Group = FindGroupMutable(GroupId);
	TArray<int32> Kept = StageFixtureGroupRules::SanitizeFixtureIds(FixtureIds);
	if (!Group || Kept.IsEmpty())
	{
		return false;
	}
	Group->FixtureIds = MoveTemp(Kept);
	OnGroupsChanged.Broadcast();
	return true;
}

bool UStageShowLibrarySubsystem::RenameGroup(const FGuid& GroupId, const FText& Name)
{
	FStageFixtureGroup* Group = FindGroupMutable(GroupId);
	const FText Sanitized = StageFixtureGroupRules::SanitizeName(Name);
	if (!Group || Sanitized.IsEmpty())
	{
		return false;
	}
	Group->Name = Sanitized;
	OnGroupsChanged.Broadcast();
	return true;
}

bool UStageShowLibrarySubsystem::RemoveGroup(const FGuid& GroupId)
{
	if (Groups.RemoveAll([&GroupId](const FStageFixtureGroup& Group) { return Group.Id == GroupId; }) == 0)
	{
		return false;
	}
	OnGroupsChanged.Broadcast();
	return true;
}

void UStageShowLibrarySubsystem::SetGroups(const TArray<FStageFixtureGroup>& InGroups)
{
	Groups = InGroups;
	const int32 Dropped = StageFixtureGroupRules::SanitizeGroups(Groups);
	if (Dropped > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Groups: %d group(s) without a valid id, duplicated or past the pool size were dropped."), Dropped);
	}
	OnGroupsChanged.Broadcast();
}

// --- Palettes ---

const FStagePalette* UStageShowLibrarySubsystem::FindPalette(const FGuid& PaletteId) const
{
	return Palettes.FindByPredicate([&PaletteId](const FStagePalette& Palette) { return Palette.Id == PaletteId; });
}

FStagePalette* UStageShowLibrarySubsystem::FindPaletteMutable(const FGuid& PaletteId)
{
	return Palettes.FindByPredicate([&PaletteId](const FStagePalette& Palette) { return Palette.Id == PaletteId; });
}

FGuid UStageShowLibrarySubsystem::AddPalette(const FStagePalette& Content)
{
	if (Palettes.Num() >= StagePaletteRules::MaxPalettes)
	{
		return FGuid();
	}
	// Sanitized as a list of one, so a new palette passes exactly the rules a loaded one does.
	TArray<FStagePalette> Candidate = { Content };
	Candidate[0].Id = FGuid::NewGuid();
	Candidate[0].Name = StagePaletteRules::SanitizeName(Content.Name);
	if (Candidate[0].Name.IsEmpty())
	{
		Candidate[0].Name = FText::Format(NSLOCTEXT("StageCraftPalettes", "UnnamedPalette", "Palette {0}"), FText::AsNumber(Palettes.Num() + 1));
	}
	StagePaletteRules::SanitizePalettes(Candidate);
	if (Candidate.IsEmpty() || !StagePaletteRules::HasContent(Candidate[0]))
	{
		return FGuid();
	}
	const FGuid Id = Candidate[0].Id;
	Palettes.Add(MoveTemp(Candidate[0]));
	OnPalettesChanged.Broadcast();
	return Id;
}

bool UStageShowLibrarySubsystem::SetPaletteContent(const FGuid& PaletteId, const FStagePalette& Content)
{
	FStagePalette* Palette = FindPaletteMutable(PaletteId);
	if (!Palette)
	{
		return false;
	}
	TArray<FStagePalette> Candidate = { Content };
	Candidate[0].Id = Palette->Id;
	Candidate[0].Name = Palette->Name;
	Candidate[0].Kind = Palette->Kind;
	StagePaletteRules::SanitizePalettes(Candidate);
	if (Candidate.IsEmpty() || !StagePaletteRules::HasContent(Candidate[0]))
	{
		return false;
	}
	*Palette = MoveTemp(Candidate[0]);
	OnPalettesChanged.Broadcast();
	return true;
}

bool UStageShowLibrarySubsystem::RenamePalette(const FGuid& PaletteId, const FText& Name)
{
	FStagePalette* Palette = FindPaletteMutable(PaletteId);
	const FText Sanitized = StagePaletteRules::SanitizeName(Name);
	if (!Palette || Sanitized.IsEmpty())
	{
		return false;
	}
	Palette->Name = Sanitized;
	OnPalettesChanged.Broadcast();
	return true;
}

bool UStageShowLibrarySubsystem::RemovePalette(const FGuid& PaletteId)
{
	if (Palettes.RemoveAll([&PaletteId](const FStagePalette& Palette) { return Palette.Id == PaletteId; }) == 0)
	{
		return false;
	}
	OnPalettesChanged.Broadcast();
	return true;
}

void UStageShowLibrarySubsystem::SetPalettes(const TArray<FStagePalette>& InPalettes)
{
	Palettes = InPalettes;
	const int32 Dropped = StagePaletteRules::SanitizePalettes(Palettes);
	if (Dropped > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Palettes: %d invalid palette(s) or value(s) were dropped."), Dropped);
	}
	OnPalettesChanged.Broadcast();
}
