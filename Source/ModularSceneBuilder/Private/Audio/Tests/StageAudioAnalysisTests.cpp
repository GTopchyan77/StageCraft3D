// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the pure audio analysis (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §3, §9): bands, onsets, tempo and
// beat phase, the resampler and the settings. They feed generated signals straight into StageAudioAnalysis: no device, no thread.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Audio; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Audio/StageAudioAnalysis.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "Algo/MinElement.h"
#include "Misc/AutomationTest.h"

namespace StageAudioAnalysisTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** The extractor's frames over a whole generated signal, fed in 10 ms blocks as a capture device would. */
	struct FRun
	{
		TArray<FStageAudioFeatures> Frames;
		TArray<StageAudioAnalysis::FOnset> Onsets;
	};

	using FGenerator = TFunctionRef<void(int64 StartSample, TArrayView<float> Out)>;

	bool Run(double Seconds, FGenerator Generate, FRun& Out, const FStageAudioReactiveSettings& Settings = FStageAudioReactiveSettings())
	{
		StageAudioAnalysis::FFeatureExtractor Extractor;
		if (!Extractor.Init(Settings))
		{
			return false;
		}
		Extractor.SetOnsetSink(&Out.Onsets);
		constexpr int32 Block = 480;
		TArray<float> Samples;
		Samples.SetNumUninitialized(Block);
		const int64 Total = int64(Seconds * StageAudioAnalysis::SampleRate);
		for (int64 Start = 0; Start < Total; Start += Block)
		{
			Generate(Start, Samples);
			Extractor.Push(Samples, double(Start) / StageAudioAnalysis::SampleRate, [&Out](const FStageAudioFeatures& Features) { Out.Frames.Add(Features); });
		}
		return true;
	}

	void Tone(float Hz, float Amplitude, int64 StartSample, TArrayView<float> Out)
	{
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			Out[Index] = Amplitude * FMath::Sin(2.f * PI * Hz * float(double(StartSample + Index) / StageAudioAnalysis::SampleRate));
		}
	}

	/** The frame at or just after Seconds. */
	const FStageAudioFeatures* FrameAt(const FRun& Run, double Seconds)
	{
		for (const FStageAudioFeatures& Frame : Run.Frames)
		{
			if (Frame.Timestamp >= Seconds)
			{
				return &Frame;
			}
		}
		return nullptr;
	}

	/** Times at which the beat counter stepped, placed where the phase crossed zero. */
	TArray<double> BeatTimes(const FRun& Run)
	{
		TArray<double> Times;
		for (int32 Index = 1; Index < Run.Frames.Num(); ++Index)
		{
			const FStageAudioFeatures& Frame = Run.Frames[Index];
			if (Frame.BeatCount > Run.Frames[Index - 1].BeatCount && Frame.BPM > 0.f)
			{
				Times.Add(Frame.Timestamp - double(Frame.BeatPhase) * 60.0 / Frame.BPM);
			}
		}
		return Times;
	}

	double NearestDistance(double Time, const TArray<double>& Targets)
	{
		double Best = TNumericLimits<double>::Max();
		for (const double Target : Targets)
		{
			Best = FMath::Min(Best, FMath::Abs(Time - Target));
		}
		return Best;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioBandsTest, "StageCraft.Audio.Bands", StageAudioAnalysisTests::Flags)

bool FStageAudioBandsTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioAnalysisTests;

	// Each tone lands in its own band, well above the others (raw energy, before the per-band normalisation).
	const struct { float Hz; int32 Band; } Tones[] = { { 40.f, 0 }, { 120.f, 1 }, { 1000.f, 2 }, { 8000.f, 3 } };
	for (const auto& Case : Tones)
	{
		StageAudioAnalysis::FFeatureExtractor Extractor;
		TestTrue(TEXT("FFT available"), Extractor.Init(FStageAudioReactiveSettings()));
		StageAudioAnalysis::FBandAnalyzer Bands;
		Bands.Init(StageAudioAnalysis::FBandEdges());
		// Analyse one frame of the tone through the same FFT path the extractor uses: run 0.5 s and read the last frame's dB.
		TArray<float> Power;
		Power.SetNumZeroed(StageAudioAnalysis::NumBins);
		TArray<float> Samples;
		Samples.SetNumUninitialized(StageAudioAnalysis::FFTSize);
		Tone(Case.Hz, 0.5f, 0, Samples);
		// A plain DFT power spectrum under a Hann window, computed here so the test checks the band maths and not the FFT.
		float WindowPower = 0.f;
		TArray<float> Windowed;
		Windowed.SetNumUninitialized(Samples.Num());
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			const float W = 0.5f - 0.5f * FMath::Cos(2.f * PI * float(Index) / float(Samples.Num() - 1));
			Windowed[Index] = Samples[Index] * W;
			WindowPower += W * W;
		}
		const int32 N = Samples.Num();
		for (int32 Bin = 0; Bin < StageAudioAnalysis::NumBins; ++Bin)
		{
			double Re = 0.0, Im = 0.0;
			for (int32 Index = 0; Index < N; ++Index)
			{
				const double Angle = -2.0 * PI * double(Bin) * double(Index) / double(N);
				Re += Windowed[Index] * FMath::Cos(Angle);
				Im += Windowed[Index] * FMath::Sin(Angle);
			}
			Power[Bin] = float((Re * Re + Im * Im) / double(N)); // unitary
		}
		StageAudioAnalysis::FBandFrame Frame;
		Bands.Process(Power, WindowPower, Frame);
		int32 Loudest = 0;
		for (int32 Band = 1; Band < 4; ++Band)
		{
			Loudest = Frame.BandDb[Band] > Frame.BandDb[Loudest] ? Band : Loudest;
		}
		TestEqual(FString::Printf(TEXT("%.0f Hz lands in band %d"), Case.Hz, Case.Band), Loudest, Case.Band);
		for (int32 Band = 0; Band < 4; ++Band)
		{
			if (Band != Case.Band)
			{
				TestTrue(FString::Printf(TEXT("%.0f Hz: band %d at least 6 dB below (%.1f vs %.1f dB)"), Case.Hz, Band, Frame.BandDb[Band], Frame.BandDb[Case.Band]),
					Frame.BandDb[Band] <= Frame.BandDb[Case.Band] - 6.f);
			}
		}
		// A 0.5 sine has a mean square of 0.125, -9 dB.
		TestTrue(FString::Printf(TEXT("%.0f Hz level is -9 dB (%.2f)"), Case.Hz, Frame.LevelDb), FMath::IsNearlyEqual(Frame.LevelDb, -9.03f, 0.5f));
	}

	// The same pipeline through the real FFT: a full-scale sine reads -3 dBFS whatever the FFT implementation scales by.
	{
		FRun Loud;
		TestTrue(TEXT("Run sine"), Run(0.5, [](int64 Start, TArrayView<float> Out) { Tone(1000.f, 1.f, Start, Out); }, Loud));
		// The extractor's Level is normalised; check the onset-free steady tone reaches the top of the range.
		TestTrue(TEXT("Steady tone: level at the top of its range"), Loud.Frames.Num() > 0 && Loud.Frames.Last().Level > 0.95f);
		TestTrue(TEXT("Steady tone: signal present"), Loud.Frames.Num() > 0 && Loud.Frames.Last().bSignal);
	}

	// A quiet (-40 dB) and a loud (-6 dB) pulsing tone both use the whole range: near 1 on the pulse, near 0 between pulses.
	for (const float Amplitude : { 0.01f, 0.5f })
	{
		FRun Pulses;
		Run(6.0, [Amplitude](int64 Start, TArrayView<float> Out)
			{
				Tone(120.f, Amplitude, Start, Out);
				for (int32 Index = 0; Index < Out.Num(); ++Index)
				{
					const double Time = double(Start + Index) / StageAudioAnalysis::SampleRate;
					Out[Index] *= FMath::Frac(Time) < 0.5 ? 1.f : 0.f; // 0.5 s on, 0.5 s off
				}
			}, Pulses);
		float Top = 0.f;
		float Bottom = 1.f;
		for (const FStageAudioFeatures& Frame : Pulses.Frames)
		{
			if (Frame.Timestamp > 3.0)
			{
				const double Into = FMath::Frac(Frame.Timestamp);
				if (Into > 0.3 && Into < 0.45) { Top = FMath::Max(Top, Frame.Bass); }
				if (Into > 0.9) { Bottom = FMath::Min(Bottom, Frame.Bass); }
			}
		}
		TestTrue(FString::Printf(TEXT("Amplitude %.2f: bass reaches the top (%.2f)"), Amplitude, Top), Top > 0.9f);
		TestTrue(FString::Printf(TEXT("Amplitude %.2f: bass falls between pulses (%.2f)"), Amplitude, Bottom), Bottom < 0.2f);
	}

	// Release smoothing: after a step down the value falls over about 150 ms, not at once.
	{
		FRun Step;
		Run(2.0, [](int64 Start, TArrayView<float> Out)
			{
				Tone(1000.f, 0.5f, Start, Out);
				for (int32 Index = 0; Index < Out.Num(); ++Index)
				{
					Out[Index] *= (Start + Index) < StageAudioAnalysis::SampleRate ? 1.f : 0.f;
				}
			}, Step);
		const FStageAudioFeatures* Just = FrameAt(Step, 1.0 + 0.043 + 0.03); // the window has left the tone; 30 ms of release
		const FStageAudioFeatures* Later = FrameAt(Step, 1.0 + 0.043 + 0.6);
		TestTrue(TEXT("Release: still up 30 ms after the step"), Just && Just->Mid > 0.5f);
		TestTrue(TEXT("Release: down after 600 ms"), Later && Later->Mid < 0.05f);
		// Silence clears the signal after 2 s, not before.
		FRun Silence;
		Run(4.0, [](int64 Start, TArrayView<float> Out)
			{
				Tone(1000.f, 0.5f, Start, Out);
				for (int32 Index = 0; Index < Out.Num(); ++Index)
				{
					Out[Index] *= (Start + Index) < StageAudioAnalysis::SampleRate ? 1.f : 0.f;
				}
			}, Silence);
		const FStageAudioFeatures* Before = FrameAt(Silence, 2.8);
		const FStageAudioFeatures* After = FrameAt(Silence, 3.3);
		TestTrue(TEXT("Signal held for 2 s of silence"), Before && Before->bSignal);
		TestTrue(TEXT("Signal lost after 2 s of silence"), After && !After->bSignal);
	}

	// Settings: hostile edges and numbers are sanitised.
	{
		FStageAudioReactiveSettings Hostile;
		Hostile.SubBassTopHz = 5000.f;
		Hostile.BassTopHz = 100.f;
		Hostile.InputGainDb = NAN;
		Hostile.FileOutputDelayMs = -50.f;
		Hostile.SystemAudioLatencyOffsetMs = 1e9f;
		Hostile.TestSignalBPM = 1000.f;
		Hostile.LastSource = EStageAudioSource::File;
		const FStageAudioReactiveSettings Clean = StageAudioReactive::Sanitize(Hostile);
		const FStageAudioReactiveSettings Defaults;
		TestEqual(TEXT("Edges out of order fall back to defaults"), Clean.SubBassTopHz, Defaults.SubBassTopHz);
		TestEqual(TEXT("Bass edge default"), Clean.BassTopHz, Defaults.BassTopHz);
		TestEqual(TEXT("NaN gain falls back"), Clean.InputGainDb, Defaults.InputGainDb);
		TestEqual(TEXT("Negative output delay clamps to 0"), Clean.FileOutputDelayMs, 0.f);
		TestEqual(TEXT("Offset clamps"), Clean.SystemAudioLatencyOffsetMs, StageAudioReactive::MaxLatencyOffsetMs);
		TestEqual(TEXT("BPM clamps"), Clean.TestSignalBPM, StageAudioReactive::MaxTestSignalBPM);
		TestEqual(TEXT("File is never remembered"), Clean.LastSource, EStageAudioSource::Off);
		FStageAudioReactiveSettings BadEnum;
		BadEnum.LastSource = EStageAudioSource(200);
		TestEqual(TEXT("Unknown source falls back to Off"), StageAudioReactive::Sanitize(BadEnum).LastSource, EStageAudioSource::Off);
		TestTrue(TEXT("File heard 78 ms after the tap by default"), FMath::IsNearlyEqual(StageAudioReactive::GetHeardDelaySeconds(Defaults, EStageAudioSource::File), 0.078, 1e-6));
		TestTrue(TEXT("System audio already heard"), FMath::IsNearlyEqual(StageAudioReactive::GetHeardDelaySeconds(Defaults, EStageAudioSource::SystemAudio), 0.0, 1e-9));
	}

	// Features: phase carried forward at the tempo, at most a second.
	{
		FStageAudioFeatures Features;
		Features.BPM = 120.f;
		Features.BeatPhase = 0.25f;
		Features.Timestamp = 10.0;
		TestTrue(TEXT("PhaseAt quarter beat later"), FMath::IsNearlyEqual(Features.PhaseAt(10.125), 0.5f, 1e-4f));
		TestTrue(TEXT("PhaseAt wraps"), FMath::IsNearlyEqual(Features.PhaseAt(10.5), 0.25f, 1e-4f));
		TestTrue(TEXT("PhaseAt before the frame holds"), FMath::IsNearlyEqual(Features.PhaseAt(9.0), 0.25f, 1e-4f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioOnsetsTest, "StageCraft.Audio.Onsets", StageAudioAnalysisTests::Flags)

bool FStageAudioOnsetsTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioAnalysisTests;

	// Kicks only (the hats would be onsets too, which is correct but not what is counted here).
	StageAudioTestSignal::FParams Params;
	Params.BPM = 120.f;
	Params.FirstBeatSeconds = 1.0;
	Params.bHiHat = false;
	FRun Kicks;
	TestTrue(TEXT("Run"), Run(20.0, [&Params](int64 Start, TArrayView<float> Out) { StageAudioTestSignal::Generate(Params, Start, Out); }, Kicks));
	const TArray<double> Truth = StageAudioTestSignal::KickTimes(Params, 19.9);

	int32 Found = 0;
	double Bias = 0.0;
	for (const double Kick : Truth)
	{
		const StageAudioAnalysis::FOnset* Match = Kicks.Onsets.FindByPredicate([Kick](const StageAudioAnalysis::FOnset& Onset) { return FMath::Abs(Onset.Time - Kick) < 0.05; });
		if (Match)
		{
			++Found;
			Bias += Match->Time - Kick;
		}
	}
	const int32 FalsePositives = Kicks.Onsets.Num() - Found;
	Bias = Found > 0 ? Bias / Found : 0.0;
	AddInfo(FString::Printf(TEXT("Kicks %d/%d, false positives %d, onset time bias %.1f ms"), Found, Truth.Num(), FalsePositives, Bias * 1000.0));
	TestEqual(TEXT("Every kick is an onset"), Found, Truth.Num());
	TestTrue(TEXT("At most 2 false positives"), FalsePositives <= 2);
	TestTrue(TEXT("Onset times within 15 ms of the kicks"), FMath::Abs(Bias) < 0.015);

	// The decision comes one hop after the peak: the counter steps at most ~40 ms after the kick (G0: 27 ms mean).
	double Latency = 0.0;
	int32 Steps = 0;
	for (int32 Index = 1; Index < Kicks.Frames.Num(); ++Index)
	{
		if (Kicks.Frames[Index].OnsetCount > Kicks.Frames[Index - 1].OnsetCount)
		{
			const double Kick = Truth.IsEmpty() ? 0.0 : *Algo::MinElementBy(Truth, [&](double T) { return FMath::Abs(T - Kicks.Frames[Index].Timestamp); });
			if (Kicks.Frames[Index].Timestamp >= Kick)
			{
				Latency += Kicks.Frames[Index].Timestamp - Kick;
				++Steps;
			}
		}
	}
	Latency = Steps > 0 ? Latency / Steps : 1.0;
	AddInfo(FString::Printf(TEXT("Mean decision latency %.1f ms"), Latency * 1000.0));
	TestTrue(TEXT("Mean decision latency under 40 ms"), Latency < 0.040);

	// Silence and a steady tone make no onsets.
	FRun Steady;
	Run(5.0, [](int64 Start, TArrayView<float> Out) { Tone(440.f, 0.3f, Start, Out); }, Steady);
	TestTrue(FString::Printf(TEXT("A steady tone has at most one onset (its start): %d"), Steady.Onsets.Num()), Steady.Onsets.Num() <= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioTempoTest, "StageCraft.Audio.Tempo", StageAudioAnalysisTests::Flags)

bool FStageAudioTempoTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioAnalysisTests;

	for (const float BPM : { 90.f, 120.f, 128.f, 140.f, 174.f })
	{
		for (const bool bHat : { false, true })
		{
			StageAudioTestSignal::FParams Params;
			Params.BPM = BPM;
			Params.FirstBeatSeconds = 0.37;
			Params.bHiHat = bHat;
			FRun Result;
			Run(12.0, [&Params](int64 Start, TArrayView<float> Out) { StageAudioTestSignal::Generate(Params, Start, Out); }, Result);
			const FString Name = FString::Printf(TEXT("%.0f BPM%s"), BPM, bHat ? TEXT(" + hat") : TEXT(""));

			// Within ±1 BPM after 6 s, and from then on.
			float Worst = 0.f;
			float LowestConfidence = 1.f;
			for (const FStageAudioFeatures& Frame : Result.Frames)
			{
				if (Frame.Timestamp >= 6.0)
				{
					Worst = FMath::Max(Worst, FMath::Abs(Frame.BPM - BPM));
					LowestConfidence = FMath::Min(LowestConfidence, Frame.Confidence);
				}
			}
			AddInfo(FString::Printf(TEXT("%s: worst error after 6 s %.2f BPM, lowest confidence %.2f"), *Name, Worst, LowestConfidence));
			TestTrue(FString::Printf(TEXT("%s within 1 BPM after 6 s (worst %.2f)"), *Name, Worst), Worst <= 1.f);
			TestTrue(FString::Printf(TEXT("%s confident (%.2f)"), *Name, LowestConfidence), LowestConfidence >= 0.6f);

			// Beats land on the kicks: every beat from the fourth after the tempo was found is within 25 ms of a kick.
			const TArray<double> Kicks = StageAudioTestSignal::KickTimes(Params, 12.5);
			const TArray<double> Beats = BeatTimes(Result);
			double WorstBeat = 0.0;
			double MeanBeat = 0.0;
			int32 Counted = 0;
			for (int32 Index = 4; Index < Beats.Num(); ++Index)
			{
				const double Distance = NearestDistance(Beats[Index], Kicks);
				WorstBeat = FMath::Max(WorstBeat, Distance);
				MeanBeat += Distance;
				++Counted;
			}
			AddInfo(FString::Printf(TEXT("%s: %d beats, from the 5th worst %.1f ms, mean %.1f ms"), *Name, Beats.Num(), WorstBeat * 1000.0, Counted ? MeanBeat / Counted * 1000.0 : 0.0));
			TestTrue(FString::Printf(TEXT("%s beats counted"), *Name), Beats.Num() >= 8);
			TestTrue(FString::Printf(TEXT("%s beats on the kicks from the 5th (worst %.1f ms)"), *Name, WorstBeat * 1000.0), Counted > 0 && WorstBeat <= 0.025);
		}
	}

	// A tempo change re-locks: 120 for 10 s, then 128; within 1 BPM of 128 eight seconds later.
	{
		StageAudioTestSignal::FParams First;
		First.BPM = 120.f;
		First.FirstBeatSeconds = 0.5;
		StageAudioTestSignal::FParams Second = First;
		Second.BPM = 128.f;
		Second.FirstBeatSeconds = 10.0; // a beat of the first part falls on 10.0 s, so the music stays continuous
		const int64 Switch = 10 * StageAudioAnalysis::SampleRate;
		FRun Change;
		Run(20.0, [&](int64 Start, TArrayView<float> Out)
			{
				for (int32 Index = 0; Index < Out.Num(); ++Index)
				{
					StageAudioTestSignal::Generate(Start + Index < Switch ? First : Second, Start + Index, Out.Slice(Index, 1));
				}
			}, Change);
		const FStageAudioFeatures* Before = FrameAt(Change, 9.9);
		const FStageAudioFeatures* After = FrameAt(Change, 18.0);
		TestTrue(FString::Printf(TEXT("120 before the change (%.2f)"), Before ? Before->BPM : 0.f), Before && FMath::Abs(Before->BPM - 120.f) <= 1.f);
		TestTrue(FString::Printf(TEXT("128 eight seconds after (%.2f)"), After ? After->BPM : 0.f), After && FMath::Abs(After->BPM - 128.f) <= 1.f);
	}

	// Noise has no tempo: low confidence throughout.
	{
		StageAudioTestSignal::FParams Params;
		Params.bNoiseOnly = true;
		FRun Noise;
		Run(12.0, [&Params](int64 Start, TArrayView<float> Out) { StageAudioTestSignal::Generate(Params, Start, Out); }, Noise);
		float Highest = 0.f;
		for (const FStageAudioFeatures& Frame : Noise.Frames)
		{
			if (Frame.Timestamp >= 6.0)
			{
				Highest = FMath::Max(Highest, Frame.Confidence);
			}
		}
		AddInfo(FString::Printf(TEXT("Noise: highest confidence after 6 s %.2f"), Highest));
		TestTrue(FString::Printf(TEXT("Noise gives low confidence (%.2f)"), Highest), Highest < 0.4f);
	}

	// Determinism: the same input gives the same frames, whatever the block size.
	{
		StageAudioTestSignal::FParams Params;
		FRun A;
		Run(4.0, [&Params](int64 Start, TArrayView<float> Out) { StageAudioTestSignal::Generate(Params, Start, Out); }, A);
		StageAudioAnalysis::FFeatureExtractor Extractor;
		Extractor.Init(FStageAudioReactiveSettings());
		TArray<FStageAudioFeatures> B;
		TArray<float> Samples;
		int64 Start = 0;
		const int64 Total = 4 * StageAudioAnalysis::SampleRate;
		int32 Block = 1;
		while (Start < Total)
		{
			const int32 Count = int32(FMath::Min<int64>(Block, Total - Start));
			Samples.SetNumUninitialized(Count);
			StageAudioTestSignal::Generate(Params, Start, Samples);
			Extractor.Push(Samples, double(Start) / StageAudioAnalysis::SampleRate, [&B](const FStageAudioFeatures& Features) { B.Add(Features); });
			Start += Count;
			Block = Block * 3 % 1999 + 1; // odd sizes
		}
		bool bSame = A.Frames.Num() == B.Num();
		for (int32 Index = 0; bSame && Index < B.Num(); ++Index)
		{
			bSame = A.Frames[Index].Bass == B[Index].Bass && A.Frames[Index].OnsetCount == B[Index].OnsetCount && A.Frames[Index].BPM == B[Index].BPM;
		}
		TestTrue(TEXT("Same frames for any block size"), bSame);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioResamplerTest, "StageCraft.Audio.Resampler", StageAudioAnalysisTests::Flags)

bool FStageAudioResamplerTest::RunTest(const FString& Parameters)
{
	// 44.1 kHz to 48 kHz: the count follows the ratio, a sine keeps its frequency and level.
	StageAudioAnalysis::FLinearResampler Resampler;
	Resampler.Init(44100, 48000);
	TArray<float> Out;
	TArray<float> In;
	In.SetNumUninitialized(441);
	for (int32 Block = 0; Block < 100; ++Block)
	{
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			In[Index] = FMath::Sin(2.f * PI * 1000.f * float(double(Block * 441 + Index) / 44100.0));
		}
		Resampler.Process(In, Out);
	}
	TestTrue(FString::Printf(TEXT("One second in, one second out (%d)"), Out.Num()), FMath::Abs(Out.Num() - 48000) <= 2);
	double Error = 0.0;
	for (int32 Index = 0; Index < Out.Num(); ++Index)
	{
		// Output sample i lies at input position i x 44100/48000, after the first input sample.
		const double Expected = FMath::Sin(2.0 * PI * 1000.0 * double(Index) / 48000.0);
		Error = FMath::Max(Error, FMath::Abs(Expected - Out[Index]));
	}
	TestTrue(FString::Printf(TEXT("Sine preserved (max error %.3f)"), Error), Error < 0.05);

	StageAudioAnalysis::FLinearResampler Same;
	Same.Init(48000, 48000);
	TArray<float> Through;
	Same.Process(In, Through);
	TestEqual(TEXT("Same rate passes through"), Through, In);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
