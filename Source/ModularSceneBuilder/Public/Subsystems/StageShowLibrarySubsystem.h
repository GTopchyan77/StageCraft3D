// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/ShowControlTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "StageShowLibrarySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageFixtureGroupsChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStagePalettesChanged);

/**
 * The show's pools: fixture groups (Docs/ADR/0025-programmer-and-output-mix.md §5) and palettes (Docs/ADR/0028-palettes-and-references.md);
 * effects join it in a later step. Show data that belongs to the stage, saved in scenes (format v7), with no rules beyond
 * validation, so it is plain state.
 *
 * Writers: AModularPlayerController's group and palette requests, undo and redo, and scene load. Every entry goes through
 * StageFixtureGroupRules / StagePaletteRules. A new stage starts with the starter palettes. Views and UShowControlSubsystem (which
 * re-mixes the fixtures that follow a palette) bind OnGroupsChanged / OnPalettesChanged. Never ticks.
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

	// --- Palettes ---

	UFUNCTION(BlueprintPure, Category = "StageCraft|Palettes")
	const TArray<FStagePalette>& GetPalettes() const { return Palettes; }

	const FStagePalette* FindPalette(const FGuid& PaletteId) const;

	/**
	 * Adds Content as a new palette with a fresh id (Content.Id is ignored), named "Palette n" when unnamed, after StagePaletteRules.
	 * Returns its id, or an invalid id (nothing added) when nothing valid is left to hold or the pool is full.
	 */
	FGuid AddPalette(const FStagePalette& Content);

	/** Replaces an existing palette's values and colour; its id, name and kind stay. False for an unknown palette or no valid content. */
	bool SetPaletteContent(const FGuid& PaletteId, const FStagePalette& Content);

	/** False for an unknown palette or an empty name (after trimming). */
	bool RenamePalette(const FGuid& PaletteId, const FText& Name);

	/** Removes the palette only; turning references to it into values is the caller's (AModularPlayerController::RequestDeletePalette). */
	bool RemovePalette(const FGuid& PaletteId);

	/** Replaces the whole pool (scene load, undo); sanitized by StagePaletteRules::SanitizePalettes. */
	void SetPalettes(const TArray<FStagePalette>& InPalettes);

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Palettes")
	FOnStagePalettesChanged OnPalettesChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End UWorldSubsystem Interface

private:
	FStageFixtureGroup* FindGroupMutable(const FGuid& GroupId);
	FStagePalette* FindPaletteMutable(const FGuid& PaletteId);

	TArray<FStageFixtureGroup> Groups;
	TArray<FStagePalette> Palettes;
};
