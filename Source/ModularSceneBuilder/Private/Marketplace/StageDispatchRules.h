// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"
#include "Marketplace/StageDispatchTypes.h"
#include "Services/StageAIToolTypes.h"

/** What is known when deciding whether a target can take an acquired stage (Docs/ADR/0022-dcc-bridge.md §11.2). */
struct FStageDispatchContext
{
	/** The publisher's flag as the listing shows it. The exporter decides again from the library package. */
	bool bListingAllowsExport = false;
	bool bBridgeRunning = false;
	bool bBridgeSending = false;
	TArray<FStageBridgeClientInfo> Clients;
	bool bToolBusy = false;
	TArray<FStageAIToolInfo> Tools;
};

/**
 * The one rule both the acquire dialog (for display) and the dispatcher (before acquiring and again before delivering) use.
 * Pure; no world, no UObjects; tested directly.
 */
namespace StageDispatchRules
{
	EStageDispatchAvailability EvaluateTarget(const FStageDispatchTarget& Target, const FStageDispatchContext& Context);

	/** A SaveFile target's path: absolute and ending in .glb. Other kinds need no path. */
	bool HasValidPath(const FStageDispatchTarget& Target);

	/** A short user-facing reason for an availability other than Available. */
	FText Describe(EStageDispatchAvailability Availability);
}
