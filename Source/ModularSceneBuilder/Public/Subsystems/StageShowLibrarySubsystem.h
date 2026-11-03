// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/ShowControlTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageShowLibrarySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageFixtureGroupsChanged);

/**
 * The show's pools (Docs/ADR/0025-programmer-and-output-mix.md §5): fixture groups now; palettes and effects join it in later steps.
 * Show data that belongs to the stage, saved in scenes (format v6), with no rules beyond validation, so it is plain state.
 *
 * Writers: AModularPlayerController's group requests (record, update, rename, delete), undo and redo, and scene load. Every entry
 * goes through StageFixtureGroupRules. Views bind OnGroupsChanged. Never ticks.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageShowLibrarySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "StageCraft|Groups")
	const TArray<FStageFixtureGroup>& GetGroups() const { return Groups; }

	const FStageFixtureGroup* FindGroup(const FGuid& GroupId) const;

	/**
	 * Adds a group of FixtureIds (selection order kept), named Name or "Group n" when empty. Returns its id, or an invalid id (nothing
	 * added) when there are no valid fixtures or the pool is full (StageFixtureGroupRules::MaxGroups).
	 */
	FGuid AddGroup(const FText& Name, TConstArrayView<int32> FixtureIds);

	/** Replaces an existing group's fixtures. False for an unknown group or no valid fixtures. */
	bool SetGroupFixtures(const FGuid& GroupId, TConstArrayView<int32> FixtureIds);

	/** False for an unknown group or an empty name (after trimming). */
	bool RenameGroup(const FGuid& GroupId, const FText& Name);

	bool RemoveGroup(const FGuid& GroupId);

	/** Replaces the whole pool (scene load, undo); sanitized by StageFixtureGroupRules::SanitizeGroups. */
	void SetGroups(const TArray<FStageFixtureGroup>& InGroups);

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Groups")
	FOnStageFixtureGroupsChanged OnGroupsChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;
	//~ End UWorldSubsystem Interface

private:
	FStageFixtureGroup* FindGroupMutable(const FGuid& GroupId);

	TArray<FStageFixtureGroup> Groups;
};
