// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageEffectTypes.h"

#include "Data/StageParameterTypes.h"
#include "Show/ShowControlTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEffectTypes)

#define LOCTEXT_NAMESPACE "StageEffects"

bool FStageEffect::Equals(const FStageEffect& Other) const
{
	return Name.ToString().Equals(Other.Name.ToString(), ESearchCase::CaseSensitive) && Target == Other.Target && Waveform == Other.Waveform
		&& Attribute == Other.Attribute && Axis == Other.Axis && ColorMode == Other.ColorMode && ColorA == Other.ColorA && ColorB == Other.ColorB
		&& Low == Other.Low && High == Other.High && Size == Other.Size && RateHz == Other.RateHz && Duty == Other.Duty && Phase == Other.Phase
		&& Spread == Other.Spread && Wings == Other.Wings && Blocks == Other.Blocks && Direction == Other.Direction && SizeFan == Other.SizeFan
		&& Audio.Equals(Other.Audio);
}

bool FStageEffectAudio::Equals(const FStageEffectAudio& Other) const
{
	return Mode == Other.Mode && Band == Other.Band && Amount == Other.Amount && Modulates == Other.Modulates && BeatsPerCycle == Other.BeatsPerCycle
		&& Response == Other.Response;
}

bool FStageProgrammerEffects::Equals(const FStageProgrammerEffects& Other) const
{
	return StageEffectRules::AreEntriesEqual(Effects, Other.Effects) && Stops == Other.Stops;
}

namespace StageEffectRules
{
	namespace
	{
		float Finite(float Value, float Fallback)
		{
			return FMath::IsFinite(Value) ? Value : Fallback;
		}

		FLinearColor SanitizeColor(const FLinearColor& Color, const FLinearColor& Fallback)
		{
			if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G) || !FMath::IsFinite(Color.B))
			{
				return Fallback;
			}
			return FLinearColor(FMath::Clamp(Color.R, 0.f, 1.f), FMath::Clamp(Color.G, 0.f, 1.f), FMath::Clamp(Color.B, 0.f, 1.f), 1.f);
		}

		template <typename EnumType>
		EnumType SanitizeEnum(EnumType Value, EnumType Last, EnumType Fallback)
		{
			return static_cast<uint8>(Value) <= static_cast<uint8>(Last) ? Value : Fallback;
		}
	}

	float HzToBPM(float Hz)
	{
		return Hz * 60.f;
	}

	float BPMToHz(float BPM)
	{
		return BPM / 60.f;
	}

	bool IsBeamAttribute(const FGameplayTag& Attribute)
	{
		return StagePaletteRules::AllowsAttribute(EStagePaletteKind::Beam, Attribute) || StagePaletteRules::AllowsAttribute(EStagePaletteKind::Focus, Attribute);
	}

	FStageEffect SanitizeEffect(const FStageEffect& Effect)
	{
		const FStageEffect Defaults;
		FStageEffect Out = Effect;
		Out.Name = StagePaletteRules::SanitizeName(Effect.Name);
		Out.Target = SanitizeEnum(Effect.Target, EStageEffectTarget::Beam, Defaults.Target);
		Out.Waveform = SanitizeEnum(Effect.Waveform, EStageEffectWaveform::Figure8, Defaults.Waveform);
		Out.Axis = SanitizeEnum(Effect.Axis, EStageEffectAxis::Both, Defaults.Axis);
		Out.ColorMode = SanitizeEnum(Effect.ColorMode, EStageEffectColorMode::Rainbow, Defaults.ColorMode);
		Out.Direction = SanitizeEnum(Effect.Direction, EStageEffectDirection::Bounce, Defaults.Direction);
		Out.SizeFan = SanitizeEnum(Effect.SizeFan, EStageFanMode::InvertedV, Defaults.SizeFan);
		Out.ColorA = SanitizeColor(Effect.ColorA, Defaults.ColorA);
		Out.ColorB = SanitizeColor(Effect.ColorB, Defaults.ColorB);
		Out.Low = FMath::Clamp(Finite(Effect.Low, Defaults.Low), 0.f, 1.f);
		Out.High = FMath::Clamp(Finite(Effect.High, Defaults.High), 0.f, 1.f);
		Out.Size = FMath::Clamp(Finite(Effect.Size, Defaults.Size), 0.f, MaxSizeDegrees);
		Out.RateHz = FMath::Clamp(Finite(Effect.RateHz, Defaults.RateHz), 0.f, MaxRateHz);
		Out.Duty = FMath::Clamp(Finite(Effect.Duty, Defaults.Duty), 0.f, 1.f);
		Out.Phase = FMath::Clamp(Finite(Effect.Phase, 0.f), -MaxPhaseDegrees, MaxPhaseDegrees);
		Out.Spread = FMath::Clamp(Finite(Effect.Spread, 0.f), -MaxPhaseDegrees, MaxPhaseDegrees);
		Out.Wings = FMath::Clamp(Effect.Wings, 1, MaxWings);
		Out.Blocks = FMath::Clamp(Effect.Blocks, 1, MaxBlocks);
		Out.Audio.Mode = SanitizeEnum(Effect.Audio.Mode, EStageEffectAudioMode::Trigger, Defaults.Audio.Mode);
		Out.Audio.Band = SanitizeEnum(Effect.Audio.Band, EStageAudioBand::Level, Defaults.Audio.Band);
		Out.Audio.Amount = FMath::Clamp(Finite(Effect.Audio.Amount, Defaults.Audio.Amount), 0.f, 1.f);
		Out.Audio.Modulates = SanitizeEnum(Effect.Audio.Modulates, EStageEffectAudioTarget::Rate, Defaults.Audio.Modulates);
		Out.Audio.BeatsPerCycle = SanitizeEnum(Effect.Audio.BeatsPerCycle, EStageEffectBeatDivision::Eight, Defaults.Audio.BeatsPerCycle);
		Out.Audio.Response = SanitizeEnum(Effect.Audio.Response, EStageEffectAudioResponse::Slow, Defaults.Audio.Response);
		if (Out.Target == EStageEffectTarget::Beam && !IsBeamAttribute(Out.Attribute))
		{
			Out.Attribute = StageCraftTags::Attribute_Zoom;
		}
		if (Out.Target != EStageEffectTarget::Beam)
		{
			Out.Attribute = FGameplayTag();
		}
		return Out;
	}

	int32 SanitizeEntries(TArray<FStageEffectEntry>& Entries)
	{
		int32 Dropped = 0;
		TSet<FGuid> SeenIds;
		TArray<FStageEffectEntry> Kept;
		Kept.Reserve(FMath::Min(Entries.Num(), MaxEffects));
		for (FStageEffectEntry& Entry : Entries)
		{
			bool bIdSeen = false;
			if (!Entry.Id.IsValid() || Kept.Num() >= MaxEffects)
			{
				++Dropped;
				continue;
			}
			SeenIds.Add(Entry.Id, &bIdSeen);
			if (bIdSeen)
			{
				++Dropped;
				continue;
			}
			TArray<int32> FixtureIds;
			TSet<int32> SeenFixtures;
			for (const int32 FixtureId : Entry.FixtureIds)
			{
				bool bFixtureSeen = false;
				if (FixtureId > 0 && FixtureIds.Num() < MaxFixturesPerEffect)
				{
					SeenFixtures.Add(FixtureId, &bFixtureSeen);
				}
				if (FixtureId <= 0 || bFixtureSeen || FixtureIds.Num() >= MaxFixturesPerEffect)
				{
					++Dropped;
					continue;
				}
				FixtureIds.Add(FixtureId);
			}
			if (FixtureIds.IsEmpty())
			{
				++Dropped;
				continue;
			}
			Entry.FixtureIds = MoveTemp(FixtureIds);
			Entry.Effect = SanitizeEffect(Entry.Effect);
			Kept.Add(MoveTemp(Entry));
		}
		Entries = MoveTemp(Kept);
		return Dropped;
	}

	bool AreEntriesEqual(TConstArrayView<FStageEffectEntry> A, TConstArrayView<FStageEffectEntry> B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].Id != B[Index].Id || A[Index].FixtureIds != B[Index].FixtureIds || !A[Index].Effect.Equals(B[Index].Effect))
			{
				return false;
			}
		}
		return true;
	}

	int32 SanitizeStops(TArray<FGuid>& Stops, TConstArrayView<FStageEffectEntry> Effects)
	{
		int32 Dropped = 0;
		TSet<FGuid> Seen;
		TArray<FGuid> Kept;
		Kept.Reserve(FMath::Min(Stops.Num(), MaxEffects));
		for (const FGuid& Id : Stops)
		{
			bool bSeen = false;
			if (Id.IsValid() && Kept.Num() < MaxEffects)
			{
				Seen.Add(Id, &bSeen);
			}
			const bool bStarted = Effects.ContainsByPredicate([&Id](const FStageEffectEntry& Entry) { return Entry.Id == Id; });
			if (!Id.IsValid() || bSeen || bStarted || Kept.Num() >= MaxEffects)
			{
				++Dropped;
				continue;
			}
			Kept.Add(Id);
		}
		Stops = MoveTemp(Kept);
		return Dropped;
	}

	TArray<FStageEffect> MakeQuickEffects()
	{
		TArray<FStageEffect> Effects;
		const auto Add = [&Effects](const FText& Name, EStageEffectTarget Target, EStageEffectWaveform Waveform, float RateHz, float Spread) -> FStageEffect&
		{
			FStageEffect& Effect = Effects.AddDefaulted_GetRef();
			Effect.Name = Name;
			Effect.Target = Target;
			Effect.Waveform = Waveform;
			Effect.RateHz = RateHz;
			Effect.Spread = Spread;
			return Effect;
		};
		Add(LOCTEXT("Circle", "Circle"), EStageEffectTarget::Position, EStageEffectWaveform::Circle, 0.25f, 0.f).Size = 25.f;
		Add(LOCTEXT("Figure8", "Figure 8"), EStageEffectTarget::Position, EStageEffectWaveform::Figure8, 0.2f, 0.f).Size = 25.f;
		FStageEffect& PanSweep = Add(LOCTEXT("PanSweep", "Pan Sweep"), EStageEffectTarget::Position, EStageEffectWaveform::Sine, 0.2f, 180.f);
		PanSweep.Axis = EStageEffectAxis::Pan;
		PanSweep.Size = 45.f;
		FStageEffect& TiltWave = Add(LOCTEXT("TiltWave", "Tilt Wave"), EStageEffectTarget::Position, EStageEffectWaveform::Sine, 0.3f, 360.f);
		TiltWave.Axis = EStageEffectAxis::Tilt;
		TiltWave.Size = 20.f;
		Add(LOCTEXT("DimmerSine", "Dimmer Sine"), EStageEffectTarget::Intensity, EStageEffectWaveform::Sine, 0.5f, 360.f);
		Add(LOCTEXT("DimmerChase", "Dimmer Chase"), EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 1.f, 360.f).Duty = 0.25f;
		Add(LOCTEXT("Strobe", "Strobe"), EStageEffectTarget::Intensity, EStageEffectWaveform::Square, 10.f, 0.f).Duty = 0.2f;
		Add(LOCTEXT("Sparkle", "Random Sparkle"), EStageEffectTarget::Intensity, EStageEffectWaveform::Random, 4.f, 360.f);
		Add(LOCTEXT("Rainbow", "Rainbow"), EStageEffectTarget::Color, EStageEffectWaveform::RampUp, 0.1f, 360.f).ColorMode = EStageEffectColorMode::Rainbow;
		Add(LOCTEXT("ColorFade", "Colour Fade"), EStageEffectTarget::Color, EStageEffectWaveform::Sine, 0.25f, 180.f);
		FStageEffect& Iris = Add(LOCTEXT("IrisPulse", "Iris Pulse"), EStageEffectTarget::Beam, EStageEffectWaveform::Sine, 0.5f, 0.f);
		Iris.Attribute = StageCraftTags::Attribute_Iris;
		Iris.High = 0.8f;
		FStageEffect& Zoom = Add(LOCTEXT("ZoomBreathe", "Zoom Breathe"), EStageEffectTarget::Beam, EStageEffectWaveform::Triangle, 0.2f, 90.f);
		Zoom.Attribute = StageCraftTags::Attribute_Zoom;
		return Effects;
	}

	TArray<FStageEffect> MakeAudioQuickEffects()
	{
		TArray<FStageEffect> Effects;
		const auto Add = [&Effects](const FText& Name, EStageEffectTarget Target, EStageEffectWaveform Waveform, EStageEffectAudioMode Mode, float Spread) -> FStageEffect&
		{
			FStageEffect& Effect = Effects.AddDefaulted_GetRef();
			Effect.Name = Name;
			Effect.Target = Target;
			Effect.Waveform = Waveform;
			Effect.Spread = Spread;
			Effect.Audio.Mode = Mode;
			return Effect;
		};
		// The level follows the kick: the top of a still wave (a cosine at rate 0 sits at its top), scaled by the bass.
		FStageEffect& BassPulse = Add(LOCTEXT("BassPulse", "Bass Pulse"), EStageEffectTarget::Intensity, EStageEffectWaveform::Cosine, EStageEffectAudioMode::Modulate, 0.f);
		BassPulse.RateHz = 0.f;
		BassPulse.Audio.Band = EStageAudioBand::Bass;
		BassPulse.Audio.Response = EStageEffectAudioResponse::Fast;
		// One fixture lit at a time, moving on one fixture per beat.
		FStageEffect& BeatChase = Add(LOCTEXT("BeatChase", "Beat Chase"), EStageEffectTarget::Intensity, EStageEffectWaveform::Square, EStageEffectAudioMode::Trigger, 360.f);
		BeatChase.Duty = 0.25f;
		BeatChase.Audio.BeatsPerCycle = EStageEffectBeatDivision::One;
		// A new random level on every hit: onsets are at least 100 ms apart (the analyser's refractory gap).
		FStageEffect& Sparkle = Add(LOCTEXT("HighSparkle", "High Sparkle"), EStageEffectTarget::Intensity, EStageEffectWaveform::Random, EStageEffectAudioMode::Trigger, 360.f);
		Sparkle.Audio.Band = EStageAudioBand::Level;
		// Once round the colour wheel per bar of four beats.
		FStageEffect& Hue = Add(LOCTEXT("HuePerBar", "Hue per Bar"), EStageEffectTarget::Color, EStageEffectWaveform::RampUp, EStageEffectAudioMode::Sync, 0.f);
		Hue.ColorMode = EStageEffectColorMode::Rainbow;
		Hue.Audio.BeatsPerCycle = EStageEffectBeatDivision::Four;
		// Left on one beat, right on the next.
		FStageEffect& Swing = Add(LOCTEXT("BeatSwing", "Beat Swing"), EStageEffectTarget::Position, EStageEffectWaveform::Sine, EStageEffectAudioMode::Sync, 0.f);
		Swing.Axis = EStageEffectAxis::Pan;
		Swing.Size = 30.f;
		Swing.Audio.BeatsPerCycle = EStageEffectBeatDivision::Two;
		// The zoom opens with the mids, held at its widest when the music stops.
		FStageEffect& Zoom = Add(LOCTEXT("MidZoom", "Mid Zoom"), EStageEffectTarget::Beam, EStageEffectWaveform::Cosine, EStageEffectAudioMode::Modulate, 0.f);
		Zoom.Attribute = StageCraftTags::Attribute_Zoom;
		Zoom.RateHz = 0.f;
		Zoom.Low = 0.2f;
		Zoom.Audio.Band = EStageAudioBand::Mid;
		Zoom.Audio.Response = EStageEffectAudioResponse::Slow;
		return Effects;
	}

	TArray<FStageEffect> MakeMusicLook()
	{
		TArray<FStageEffect> Effects;
		// Bass: the same pulse as the quick effect, so the look and the button behave alike.
		const TArray<FStageEffect> Quick = MakeAudioQuickEffects();
		Effects.Add(Quick[0]);
		check(Effects[0].Audio.Band == EStageAudioBand::Bass);

		// Mids: a slow walk round the colour wheel, spread over the fixtures, that speeds up to twice its rate when the mids are loud.
		FStageEffect& Colour = Effects.AddDefaulted_GetRef();
		Colour.Name = LOCTEXT("MidColour", "Mid Colour");
		Colour.Target = EStageEffectTarget::Color;
		Colour.Waveform = EStageEffectWaveform::RampUp;
		Colour.ColorMode = EStageEffectColorMode::Rainbow;
		Colour.RateHz = 0.08f;
		Colour.Spread = 360.f;
		Colour.Audio.Mode = EStageEffectAudioMode::Modulate;
		Colour.Audio.Band = EStageAudioBand::Mid;
		Colour.Audio.Modulates = EStageEffectAudioTarget::Rate;
		Colour.Audio.Amount = 1.f;
		Colour.Audio.Response = EStageEffectAudioResponse::Slow;

		// Highs: the shutter strobes faster with the cymbals and hats. A still wave at its top (rate 0), so the level is the band alone;
		// open in silence (StageEffects::GetSilentBand) and never faster than the flash limit.
		FStageEffect& Strobe = Effects.AddDefaulted_GetRef();
		Strobe.Name = LOCTEXT("HighStrobe", "High Strobe");
		Strobe.Target = EStageEffectTarget::Beam;
		Strobe.Attribute = StageCraftTags::Attribute_Shutter;
		Strobe.Waveform = EStageEffectWaveform::Cosine;
		Strobe.RateHz = 0.f;
		Strobe.Spread = 0.f;
		Strobe.Low = 0.f;
		Strobe.High = 0.6f;
		Strobe.Audio.Mode = EStageEffectAudioMode::Modulate;
		Strobe.Audio.Band = EStageAudioBand::High;
		Strobe.Audio.Modulates = EStageEffectAudioTarget::Level;
		Strobe.Audio.Amount = 1.f;
		Strobe.Audio.Response = EStageEffectAudioResponse::Fast;
		return Effects;
	}

	float GetBeatsPerCycle(EStageEffectBeatDivision Division)
	{
		switch (Division)
		{
		case EStageEffectBeatDivision::Quarter: return 0.25f;
		case EStageEffectBeatDivision::Half:    return 0.5f;
		case EStageEffectBeatDivision::One:     return 1.f;
		case EStageEffectBeatDivision::Two:     return 2.f;
		case EStageEffectBeatDivision::Four:    return 4.f;
		case EStageEffectBeatDivision::Eight:   return 8.f;
		}
		return 1.f;
	}

	bool IsClockFromAudio(const FStageEffectAudio& Audio)
	{
		return Audio.Mode == EStageEffectAudioMode::Sync || Audio.Mode == EStageEffectAudioMode::Trigger;
	}
}

#undef LOCTEXT_NAMESPACE
