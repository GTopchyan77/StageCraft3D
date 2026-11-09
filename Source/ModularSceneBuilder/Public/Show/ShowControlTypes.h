// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Show/StageEffectTypes.h"
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

/**
 * How a fixture's pan and tilt channels map onto the wire (Docs/ADR/0019 §10.1), for a fixture hung so its axes run the other way
 * or sideways. They change only what WriteDMX sends and how ReadDMX reads: the fixture's attributes, its beam on the stage and
 * the cues stay as they are, as with the invert and swap options of a console's patch. SaveGame fields, like FStageDMXPatch.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageDMXPatchOptions
{
	GENERATED_BODY()

	/** The pan channel runs the other way (0 for full pan and back). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "DMX")
	bool bInvertPan = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "DMX")
	bool bInvertTilt = false;

	/** The pan channel carries the tilt and the tilt channel the pan. Only on a fixture with both. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "DMX")
	bool bSwapPanTilt = false;

	bool IsDefault() const { return !bInvertPan && !bInvertTilt && !bSwapPanTilt; }

	bool operator==(const FStageDMXPatchOptions& Other) const = default;
};

/**
 * Attribute values of one fixture inside a cue or the programmer. Fixtures are referenced by FixtureId so cues survive save/load
 * and re-spawns.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FShowCueFixtureState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	int32 FixtureId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (Categories = "StageCraft.Attribute"))
	TMap<FGameplayTag, float> Attributes;

	/**
	 * Attributes that follow a palette (Docs/ADR/0028-palettes-and-references.md §3): the palette's current value wins, and the value
	 * in Attributes is only the fallback when the palette is gone. Every key also has a value in Attributes
	 * (StagePaletteRules::SanitizeReferences drops any that does not).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (Categories = "StageCraft.Attribute"))
	TMap<FGameplayTag, FGuid> PaletteRefs;
};

/**
 * Which feature of a fixture an attribute belongs to: a palette's kind (Docs/ADR/0028-palettes-and-references.md §2), as a console's
 * preset types, and the features a cue can time separately (Docs/ADR/0026 §2).
 */
UENUM(BlueprintType)
enum class EStagePaletteKind : uint8
{
	/** Any attribute. */
	All,
	/** The dimmer and the cell dimmers. */
	Intensity,
	/** Pan and tilt. */
	Position,
	/** Emitters and filters, colour temperature, CTO, the colour wheel and the cell colours. */
	Color,
	/** Shutter, gobos, prisms, iris and blades: every attribute no other kind claims. */
	Beam,
	/** Zoom and frost. */
	Focus,
};

/** What starts a cue (Docs/ADR/0026-cue-timing-tracking-playbacks.md §2). */
UENUM(BlueprintType)
enum class EStageCueTrigger : uint8
{
	/** A Go from the user. */
	Go,
	/** Starts when the cue before it has finished. */
	Follow,
	/** Starts WaitTime seconds after the cue before it was started. */
	Wait,
};

/** How a cue's values move from start to target. */
UENUM(BlueprintType)
enum class EStageCueCurve : uint8
{
	Linear,
	/** Eases in and out (smoothstep). */
	SCurve	UMETA(DisplayName = "S-Curve"),
};

/** How a store records a cue (Docs/ADR/0026 §3). */
UENUM(BlueprintType)
enum class EStageCueStoreMode : uint8
{
	/** Every fixture's whole look, as a blocked cue that replaces the cue (what every store did before tracking). */
	Look,
	/** What the programmer holds, merged into the cue; it tracks into the cues that follow. */
	Track,
	/** Track, and the next cue keeps the old values, so the change stops at this cue. */
	CueOnly,
};

/**
 * A cue's timing beyond its In time (FShowCue::FadeTime), Docs/ADR/0026 §2. Sanitized by StageCueRules::SanitizeTiming; resolved
 * per attribute by StageCueTiming::GetAttributeTiming.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageCueTiming
{
	GENERATED_BODY()

	/** Seconds after the cue starts before anything in it moves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (ClampMin = "0.0", Units = "s"))
	float Delay = 0.f;

	/** Whether intensity going down has its own time (OutTime); otherwise it uses the In time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	bool bHasOutTime = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (ClampMin = "0.0", Units = "s", EditCondition = "bHasOutTime"))
	float OutTime = 0.f;

	/** Fade times per feature (never All), beating the In time; intensity going down still takes OutTime when set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	TMap<EStagePaletteKind, float> FeatureTimes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	EStageCueTrigger Trigger = EStageCueTrigger::Go;

	/** Trigger Wait: seconds after the cue before this one was started. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (ClampMin = "0.0", Units = "s"))
	float WaitTime = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	EStageCueCurve Curve = EStageCueCurve::Linear;

	/** Same timing (times compared to the cue number resolution's precision). */
	bool Equals(const FStageCueTiming& Other) const;
};

/**
 * One lighting state of a tracking cue list (Docs/ADR/0026 §3): a cue holds only what it changes; the look at a cue is every earlier
 * cue's values back to the nearest blocked cue, with this cue's on top (StageCueTracking::StateAt). Unrecorded attributes are
 * simply absent from the maps.
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

	/** The In time: every attribute's fade unless Timing gives it another. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue", meta = (ClampMin = "0.0", Units = "s"))
	float FadeTime = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	FStageCueTiming Timing;

	/** Inherits nothing from earlier cues: fixtures and attributes it does not hold go to their home look. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	bool bBlock = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	TArray<FShowCueFixtureState> Fixtures;

	/**
	 * Effects this cue starts or changes (Docs/ADR/0027-effects-engine.md §5); they track into the cues that follow until a cue
	 * replaces (same id) or stops them. Sanitized by StageCueRules::SanitizeCueEffects.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	TArray<FStageEffectEntry> Effects;

	/** Ids of effects running from earlier cues that end at this cue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cue")
	TArray<FGuid> EffectStops;
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

	/** Every time clamped like SanitizeFadeTime; feature times only for Intensity..Focus (All and unknown kinds dropped). Returns how many were dropped. */
	MODULARSCENEBUILDER_API int32 SanitizeTiming(FStageCueTiming& Timing);

	/** Trimmed and cut to MaxLabelLength. */
	MODULARSCENEBUILDER_API FText SanitizeLabel(const FText& Label);

	/**
	 * Repairs a cue list from an untrusted source (a file, an undo step): drops cues with invalid numbers and later duplicates
	 * of a number, rounds numbers, sanitizes fades and labels, and sorts by number. Returns how many cues were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizeCueList(TArray<FShowCue>& Cues);

	/**
	 * The cue's effects and stops made valid (StageEffectRules::SanitizeEntries and SanitizeStops). SanitizeCueList applies it to
	 * every cue; a reader that reports drops calls it itself. Returns how many entries, fixture IDs and stops were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizeCueEffects(FShowCue& Cue);
}

/** A cue request a player makes, judged by AStageCraftGameModeBase::EvaluateCueRequest. */
UENUM(BlueprintType)
enum class EStageCueRequest : uint8
{
	/** Record the current look as a cue (a new number, or re-record an existing one). */
	Store,
	/** Change an existing cue's label, timing or block. */
	Update,
	Delete,
	/** Playback: Go, Back or Go To a cue. */
	Play,
	/** Switch between the internal cue list and external DMX. */
	SetControlSource,
	/** Record into, clear or rename a playback (the number is the playback's index). Undoable. */
	PlaybackEdit,
	/** Move a playback's fader, flash it, step it (Go) or release it (the number is the playback's index). Live, not undoable. */
	PlaybackLive,
	/** Grand Master and Blackout. Live, not undoable. */
	Masters,
	/** Start, change or stop an effect in the programmer (Docs/ADR/0027). Undoable; refused under External DMX. */
	Effects,
};

/**
 * One playback fader's content (Docs/ADR/0026 §6): its own tracking cue list, recorded from the programmer and stepped by the
 * playback's Go. Saved in scenes; its fader level, flash and position are live state (UShowControlSubsystem) and are not.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStagePlayback
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Playback")
	FText Name;

	UPROPERTY(BlueprintReadOnly, Category = "Playback")
	TArray<FShowCue> Cues;
};

/** What valid playbacks are (Docs/ADR/0026 §6). Pure, any thread. */
namespace StagePlaybackRules
{
	/** Playback faders on the desk. */
	inline constexpr int32 Count = 10;
	inline constexpr int32 MaxCuesPerPlayback = 999;

	/**
	 * Repairs playbacks from an untrusted source (a file, an undo step): exactly Count of them (missing ones empty, extra ones dropped),
	 * names trimmed and cut like cue labels, each cue list sanitized like the main one and cut to MaxCuesPerPlayback. Returns how many
	 * cues and playbacks were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizePlaybacks(TArray<FStagePlayback>& Playbacks);
}

/** Who owns the fixture attributes right now. */
UENUM(BlueprintType)
enum class EStageControlSource : uint8
{
	/** Internal cue list drives fixtures; the DMX bridge (if any) outputs the result. StageCraft acts as the console. */
	Internal	UMETA(DisplayName = "Internal (Cue List)"),
	/** Incoming DMX drives fixtures; cues are disabled. StageCraft acts as a visualiser for an external console. */
	External	UMETA(DisplayName = "External (DMX In)"),
};

// EStageFanMode lives in Show/StageEffectTypes.h: effects spread their size with it too (Docs/ADR/0027 §2).

/**
 * A saved fixture selection, as on a console's group pool (Docs/ADR/0025 §5). Fixtures are named by FixtureId, in the order they
 * were selected, so a group survives save/load and re-spawns and keeps its fan order. Validated by StageFixtureGroupRules.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageFixtureGroup
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Group")
	FGuid Id;

	UPROPERTY(BlueprintReadOnly, Category = "Group")
	FText Name;

	UPROPERTY(BlueprintReadOnly, Category = "Group")
	TArray<int32> FixtureIds;
};

/** What a valid group list is. Applied by UStageShowLibrarySubsystem on every entry (record, rename, load, undo). Pure, any thread. */
namespace StageFixtureGroupRules
{
	inline constexpr int32 MaxGroups = 999;
	inline constexpr int32 MaxFixturesPerGroup = 4096;
	inline constexpr int32 MaxNameLength = 64;

	/** Trimmed and cut to MaxNameLength; empty stays empty (the caller names it). */
	MODULARSCENEBUILDER_API FText SanitizeName(const FText& Name);

	/** Positive ids only, each once, in their first order, at most MaxFixturesPerGroup. */
	MODULARSCENEBUILDER_API TArray<int32> SanitizeFixtureIds(TConstArrayView<int32> FixtureIds);

	/**
	 * Repairs a group list from an untrusted source (a file, an undo step): drops groups without a valid id, later duplicates of an
	 * id and groups past MaxGroups, gives unnamed groups a name and sanitizes the fixture lists. Returns how many groups were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizeGroups(TArray<FStageFixtureGroup>& Groups);
}

/** How a palette is recorded from the selection. */
UENUM(BlueprintType)
enum class EStagePaletteScope : uint8
{
	/** One look for any fixture: the primary fixture's values (a colour palette: its colour, mixed by each fixture). */
	Global,
	/** Each selected fixture's own values, exactly. */
	PerFixture,
};

/**
 * A named look for one feature, as on a console's palette pool (Docs/ADR/0028 §2). Applied to a selection, it puts references into
 * the programmer; cues keep them, so re-recording the palette changes every cue that uses it. Validated by StagePaletteRules.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStagePalette
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Palette")
	FGuid Id;

	UPROPERTY(BlueprintReadOnly, Category = "Palette")
	FText Name;

	UPROPERTY(BlueprintReadOnly, Category = "Palette")
	EStagePaletteKind Kind = EStagePaletteKind::All;

	/** Colour palettes: whether Color holds a look. */
	UPROPERTY(BlueprintReadOnly, Category = "Palette")
	bool bHasColor = false;

	/** Colour palettes: the colour each fixture mixes with its own system (ALightingFixtureActor::MatchColor). RGB 0..1. */
	UPROPERTY(BlueprintReadOnly, Category = "Palette")
	FLinearColor Color = FLinearColor::White;

	/** Values for any fixture that has the attribute (never colour attributes, whose meaning differs between colour systems). */
	UPROPERTY(BlueprintReadOnly, Category = "Palette", meta = (Categories = "StageCraft.Attribute"))
	TMap<FGameplayTag, float> GlobalValues;

	/** Exact values per fixture ID; they win over the global values. One entry per fixture. */
	UPROPERTY(BlueprintReadOnly, Category = "Palette")
	TArray<FShowCueFixtureState> FixtureValues;
};

/** What a valid palette list is and how a reference resolves (Docs/ADR/0028). Pure, any thread. */
namespace StagePaletteRules
{
	inline constexpr int32 MaxPalettes = 999;
	inline constexpr int32 MaxFixturesPerPalette = 4096;
	/** A 64-cell RGBWA fixture with dimmers has 384 cell values plus its own (ADR 0033 G4). */
	inline constexpr int32 MaxAttributes = 512;
	inline constexpr int32 MaxNameLength = 64;

	/** The kind an attribute belongs to (never All). Unknown attributes count as Beam. */
	MODULARSCENEBUILDER_API EStagePaletteKind GetAttributeKind(const FGameplayTag& Attribute);

	/** A stored StageCraft.Attribute tag (composite rows such as ColorRGB are not) that a palette of Kind may hold. */
	MODULARSCENEBUILDER_API bool AllowsAttribute(EStagePaletteKind Kind, const FGameplayTag& Attribute);

	/** Only the values a palette of Kind may hold, finite, at most MaxAttributes. */
	MODULARSCENEBUILDER_API TMap<FGameplayTag, float> FilterValues(EStagePaletteKind Kind, const TMap<FGameplayTag, float>& Values);

	/** True when the palette holds something to apply (values or a colour). */
	MODULARSCENEBUILDER_API bool HasContent(const FStagePalette& Palette);

	/**
	 * The value Palette gives FixtureId's Attribute: the fixture's own value, else the global value, else (colour palettes) the
	 * attribute in ColorMatch, the fixture's MatchColor of Palette.Color (null when the caller has no fixture). False when none.
	 */
	MODULARSCENEBUILDER_API bool FindPaletteValue(const FStagePalette& Palette, int32 FixtureId, const FGameplayTag& Attribute, const TMap<FGameplayTag, float>* ColorMatch, float& OutValue);

	/** Drops references with an invalid id or without a value in State.Attributes. Returns how many were dropped. */
	MODULARSCENEBUILDER_API int32 SanitizeReferences(FShowCueFixtureState& State);

	/** Trimmed and cut to MaxNameLength; empty stays empty. */
	MODULARSCENEBUILDER_API FText SanitizeName(const FText& Name);

	/**
	 * Repairs a palette list from an untrusted source (a file, an undo step): drops palettes without a valid id, later duplicates and
	 * palettes past MaxPalettes; names unnamed ones; keeps only the values of each palette's kind (finite, bounded), positive unique
	 * fixture IDs and a finite colour. Returns how many palettes and entries were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizePalettes(TArray<FStagePalette>& Palettes);

	/** The cue numbers whose fixtures refer to PaletteId, ascending. */
	MODULARSCENEBUILDER_API TArray<float> FindCuesUsing(const TArray<FShowCue>& Cues, const FGuid& PaletteId);

	/** The palettes every new stage starts with (Docs/ADR/0028 §5). Fixed ids, so they are the same in every stage. */
	MODULARSCENEBUILDER_API TArray<FStagePalette> MakeStarterPalettes();
}
