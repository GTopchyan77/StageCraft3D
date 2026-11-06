// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioReactiveTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudioReactiveTypes)

float FStageAudioFeatures::GetBand(EStageAudioBand Band) const
{
	switch (Band)
	{
	case EStageAudioBand::SubBass: return SubBass;
	case EStageAudioBand::Bass: return Bass;
	case EStageAudioBand::Mid: return Mid;
	case EStageAudioBand::High: return High;
	case EStageAudioBand::Level: return Level;
	}
	return 0.f;
}

float FStageAudioFeatures::PhaseAt(double NowSeconds) const
{
	if (BPM <= 0.f)
	{
		return BeatPhase;
	}
	// Carried at most one second: a reader that stalled longer gets the last phase moved on, not an extrapolation over minutes.
	const double Elapsed = FMath::Clamp(NowSeconds - Timestamp, 0.0, 1.0);
	const double Phase = double(BeatPhase) + Elapsed * double(BPM) / 60.0;
	return float(Phase - FMath::FloorToDouble(Phase));
}

namespace StageAudioReactive
{
	namespace
	{
		float SanitizeFloat(float Value, float Default, float Min, float Max)
		{
			return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Default;
		}
	}

	FStageAudioReactiveSettings Sanitize(const FStageAudioReactiveSettings& Settings)
	{
		const FStageAudioReactiveSettings Defaults;
		FStageAudioReactiveSettings Out = Settings;

		const uint8 RawSource = uint8(Settings.LastSource);
		Out.LastSource = RawSource <= uint8(EStageAudioSource::TestSignal) ? Settings.LastSource : EStageAudioSource::Off;
		if (Out.LastSource == EStageAudioSource::File)
		{
			Out.LastSource = EStageAudioSource::Off;
		}
		Out.InputDeviceId = Settings.InputDeviceId.Left(512);
		Out.InputGainDb = SanitizeFloat(Settings.InputGainDb, Defaults.InputGainDb, MinInputGainDb, MaxInputGainDb);
		Out.FileOutputDelayMs = SanitizeFloat(Settings.FileOutputDelayMs, Defaults.FileOutputDelayMs, 0.f, MaxFileOutputDelayMs);
		Out.FileLatencyOffsetMs = SanitizeFloat(Settings.FileLatencyOffsetMs, 0.f, MinLatencyOffsetMs, MaxLatencyOffsetMs);
		Out.InputLatencyOffsetMs = SanitizeFloat(Settings.InputLatencyOffsetMs, 0.f, MinLatencyOffsetMs, MaxLatencyOffsetMs);
		Out.SystemAudioLatencyOffsetMs = SanitizeFloat(Settings.SystemAudioLatencyOffsetMs, 0.f, MinLatencyOffsetMs, MaxLatencyOffsetMs);
		Out.TestSignalBPM = SanitizeFloat(Settings.TestSignalBPM, Defaults.TestSignalBPM, MinTestSignalBPM, MaxTestSignalBPM);
		Out.FlashLimitHz = SanitizeFloat(Settings.FlashLimitHz, Defaults.FlashLimitHz, MinFlashLimitHz, MaxFlashLimitHz);

		// Ascending edges, each at least 10 Hz above the previous; a set that cannot be made ascending falls back to the defaults.
		float Edges[4] = {
			SanitizeFloat(Settings.SubBassTopHz, Defaults.SubBassTopHz, MinBandHz + 10.f, MaxBandHz),
			SanitizeFloat(Settings.BassTopHz, Defaults.BassTopHz, MinBandHz + 10.f, MaxBandHz),
			SanitizeFloat(Settings.MidTopHz, Defaults.MidTopHz, MinBandHz + 10.f, MaxBandHz),
			SanitizeFloat(Settings.HighTopHz, Defaults.HighTopHz, MinBandHz + 10.f, MaxBandHz) };
		bool bAscending = true;
		for (int32 Index = 1; Index < 4; ++Index)
		{
			bAscending &= Edges[Index] >= Edges[Index - 1] + 10.f;
		}
		if (!bAscending)
		{
			Edges[0] = Defaults.SubBassTopHz;
			Edges[1] = Defaults.BassTopHz;
			Edges[2] = Defaults.MidTopHz;
			Edges[3] = Defaults.HighTopHz;
		}
		Out.SubBassTopHz = Edges[0];
		Out.BassTopHz = Edges[1];
		Out.MidTopHz = Edges[2];
		Out.HighTopHz = Edges[3];
		return Out;
	}

	double GetHeardDelaySeconds(const FStageAudioReactiveSettings& InSettings, EStageAudioSource Source)
	{
		const FStageAudioReactiveSettings Settings = Sanitize(InSettings);
		switch (Source)
		{
		// The tap sits where the mixer renders the file; the audience hears it after the engine's output buffering.
		case EStageAudioSource::File: return (double(Settings.FileOutputDelayMs) + double(Settings.FileLatencyOffsetMs)) / 1000.0;
		// Captured audio has already been heard: its analysis is late by the detection time, and beat phase is carried forward.
		case EStageAudioSource::Input: return double(Settings.InputLatencyOffsetMs) / 1000.0;
		case EStageAudioSource::SystemAudio: return double(Settings.SystemAudioLatencyOffsetMs) / 1000.0;
		case EStageAudioSource::TestSignal:
		case EStageAudioSource::Off:
			break;
		}
		return 0.0;
	}
}
