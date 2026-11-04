// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "StageEffectTypes.generated.h"

/**
 * How one edit spreads over several selected fixtures (Docs/ADR/0025-programmer-and-output-mix.md §4), as a console's fan.
 * The change made on the primary fixture is scaled per fixture by StageShowMix::FanFactor, in selection order. Effects scale
 * their size the same way (Docs/ADR/0027-effects-engine.md §2).
 */
UENUM(BlueprintType)
enum class EStageFanMode : uint8
{
	/** Every fixture takes the value set. */
	Off,
	/** From minus the change on the first fixture to plus the change on the last; the middle keeps its value. */
	Linear,
	/** The ends take the full change, the middle none. */
	V		UMETA(DisplayName = "V"),
	/** The middle takes the full change, the ends none. */
	InvertedV	UMETA(DisplayName = "Inverted V"),
};

/** The shape an effect repeats once per cycle (Docs/ADR/0027 §2). */
UENUM(BlueprintType)
enum class EStageEffectWaveform : uint8
{
	Sine,
	/** A sine a quarter cycle ahead: starts at its top. */
	Cosine,
	/** On for the duty fraction of each cycle, then off: chases and strobes. */
	Square,
	/** Rises from bottom to top, then jumps back. */
	RampUp		UMETA(DisplayName = "Ramp Up"),
	/** Falls from top to bottom, then jumps back. */
	RampDown	UMETA(DisplayName = "Ramp Down"),
	/** Rises and falls in straight lines. */
	Triangle,
	/** A new random level every cycle, the same on every run (seeded by the fixture's place and the cycle). */
	Random,
	/** Pan and tilt a quarter cycle apart. On anything but position it is a sine. */
	Circle,
	/** Tilt at twice pan's rate. On anything but position it is a sine. */
	Figure8		UMETA(DisplayName = "Figure 8"),
};

/** What an effect moves (Docs/ADR/0027 §2). */
UENUM(BlueprintType)
enum class EStageEffectTarget : uint8
{
	/** Every intensity attribute (dimmer, cell dimmers), between Low and High of each fixture's range. Replaces the static value. */
	Intensity,
	/** Pan and tilt, Size degrees either side of the static position. Added to the static value. */
	Position,
	/** The colour, from ColorA to ColorB or round the colour wheel, matched by each fixture. Replaces the static value. */
	Color,
	/** One beam or focus attribute (Attribute), between Low and High of each fixture's range. Replaces the static value. */
	Beam,
};

/** Which position axes a single-axis waveform drives. Circle and Figure 8 always drive both. */
UENUM(BlueprintType)
enum class EStageEffectAxis : uint8
{
	Pan,
	Tilt,
	/** Both together: a diagonal. */
	Both,
};

UENUM(BlueprintType)
enum class EStageEffectColorMode : uint8
{
	/** From ColorA (bottom of the wave) to ColorB (top). */
	Crossfade,
	/** Round the hue circle once per cycle, at full saturation. */
	Rainbow,
};

/** Which way the pattern travels along the fixtures. */
UENUM(BlueprintType)
enum class EStageEffectDirection : uint8
{
	/** From the first fixture to the last. */
	Forward,
	/** From the last fixture to the first. */
	Reverse,
	/** Forward for one cycle, back for the next. */
	Bounce,
};

/**
 * One effect's settings (Docs/ADR/0027-effects-engine.md §2): what it moves, its shape and speed, and how it spreads over its
 * fixtures. Values are sanitized by StageEffectRules::SanitizeEffect wherever they enter (request, file, undo step).
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEffect
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	FText Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	EStageEffectTarget Target = EStageEffectTarget::Intensity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	EStageEffectWaveform Waveform = EStageEffectWaveform::Sine;

	/** Beam target only: the beam or focus attribute it moves (StageEffectRules::IsBeamAttribute). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (Categories = "StageCraft.Attribute"))
	FGameplayTag Attribute;

	/** Position target, single-axis waveforms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	EStageEffectAxis Axis = EStageEffectAxis::Both;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	EStageEffectColorMode ColorMode = EStageEffectColorMode::Crossfade;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	FLinearColor ColorA = FLinearColor::Red;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	FLinearColor ColorB = FLinearColor::Blue;

	/** Intensity and Beam: the bottom and top of the wave, as fractions 0..1 of each fixture's range. Low may exceed High (inverted). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Low = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float High = 1.f;

	/** Position: degrees either side of the static position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "0.0", Units = "deg"))
	float Size = 30.f;

	/** Cycles per second (BPM / 60). 0 holds the pattern still. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "0.0", Units = "Hz"))
	float RateHz = 0.5f;

	/** Square: the fraction of each cycle that is on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Duty = 0.5f;

	/** Where in the cycle the first fixture starts, in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (Units = "deg"))
	float Phase = 0.f;

	/** How far apart in the cycle the fixtures are, in degrees across all of them: 0 = in step, 360 = one wave across them (a chase). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (Units = "deg"))
	float Spread = 360.f;

	/** The fixtures split into this many equal parts, every other one mirrored, so the pattern runs symmetrically. 1 = none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "1"))
	int32 Wings = 1;

	/** Fixtures that move together, as one step of the spread. 1 = every fixture on its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect", meta = (ClampMin = "1"))
	int32 Blocks = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	EStageEffectDirection Direction = EStageEffectDirection::Forward;

	/** Scales the size per fixture in order (StageShowMix::FanFactor); Off = the same size on every fixture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Effect")
	EStageFanMode SizeFan = EStageFanMode::Off;

	/** Every setting equal (names compared exactly). */
	bool Equals(const FStageEffect& Other) const;
};

/**
 * An effect running on fixtures (Docs/ADR/0027 §3): in the programmer now, in cues with F4b. Fixtures are named by FixtureId in
 * the order they were selected, which is the order the pattern runs along. Id identifies the entry for edits and stop.
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageEffectEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Effect")
	FGuid Id;

	UPROPERTY(BlueprintReadOnly, Category = "Effect")
	FStageEffect Effect;

	UPROPERTY(BlueprintReadOnly, Category = "Effect")
	TArray<int32> FixtureIds;
};

/** What valid effects are (Docs/ADR/0027 §5). Pure, any thread; covered by StageCraft.Show.EffectRules. */
namespace StageEffectRules
{
	/** Above this the frame rate cannot show the wave; a 25 Hz square is already a fast strobe. */
	inline constexpr float MaxRateHz = 25.f;
	inline constexpr float MaxSizeDegrees = 270.f;
	inline constexpr float MaxPhaseDegrees = 3600.f;
	inline constexpr int32 MaxWings = 16;
	inline constexpr int32 MaxBlocks = 512;
	/** Effects one programmer (or, with F4b, one cue) can hold. */
	inline constexpr int32 MaxEffects = 64;
	inline constexpr int32 MaxFixturesPerEffect = 4096;
	inline constexpr int32 MaxNameLength = 64;

	MODULARSCENEBUILDER_API float HzToBPM(float Hz);
	MODULARSCENEBUILDER_API float BPMToHz(float BPM);

	/** A beam or focus attribute (StagePaletteRules kinds Beam and Focus) an effect may move: a stored StageCraft.Attribute tag. */
	MODULARSCENEBUILDER_API bool IsBeamAttribute(const FGameplayTag& Attribute);

	/**
	 * Effect with every value made valid: finite, clamped to the bounds above, Duty/Low/High to 0..1, colours to 0..1 and opaque,
	 * Wings and Blocks at least 1, the name trimmed and cut. A Beam effect without a valid attribute becomes Zoom.
	 */
	MODULARSCENEBUILDER_API FStageEffect SanitizeEffect(const FStageEffect& Effect);

	/**
	 * Repairs entries from an untrusted source (a file, an undo step, a request): drops entries without a valid id, later duplicates,
	 * entries past MaxEffects; keeps positive unique fixture IDs (at most MaxFixturesPerEffect) and drops entries left with none;
	 * sanitizes each effect. Returns how many entries and fixture IDs were dropped.
	 */
	MODULARSCENEBUILDER_API int32 SanitizeEntries(TArray<FStageEffectEntry>& Entries);

	/** The one-click effects of the Effects panel (Docs/ADR/0027 §6), named. */
	MODULARSCENEBUILDER_API TArray<FStageEffect> MakeQuickEffects();
}
