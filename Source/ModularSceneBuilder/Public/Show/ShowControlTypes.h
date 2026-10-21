// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ShowControlTypes.generated.h"

/**
 * Where a fixture's DMX mode starts. Universe 0 means unpatched.
 *
 * The fields are SaveGame because a fixture's Patch is: SaveGame serialization (undo snapshots, scene files) skips every member
 * of a struct that is not itself SaveGame, so without the flags the patch was silently dropped and re-auto-patched on every load
 * (found by StageCraft.Scene.RestoredState, STATE.md #40).
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageDMXPatch
{
	GENERATED_BODY()

	/** 1-based. Art-Net and sACN universes both map onto this number; the DMX bridge translates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "DMX", meta = (ClampMin = "0", ClampMax = "63999"))
	int32 Universe = 0;

	/** 1..512. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "DMX", meta = (ClampMin = "0", ClampMax = "512"))
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

/**
 * What a valid cue is (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.1). Applied by UShowControlSubsystem on every
 * entry (store, update, load, undo) and by the GameMode before a request runs. Pure, any thread.
 */
namespace StageCueRules
{
	/** Cue numbers are compared with this tolerance, so they are stored rounded to it. */
	inline constexpr float NumberResolution = 0.001f;
	inline constexpr float MinCueNumber = 0.001f;
	inline constexpr float MaxCueNumber = 9999.999f;
	inline constexpr float MaxFadeTime = 600.f;
	inline constexpr int32 MaxLabelLength = 256;

	/** Finite and within MinCueNumber..MaxCueNumber after rounding. */
	MODULARSCENEBUILDER_API bool IsValidCueNumber(float CueNumber);

	/** CueNumber rounded to NumberResolution (call IsValidCueNumber first). */
	MODULARSCENEBUILDER_API float RoundCueNumber(float CueNumber);

	/** Clamped to 0..MaxFadeTime; a non-finite time becomes 0. */
	MODULARSCENEBUILDER_API float SanitizeFadeTime(float FadeTime);

	/** Trimmed and cut to MaxLabelLength. */
	MODULARSCENEBUILDER_API FText SanitizeLabel(const FText& Label);

	/**
	 * Repairs a cue list from an untrusted source (a file, an undo step): drops cues with invalid numbers and later duplicates
	 * of a number, rounds numbers, sanitizes fades and labels, and sorts by number. Returns how many cues were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizeCueList(TArray<FShowCue>& Cues);
}

/** A cue request a player makes, judged by AStageCraftGameModeBase::EvaluateCueRequest. */
UENUM(BlueprintType)
enum class EStageCueRequest : uint8
{
	/** Record the current look as a cue (a new number, or re-record an existing one). */
	Store,
	/** Change an existing cue's label or fade time. */
	Update,
	Delete,
	/** Playback: Go, Back or Go To a cue. */
	Play,
	/** Switch between the internal cue list and external DMX. */
	SetControlSource,
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
