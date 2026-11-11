// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Mvr/StageMvrWriter.h"

/**
 * Reads the stage of a world into the plain values StageMvrExport::Write takes (Docs/ADR/0034 §7, MV4). Game thread: it reads
 * placed actors, the fixture library and the show library; it writes nothing.
 *
 * Rules:
 *  - Every placed item is written once. Lighting fixtures become MVR fixtures. Items whose catalog category is a truss
 *    (StageCraft.Category.Truss and below) become trusses: a span one Truss per leg, any other truss piece one Truss the size of its
 *    bounds. Every other item becomes a SceneObject the size of its bounds.
 *  - Ids: an item's MVR UUID when it was imported (so a round trip keeps them), else its StageCraft instance id, which scene files
 *    keep, so exporting the same stage again writes the same UUIDs. A repeated or missing id gets a new one. A truss's later legs
 *    get ids derived from the item's.
 *  - Layers: one per fixture group with a fixture on the stage, in group order and named after it; a fixture goes to the first
 *    group that holds it. Everything else goes to a layer named "StageCraft".
 *  - Fixture types: a GDTF library type is written with its library file under the name the user imported it as (made safe and
 *    unique), and the fixture's DMX mode. Built-in fixtures and session-only types are written without a GDTF type.
 *  - Items without bounds (nothing visible) are not written and counted.
 */
namespace StageMvrExportGather
{
	struct FGathered
	{
		StageMvrExport::FScene Scene;
		/** GDTF file name in the .mvr -> the library file to copy (read on the worker). */
		TMap<FString, FString> GdtfPaths;
		int32 Fixtures = 0;
		/** Truss items (each written as one Truss per leg). */
		int32 Trusses = 0;
		int32 OtherItems = 0;
		int32 Skipped = 0;
	};

	FGathered Gather(const class UWorld& World);

	/** The id of a truss's leg Leg (0 is the item's own id): the item's id with its last 32 bits mixed with the leg number. */
	FGuid LegId(const FGuid& ItemId, int32 Leg);
}
