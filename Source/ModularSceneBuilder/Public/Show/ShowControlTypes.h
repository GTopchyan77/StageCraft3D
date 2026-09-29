// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ShowControlTypes.generated.h"

/** Where a fixture's DMX mode starts. Universe 0 means unpatched. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageDMXPatch
{
	GENERATED_BODY()

	/** 1-based. Art-Net and sACN universes both map onto this number; the DMX bridge translates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "0", ClampMax = "63999"))
	int32 Universe = 0;

	/** 1..512. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "0", ClampMax = "512"))
	int32 Address = 0;

	bool IsPatched() const { return Universe > 0 && Address > 0; }

	bool operator==(const FStageDMXPatch& Other) const { return Universe == Other.Universe && Address == Other.Address; }
};

/** Attribute values of one fixture inside a cue. Fixtures are referenced by FixtureId so cues survive save/load and re-spawns. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FShowCueFixtureState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	int32 FixtureId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (Categories = "StageCraft.Attribute"))
	TMap<FGameplayTag, float> Attributes;
};

/**
 * One lighting state. Phase 5 records full snapshots ("cue only"); tracking (storing only
 * changed values and inheriting the rest from previous cues) is a later extension that needs
 * no format change because unrecorded attributes are simply absent from the map.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FShowCue
{
	GENERATED_BODY()

	/** Console-style cue number; decimals allowed for inserted cues (e.g. 2.5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (ClampMin = "0.0"))
	float Number = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	FText Label;

	/** Crossfade time for every attribute in the cue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (ClampMin = "0.0", Units = "s"))
	float FadeTime = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	TArray<FShowCueFixtureState> Fixtures;
};

/** Who owns the fixture attributes right now. */
UENUM(BlueprintType)
enum class EStageControlSource : uint8
{
	/** Internal cue list drives fixtures; the DMX bridge (if any) outputs the result. StageCraft acts as the console. */
	Internal	UMETA(DisplayName = "Internal (Cue List)"),
	/** Incoming DMX drives fixtures; cues are disabled. StageCraft acts as a visualiser for an external console. */
	External	UMETA(DisplayName = "External (DMX In)"),
};
