// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

/** One placed item as the outliner lists it. Plain values copied from the item, so the model is testable without a world. */
struct FStageOutlinerItem
{
	FGuid InstanceId;
	FText Label;
	/** The catalog item's display name ("Moving Head 300"). */
	FText ModelName;
	/** The catalog category (any depth under StageCraft.Category). */
	FGameplayTag Category;
	/** Catalog units (a span's segments; 1 otherwise). */
	int32 Quantity = 1;
	bool bHidden = false;
	bool bLocked = false;
};

/** How a flag is set across a group's items; drives the group's eye and lock toggles. */
enum class EStageOutlinerFlagState : uint8
{
	None,
	Some,
	All,
};

/** One discipline of the stage (Truss, Lighting, Audio...): the outliner's layer. */
struct FStageOutlinerGroup
{
	/** The top-level category, e.g. StageCraft.Category.Lighting. Invalid for items without a category ("Other"). */
	FGameplayTag Category;
	FText Label;
	TArray<FStageOutlinerItem> Items;
	EStageOutlinerFlagState Hidden = EStageOutlinerFlagState::None;
	EStageOutlinerFlagState Locked = EStageOutlinerFlagState::None;
};

/** One line of the outliner's list: a discipline header or one of its items, as indices into the groups it was listed from. */
struct FStageOutlinerRowRef
{
	int32 GroupIndex = INDEX_NONE;
	/** INDEX_NONE for the group's header row. */
	int32 ItemIndex = INDEX_NONE;
	/** Header rows: whether the group shows its items (always while filtering). */
	bool bExpanded = true;

	bool IsGroupRow() const { return ItemIndex == INDEX_NONE; }
};

/** What the outliner lists for a set of groups, a filter and the groups the user collapsed. */
struct FStageOutlinerListing
{
	TArray<FStageOutlinerRowRef> Rows;
	/** Items that pass the filter, whether their group is expanded or not. */
	int32 Listed = 0;
	/** Hidden and locked items on the whole stage, filtered or not. */
	int32 Hidden = 0;
	int32 Locked = 0;
};

/**
 * The outliner's hierarchy (Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §4): placed items grouped by discipline, the
 * way production drawings are layered (rigging, lighting, sound...). Groups are derived from each item's catalog category, so
 * they never need to be stored or kept in sync. Pure and deterministic; covered by the StageCraft.Outliner tests.
 */
namespace StageOutliner
{
	/**
	 * The discipline a catalog category belongs to: the direct child of StageCraft.Category on its path
	 * (StageCraft.Category.Lighting.MovingHead -> StageCraft.Category.Lighting). Invalid for no category, the root itself or a tag outside it.
	 */
	MODULARSCENEBUILDER_API FGameplayTag GetGroupCategory(const FGameplayTag& Category);

	/**
	 * Sort key of a discipline in the build order of a show: Stage, Truss, Rigging, Lighting, Audio, Screen, Cable, Prop, then any
	 * other discipline, then no category. Shared with the bill of materials so both list disciplines alike.
	 */
	MODULARSCENEBUILDER_API int32 GetGroupOrder(const FGameplayTag& GroupCategory);

	/** "Lighting", "Truss"...; "Other" for an invalid group. */
	MODULARSCENEBUILDER_API FText MakeGroupLabel(const FGameplayTag& GroupCategory);

	/**
	 * Groups Items by discipline. Groups follow the build order of a show (Stage, Truss, Rigging, Lighting, Audio, Screen, Cable,
	 * Prop), then other disciplines alphabetically, then Other. Items are in natural label order ("Spot 2" before "Spot 10"), then
	 * by id, so equal labels never swap places between rebuilds. Each group summarises its hidden and locked flags.
	 */
	MODULARSCENEBUILDER_API TArray<FStageOutlinerGroup> BuildGroups(TArray<FStageOutlinerItem> Items);

	/** Whether an item row passes the filter box: every word of Query in the label, model or group name (as the Library's search). */
	MODULARSCENEBUILDER_API bool MatchesFilter(const FStageOutlinerItem& Item, const FText& GroupLabel, const FString& Query);

	/**
	 * The rows of the list, in order: every group with an item that passes the filter, each followed by those items unless the
	 * group is collapsed. While filtering (a Query with more than spaces) every listed group is expanded, as in the Library's search.
	 */
	MODULARSCENEBUILDER_API FStageOutlinerListing BuildListing(TConstArrayView<FStageOutlinerGroup> Groups, const TSet<FGameplayTag>& CollapsedGroups, const FString& Query);
}
