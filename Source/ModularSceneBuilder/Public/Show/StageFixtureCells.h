// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

/** One control of a fixture cell (a pixel of a pixel bar, a lamp of a blinder). */
enum class EStageCellChannel : uint8
{
	Dimmer,
	ColorR,
	ColorG,
	ColorB,
	ColorW,
	ColorA,
	/** Composite inspector row over the cell's R/G/B, like StageCraft.Attribute.ColorRGB. Not stored. */
	ColorRGB,
	Count
};

/**
 * Per-cell fixture attributes (Docs/ADR/0020-professional-fixture-suite.md §1). Each cell control is a registered gameplay tag
 * under StageCraft.Attribute (StageCraft.Attribute.Cell.01.ColorR ... Cell.32.ColorRGB), so cells are ordinary attributes:
 * cues record and fade them, scenes save them, DMX addresses them and the inspector lists them, with no path of their own.
 */
namespace StageFixtureCells
{
	/** Cells a fixture can have. Every cell's tags exist, so the limit is fixed at compile time. */
	constexpr int32 MaxCells = 32;

	/** Tag of a cell control; CellIndex is 0-based. Invalid tag for an out-of-range cell or channel. */
	MODULARSCENEBUILDER_API FGameplayTag GetCellAttribute(int32 CellIndex, EStageCellChannel Channel);

	/** Inverse of GetCellAttribute: false for tags that are not cell controls. */
	MODULARSCENEBUILDER_API bool ParseCellAttribute(const FGameplayTag& Attribute, int32& OutCellIndex, EStageCellChannel& OutChannel);

	MODULARSCENEBUILDER_API bool IsCellAttribute(const FGameplayTag& Attribute);
}
