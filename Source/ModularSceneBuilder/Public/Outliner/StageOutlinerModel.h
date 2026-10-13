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
}
