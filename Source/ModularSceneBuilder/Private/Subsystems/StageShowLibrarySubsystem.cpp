// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageShowLibrarySubsystem.h"

#include "Engine/World.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageShowLibrarySubsystem)

bool UStageShowLibrarySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageShowLibrarySubsystem::Deinitialize()
{
	Groups.Reset();
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
