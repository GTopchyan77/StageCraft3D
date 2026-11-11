// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageSessionTypes.generated.h"

/**
 * Rules of one stage session, owned by the GameMode and handed to UStageSessionSubsystem when
 * play starts. Different modes (free sandbox, budgeted challenge, venue with a power limit) are
 * different GameMode defaults, not different code.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSessionRules
{
	GENERATED_BODY()

	/** Off: a sandbox where every item and parameter is usable regardless of ownership. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bEnforceEntitlements = true;

	/** Placed items allowed at once. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session", meta = (ClampMin = "0"))
	int32 MaxPlacedItems = 0;

	/** Venue power budget across all placed gear. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session", meta = (ClampMin = "0.0", DisplayName = "Max Total Power (W)"))
	float MaxTotalPowerWatts = 0.f;

	/** Total weight of all placed gear. 0 = unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session", meta = (ClampMin = "0.0", Units = "kg"))
	float MaxTotalWeightKg = 0.f;

	/** Users may import their own 3D models into the Library (drag and drop, Import Model...). Off for curated sessions. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowModelImport = true;

	/** Users may import GDTF fixture files into the device library (Docs/ADR/0033 §7). Off for curated sessions. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowFixtureImport = true;

	/**
	 * Users may import MVR scenes (Docs/ADR/0034 §6): their fixtures and trusses are placed and their GDTF types added to the
	 * library. Free in every tier (M1). Off for curated sessions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowMvrImport = true;

	/**
	 * Users may export the stage as an MVR scene (Docs/ADR/0034 §7): fixtures with their GDTF files, patch and IDs, trusses and
	 * other items as boxes. Pro in the tiers (ADR 0034 §2); the entitlement check comes with Step 6. Off for curated sessions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowMvrExport = true;

	/**
	 * Users may sign in to online fixture sources (GDTF Share, Docs/ADR/0033 §18) and download fixture files from them for review. A
	 * downloaded file is added to the library only through the fixture import review, so bAllowFixtureImport must also be on. Off
	 * for curated or offline sessions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowOnlineFixtureSources = true;

	/**
	 * MIDI and OSC controllers may run the desk's live controls (Docs/ADR/0035 §4): cue GO / BACK, playbacks, masters. Each command is
	 * still judged by EvaluateCueRequest. Off for curated sessions. Pro in the tiers (ADR 0034 §2); the entitlement check comes with Step 6.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowRemoteControl = true;

	/**
	 * May the Web Remote serve its page and run the desk's live controls from phones and tablets (Docs/ADR/0036)? Each command is
	 * still judged by EvaluateCueRequest. Off for curated sessions. Pro in the tiers (ADR 0034 §2); the entitlement check comes with Step 6.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Session")
	bool bAllowWebRemote = true;
};

/** Live totals of the stage, recomputed whenever an item is added, removed or changes type. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageSessionStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Session")
	int32 PlacedItems = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Session", meta = (DisplayName = "Total Power (W)"))
	float TotalPowerWatts = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Session", meta = (Units = "kg"))
	float TotalWeightKg = 0.f;
};
