// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "Engine/World.h"
#include "Mvr/StageMvrImportTypes.h"

/**
 * Reads what an MVR import needs to know about a world's stage into plain values (Docs/ADR/0034 §6, §18): every item with an MVR
 * UUID (for re-import), every fixture ID in use, and the truss and fixture the Sketch window builds with (MVR trusses become runs of
 * that truss, fixtures without a usable type that fixture).
 *
 * Game thread: it reads placed actors and world subsystems; it changes nothing. Used by AModularPlayerController when an import is
 * asked for (the review) and confirmed (the plan), and by tests.
 */
namespace StageMvrStageReader
{
	FStageMvrStage Read(const UWorld& World);

	/** One placed item as the planner compares it. */
	FStageMvrExistingItem Describe(const AModularBaseActor& Item);
}
