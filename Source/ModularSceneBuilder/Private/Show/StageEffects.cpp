// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageEffects.h"

#include "Show/StageShowMix.h"

namespace StageEffects
{
	namespace
	{
		/** A well-mixed 32-bit hash (the lowbias32 finaliser), so neighbouring seeds and cycles give unrelated levels. */
		uint32 Mix(uint32 Value)
		{
			Value ^= Value >> 16;
			Value *= 0x7feb352dU;
			Value ^= Value >> 15;
			Value *= 0x846ca68bU;
			Value ^= Value >> 16;
			return Value;
		}

		float ToUnit(uint32 Hash)
		{
			return static_cast<float>(Hash & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
		}

		FSample FromUnit(float Unit)
		{
			FSample Sample;
			Sample.Unit = Unit;
			Sample.Pan = Sample.Tilt = 2.f * Unit - 1.f;
			return Sample;
		}
	}

	FSample SampleWave(EStageEffectWaveform Waveform, double Position, float Duty, uint32 Seed)
	{
		const double Whole = FMath::FloorToDouble(Position);
		const float Fraction = static_cast<float>(Position - Whole);
		const float Angle = UE_TWO_PI * Fraction;
		switch (Waveform)
		{
		case EStageEffectWaveform::Sine:
			return FromUnit(0.5f + 0.5f * FMath::Sin(Angle));
		case EStageEffectWaveform::Cosine:
			return FromUnit(0.5f + 0.5f * FMath::Cos(Angle));
		case EStageEffectWaveform::Square:
			return FromUnit(Fraction < Duty ? 1.f : 0.f);
		case EStageEffectWaveform::RampUp:
			return FromUnit(Fraction);
		case EStageEffectWaveform::RampDown:
			return FromUnit(1.f - Fraction);
		case EStageEffectWaveform::Triangle:
			return FromUnit(1.f - FMath::Abs(2.f * Fraction - 1.f));
		case EStageEffectWaveform::Random:
		{
			// One level per whole cycle; the cycle is folded to 32 bits, which repeats only after 4 billion cycles.
			const uint32 Cycle = static_cast<uint32>(static_cast<int64>(Whole));
			const uint32 Hash = Mix(Mix(Seed * 0x9E3779B1u + 0x632BE5ABu) ^ Cycle);
			FSample Sample;
			Sample.Unit = ToUnit(Hash);
			Sample.Pan = 2.f * Sample.Unit - 1.f;
			Sample.Tilt = 2.f * ToUnit(Mix(Hash ^ 0xA5A5A5A5u)) - 1.f;
			return Sample;
		}
		case EStageEffectWaveform::Circle:
		{
			FSample Sample = FromUnit(0.5f + 0.5f * FMath::Sin(Angle));
			Sample.Tilt = FMath::Cos(Angle);
			return Sample;
		}
		case EStageEffectWaveform::Figure8:
		{
			FSample Sample = FromUnit(0.5f + 0.5f * FMath::Sin(Angle));
			Sample.Tilt = FMath::Sin(2.f * Angle);
			return Sample;
		}
		default:
			return FSample();
		}
	}

	void GetPatternStep(const FStageEffect& Effect, int32 Index, int32 Count, int32& OutStep, int32& OutSteps)
	{
		const int32 Fixtures = FMath::Max(Count, 1);
		const int32 Clamped = FMath::Clamp(Index, 0, Fixtures - 1);
		const int32 Wings = FMath::Clamp(Effect.Wings, 1, Fixtures);
		const int32 WingSize = FMath::DivideAndRoundUp(Fixtures, Wings);
		int32 Position = Clamped % WingSize;
		if ((Clamped / WingSize) % 2 == 1)
		{
			Position = WingSize - 1 - Position; // Every other wing mirrored, so the pattern runs out from (or in to) the middle.
		}
		const int32 Blocks = FMath::Clamp(Effect.Blocks, 1, WingSize);
		OutStep = Position / Blocks;
		OutSteps = FMath::DivideAndRoundUp(WingSize, Blocks);
	}

	double GetFixtureOffset(const FStageEffect& Effect, int32 Index, int32 Count)
	{
		int32 Step = 0;
		int32 Steps = 1;
		GetPatternStep(Effect, Index, Count, Step, Steps);
		return (static_cast<double>(Effect.Phase) + static_cast<double>(Effect.Spread) * Step / Steps) / 360.0;
	}

	double GetWavePosition(const FStageEffect& Effect, int32 Index, int32 Count, double Cycles)
	{
		const double Offset = GetFixtureOffset(Effect, Index, Count);
		switch (Effect.Direction)
		{
		case EStageEffectDirection::Reverse:
			// The last fixture leads: the pattern travels back along the fixtures.
			return Cycles + Offset;
		case EStageEffectDirection::Bounce:
		{
			// The clock runs 0 -> 1 -> 0, so the pattern travels out and back without a jump.
			const double Folded = FMath::Fmod(Cycles, 2.0);
			const double Wrapped = Folded < 0.0 ? Folded + 2.0 : Folded;
			return (Wrapped <= 1.0 ? Wrapped : 2.0 - Wrapped) - Offset;
		}
		default:
			return Cycles - Offset;
		}
	}

	FSample Evaluate(const FStageEffect& Effect, int32 Index, int32 Count, double Cycles)
	{
		int32 Step = 0;
		int32 Steps = 1;
		GetPatternStep(Effect, Index, Count, Step, Steps);
		return SampleWave(Effect.Waveform, GetWavePosition(Effect, Index, Count, Cycles), Effect.Duty, static_cast<uint32>(Step));
	}

	float GetSizeFactor(const FStageEffect& Effect, int32 Index, int32 Count)
	{
		return StageShowMix::FanFactor(Effect.SizeFan, Index, Count);
	}

	FLinearColor GetColor(const FStageEffect& Effect, float Unit)
	{
		if (Effect.ColorMode == EStageEffectColorMode::Rainbow)
		{
			return FLinearColor(360.f * FMath::Clamp(Unit, 0.f, 1.f), 1.f, 1.f).HSVToLinearRGB();
		}
		return FMath::Lerp(Effect.ColorA, Effect.ColorB, FMath::Clamp(Unit, 0.f, 1.f));
	}

	float GetLevel(const FStageEffect& Effect, float Unit, float SizeFactor)
	{
		return Effect.Low + (Effect.High - Effect.Low) * FMath::Clamp(Unit, 0.f, 1.f) * FMath::Abs(SizeFactor);
	}

	// --- Audio ---

	float FAudioInput::GetBand(EStageAudioBand Band) const
	{
		const int32 Index = static_cast<int32>(Band);
		return Index >= 0 && Index < UE_ARRAY_COUNT(Bands) ? Bands[Index] : 0.f;
	}

	FAudioInput FAudioInput::FromFeatures(const FStageAudioFeatures& Features, double NowSeconds)
	{
		FAudioInput Input;
		for (int32 Band = 0; Band < UE_ARRAY_COUNT(Input.Bands); ++Band)
		{
			Input.Bands[Band] = FMath::Clamp(Features.GetBand(static_cast<EStageAudioBand>(Band)), 0.f, 1.f);
		}
		Input.BPM = Features.BPM;
		Input.Confidence = Features.Confidence;
		Input.OnsetCount = Features.OnsetCount;
		Input.bSignal = Features.bSignal;
		// The count and phase describe the frame's time; carried forward the way PhaseAt carries the phase (at most a second), with the
		// count following the phase past the beat.
		double Beats = static_cast<double>(Features.BeatCount) + Features.BeatPhase;
		if (Features.BPM > 0.f)
		{
			Beats += FMath::Clamp(NowSeconds - Features.Timestamp, 0.0, 1.0) * Features.BPM / 60.0;
		}
		Input.Beats = Beats;
		return Input;
	}

	float GetModulationBand(const FStageEffectAudio& Audio, const FAudioInput& Input)
	{
		return Input.bSignal ? FMath::Clamp(Input.GetBand(Audio.Band), 0.f, 1.f) : 1.f;
	}

	float SmoothBand(float Previous, float Target, float DeltaSeconds, EStageEffectAudioResponse Response)
	{
		if (Target >= Previous)
		{
			return Target;
		}
		if (DeltaSeconds <= 0.f)
		{
			return Previous;
		}
		float TimeConstant = 0.12f;
		switch (Response)
		{
		case EStageEffectAudioResponse::Fast:   TimeConstant = 0.03f; break;
		case EStageEffectAudioResponse::Medium: TimeConstant = 0.12f; break;
		case EStageEffectAudioResponse::Slow:   TimeConstant = 0.4f; break;
		}
		const float Keep = FMath::Exp(-DeltaSeconds / TimeConstant);
		return Target + (Previous - Target) * Keep;
	}

	float GetModulationScale(const FStageEffectAudio& Audio, float Band)
	{
		return 1.f - Audio.Amount + Audio.Amount * FMath::Clamp(Band, 0.f, 1.f);
	}

	void ApplyModulation(FStageEffect& InOutEffect, float Band)
	{
		if (InOutEffect.Audio.Mode != EStageEffectAudioMode::Modulate)
		{
			return;
		}
		const float Scale = GetModulationScale(InOutEffect.Audio, Band);
		switch (InOutEffect.Audio.Modulates)
		{
		case EStageEffectAudioTarget::Level:
			InOutEffect.High = InOutEffect.Low + (InOutEffect.High - InOutEffect.Low) * Scale;
			break;
		case EStageEffectAudioTarget::Size:
			InOutEffect.Size *= Scale;
			break;
		case EStageEffectAudioTarget::Rate:
			break;
		}
	}

	float GetModulatedRate(const FStageEffect& Effect, float RateHz, float Band)
	{
		if (Effect.Audio.Mode != EStageEffectAudioMode::Modulate || Effect.Audio.Modulates != EStageEffectAudioTarget::Rate)
		{
			return RateHz;
		}
		return RateHz * (1.f + Effect.Audio.Amount * FMath::Clamp(Band, 0.f, 1.f));
	}

	double GetSyncTarget(const FStageEffectAudio& Audio, double Beats)
	{
		return Beats / StageEffectRules::GetBeatsPerCycle(Audio.BeatsPerCycle);
	}

	int64 GetTriggerEvents(const FStageEffectAudio& Audio, const FAudioInput& Input)
	{
		if (Audio.Band == EStageAudioBand::Level)
		{
			return Input.OnsetCount;
		}
		// Every BeatsPerCycle beats: a quarter gives four steps a beat, eight one every two bars.
		return static_cast<int64>(FMath::FloorToDouble(Input.Beats / StageEffectRules::GetBeatsPerCycle(Audio.BeatsPerCycle)));
	}

	double GetTriggerStep(const FStageEffect& Effect, int32 Count)
	{
		if (Effect.Waveform == EStageEffectWaveform::Random)
		{
			return 1.0;
		}
		int32 Step = 0;
		int32 Steps = 1;
		GetPatternStep(Effect, 0, Count, Step, Steps);
		return 1.0 / FMath::Max(Steps, 1);
	}

	double GetCycleError(double Cycles, double Target)
	{
		const double Difference = Target - Cycles;
		return Difference - FMath::FloorToDouble(Difference + 0.5);
	}
}
