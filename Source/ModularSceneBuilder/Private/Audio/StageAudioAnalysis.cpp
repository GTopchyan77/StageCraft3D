// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioAnalysis.h"

#include <algorithm>

namespace StageAudioAnalysis
{
	namespace
	{
		constexpr double FrameSeconds = 1.0 / FrameRate;

		/** Low band of the tempo envelope: kicks and bass notes. */
		constexpr float LowBandTopHz = 250.f;

		/** G0's log compression: log(1 + Gamma x |X| / N) on the textbook DFT, written on unitary magnitudes. */
		constexpr float FluxGamma = 100.f;

		/** An onset is a local maximum above Median x FluxMedianFactor + FluxMaxFactor x the decaying maximum. */
		constexpr float FluxMedianFactor = 1.5f;
		constexpr float FluxMaxFactor = 0.05f;
		constexpr float RunningMaxDecay = 0.999f;

		/**
		 * Below this flux nothing is an onset, whatever the relative threshold says: a steady tone's frame-to-frame rounding is not a
		 * note. A -40 dBFS note starting on a few bins still rises by about 0.5.
		 */
		constexpr float MinimumOnsetFlux = 0.1f;

		/** Weight of the full-band flux next to the low-band flux in the tempo envelope (hats count, kicks count more). */
		constexpr float FullBandTempoWeight = 0.35f;
		constexpr float EnvelopeMaxDecay = 0.999f;

		/** The tempo prior: a log-normal around 120 BPM, one octave wide. Only breaks near ties. */
		constexpr double PriorCentreBPM = 120.0;
		constexpr double PriorOctaves = 1.0;

		/** The shorter of two octave-related periods wins at this fraction of the longer one's strength. */
		constexpr double OctaveRatio = 0.75;

		/** An onset belongs to a beat when it lies within this fraction of a period of it. */
		constexpr double BeatTolerance = 0.15;

		/** The comb that places the beat looks this far back. */
		constexpr double CombSeconds = 4.0;

		/** A refined period may differ from the autocorrelation's by at most this fraction, or it is not trusted. */
		constexpr double MaxRefinement = 0.04;

		/** Successive estimates within this many BPM agree. */
		constexpr float AgreeBPM = 1.5f;

		double Frac(double Value) { return Value - FMath::FloorToDouble(Value); }

		/** Wraps a phase difference to [-0.5, 0.5). */
		double WrapHalf(double Value) { return Frac(Value + 0.5) - 0.5; }

		float ToDb(float MeanSquare) { return 10.f * FMath::LogX(10.f, MeanSquare + 1e-12f); }

		float SmoothingCoefficient(float Seconds) { return 1.f - FMath::Exp(-float(FrameSeconds) / FMath::Max(Seconds, 1e-4f)); }

		/** Multiplier that turns the FFT's output into unitary (energy-preserving) magnitudes. */
		float UnitaryScale(Audio::EFFTScaling Scaling)
		{
			const float N = float(FFTSize);
			switch (Scaling)
			{
			case Audio::EFFTScaling::None: return 1.f;
			case Audio::EFFTScaling::MultipliedByFFTSize: return 1.f / N;
			case Audio::EFFTScaling::MultipliedBySqrtFFTSize: return 1.f / FMath::Sqrt(N);
			case Audio::EFFTScaling::DividedByFFTSize: return N;
			case Audio::EFFTScaling::DividedBySqrtFFTSize: return FMath::Sqrt(N);
			}
			return 1.f;
		}
	}

	// --- Bands ---

	FBandEdges FBandEdges::FromSettings(const FStageAudioReactiveSettings& InSettings)
	{
		const FStageAudioReactiveSettings Settings = StageAudioReactive::Sanitize(InSettings);
		FBandEdges Edges;
		Edges.Hz[0] = StageAudioReactive::MinBandHz;
		Edges.Hz[1] = Settings.SubBassTopHz;
		Edges.Hz[2] = Settings.BassTopHz;
		Edges.Hz[3] = Settings.MidTopHz;
		Edges.Hz[4] = Settings.HighTopHz;
		return Edges;
	}

	void FBandAnalyzer::Init(const FBandEdges& Edges)
	{
		const double BinHz = double(SampleRate) / double(FFTSize);
		for (int32 Edge = 0; Edge < 5; ++Edge)
		{
			FirstBin[Edge] = FMath::Clamp(FMath::CeilToInt32(double(Edges.Hz[Edge]) / BinHz), 1, NumBins - 1);
			if (Edge > 0)
			{
				// Every band keeps at least one bin, whatever the edges.
				FirstBin[Edge] = FMath::Min(FMath::Max(FirstBin[Edge], FirstBin[Edge - 1] + 1), NumBins - 1);
			}
		}
		Reset();
	}

	void FBandAnalyzer::Reset()
	{
		for (FChannel& Channel : Channels)
		{
			Channel = FChannel();
		}
		SilentSeconds = SilenceSeconds;
		bSignal = false;
	}

	float FBandAnalyzer::Normalise(FChannel& Channel, float Db) const
	{
		// The ceiling follows the loudest recent value at once and falls by the whole range over CeilingFallSeconds, never below
		// a floor that keeps a near-silent signal from being stretched to full scale.
		const float Fall = RangeDb / CeilingFallSeconds * float(FrameSeconds);
		Channel.CeilingDb = FMath::Max3(Db, Channel.CeilingDb - Fall, AbsoluteFloorDb + 10.f);
		const float Target = Db < AbsoluteFloorDb ? 0.f : FMath::Clamp((Db - (Channel.CeilingDb - RangeDb)) / RangeDb, 0.f, 1.f);
		const float Coefficient = Target > Channel.Smoothed ? SmoothingCoefficient(AttackSeconds) : SmoothingCoefficient(ReleaseSeconds);
		Channel.Smoothed += (Target - Channel.Smoothed) * Coefficient;
		return Channel.Smoothed;
	}

	void FBandAnalyzer::Process(TConstArrayView<float> PowerSpectrum, float WindowPowerSum, FBandFrame& Out)
	{
		if (PowerSpectrum.Num() < NumBins || WindowPowerSum <= 0.f)
		{
			return;
		}
		// Unitary spectrum of a windowed frame: mean square = 2 x sum(|X|^2) / sum(w^2) over the positive bins.
		const float Scale = 2.f / WindowPowerSum;
		float Total = 0.f;
		for (int32 Bin = 1; Bin < NumBins; ++Bin)
		{
			Total += PowerSpectrum[Bin];
		}
		for (int32 Band = 0; Band < 4; ++Band)
		{
			float Sum = 0.f;
			for (int32 Bin = FirstBin[Band]; Bin < FirstBin[Band + 1]; ++Bin)
			{
				Sum += PowerSpectrum[Bin];
			}
			Out.BandDb[Band] = ToDb(Sum * Scale);
			Out.Bands[Band] = Normalise(Channels[Band], Out.BandDb[Band]);
		}
		Out.LevelDb = ToDb(Total * Scale);
		Out.Level = Normalise(Channels[4], Out.LevelDb);

		if (Out.LevelDb >= SilenceDb)
		{
			SilentSeconds = 0.f;
			bSignal = true;
		}
		else
		{
			SilentSeconds += float(FrameSeconds);
			bSignal = bSignal && SilentSeconds < SilenceSeconds;
		}
		Out.bSignal = bSignal;
	}

	// --- Onsets ---

	void FOnsetDetector::Reset()
	{
		*this = FOnsetDetector();
	}

	bool FOnsetDetector::Process(TConstArrayView<float> Magnitudes, double FrameTime, float& OutFlux, float& OutLowFlux, FOnset& OutOnset)
	{
		const int32 LowTopBin = FMath::CeilToInt32(LowBandTopHz * float(FFTSize) / float(SampleRate));
		const float Gamma = FluxGamma / FMath::Sqrt(float(FFTSize));
		const bool bFirst = PreviousLog.Num() != Magnitudes.Num();
		if (bFirst)
		{
			PreviousLog.SetNumZeroed(Magnitudes.Num());
			PreviousMagnitude.SetNumZeroed(Magnitudes.Num());
		}
		float Flux = 0.f;
		float LowFlux = 0.f;
		for (int32 Bin = 0; Bin < Magnitudes.Num(); ++Bin)
		{
			const float Log = FMath::Loge(1.f + Gamma * Magnitudes[Bin]);
			Flux += FMath::Max(0.f, Log - PreviousLog[Bin]);
			PreviousLog[Bin] = Log;
			// The low band rises in plain magnitude: log compression would make a hat's faint low end rise as much as a kick.
			if (Bin >= 1 && Bin < LowTopBin)
			{
				LowFlux += FMath::Max(0.f, Magnitudes[Bin] - PreviousMagnitude[Bin]);
			}
			PreviousMagnitude[Bin] = Magnitudes[Bin];
		}
		if (bFirst)
		{
			Flux = 0.f; // The first frame rises from nothing; it is not an onset.
			LowFlux = 0.f;
		}
		OutFlux = Flux;
		OutLowFlux = LowFlux;

		Values[0] = Values[1];
		Values[1] = Values[2];
		Values[2] = Flux;
		Times[0] = Times[1];
		Times[1] = Times[2];
		Times[2] = FrameTime;
		RunningMax = FMath::Max(RunningMax * RunningMaxDecay, Flux);

		// The previous frame is an onset when it is a local maximum above the threshold it had when it arrived.
		bool bOnset = false;
		if (Values[1] > Values[0] && Values[1] >= Values[2] && Values[1] > PreviousThreshold && Values[1] > MinimumOnsetFlux
			&& Times[1] - LastOnsetTime > RefractorySeconds)
		{
			const float Curvature = Values[0] - 2.f * Values[1] + Values[2];
			const double Offset = Curvature < 0.f ? FMath::Clamp(0.5 * double(Values[0] - Values[2]) / double(Curvature), -0.5, 0.5) : 0.0;
			OutOnset.Time = Times[1] + Offset * FrameSeconds;
			OutOnset.Strength = Values[1];
			LastOnsetTime = Times[1];
			bOnset = true;
		}

		History[HistoryNext] = Flux;
		HistoryNext = (HistoryNext + 1) % UE_ARRAY_COUNT(History);
		HistoryCount = FMath::Min(HistoryCount + 1, int32(UE_ARRAY_COUNT(History)));
		float Sorted[UE_ARRAY_COUNT(History)];
		FMemory::Memcpy(Sorted, History, sizeof(float) * HistoryCount);
		std::nth_element(Sorted, Sorted + HistoryCount / 2, Sorted + HistoryCount);
		PreviousThreshold = Sorted[HistoryCount / 2] * FluxMedianFactor + FluxMaxFactor * RunningMax;
		return bOnset;
	}

	// --- Tempo ---

	void FTempoTracker::Reset()
	{
		FMemory::Memzero(Env, sizeof(Env));
		FMemory::Memzero(FrameTimes, sizeof(FrameTimes));
		FramesPushed = 0;
		FluxMax = 1e-6f;
		LowFluxMax = 1e-6f;
		Onsets.Reset();
		LastUpdateTime = -1.0e9;
		Estimate = FTempoEstimate();
		PreviousBPM = 0.f;
		AgreeingEstimates = 0;
	}

	void FTempoTracker::PushFrame(float Flux, float LowFlux, double FrameTime)
	{
		FluxMax = FMath::Max(FluxMax * EnvelopeMaxDecay, Flux);
		LowFluxMax = FMath::Max(LowFluxMax * EnvelopeMaxDecay, LowFlux);
		const float Value = LowFlux / FMath::Max(LowFluxMax, 1e-3f) + FullBandTempoWeight * Flux / FMath::Max(FluxMax, 1e-3f);
		Env[FramesPushed & (Capacity - 1)] = Value;
		FrameTimes[FramesPushed & (Capacity - 1)] = FrameTime;
		++FramesPushed;
	}

	void FTempoTracker::PushOnset(const FOnset& Onset)
	{
		// Onsets come in time order; keep the window only (a handful of entries a second).
		Onsets.Add(Onset);
		const double Oldest = Onset.Time - WindowSeconds;
		int32 Drop = 0;
		while (Drop < Onsets.Num() && Onsets[Drop].Time < Oldest)
		{
			++Drop;
		}
		if (Drop > 0)
		{
			Onsets.RemoveAt(0, Drop, EAllowShrinking::No);
		}
	}

	bool FTempoTracker::FitBeats(double CoarsePeriod, double ReferenceTime, double& OutPeriod, double& OutBeatTime, float& OutHitFraction) const
	{
		if (Onsets.Num() < 4 || CoarsePeriod <= 0.0)
		{
			return false;
		}
		// Beat index k of each onset that falls on a beat of the coarse grid; fit t = a + k x P by least squares.
		double SumK = 0.0, SumT = 0.0, SumKK = 0.0, SumKT = 0.0;
		int32 Count = 0;
		int64 MinK = TNumericLimits<int64>::Max(), MaxK = TNumericLimits<int64>::Lowest();
		TSet<int64, DefaultKeyFuncs<int64>, TInlineSetAllocator<64>> Hit;
		for (const FOnset& Onset : Onsets)
		{
			const double Beats = (Onset.Time - ReferenceTime) / CoarsePeriod;
			const double K = FMath::RoundToDouble(Beats);
			if (FMath::Abs(Beats - K) > BeatTolerance)
			{
				continue;
			}
			const double T = Onset.Time - ReferenceTime; // relative, for precision
			SumK += K;
			SumT += T;
			SumKK += K * K;
			SumKT += K * T;
			++Count;
			MinK = FMath::Min(MinK, int64(K));
			MaxK = FMath::Max(MaxK, int64(K));
			Hit.Add(int64(K));
		}
		const double Span = double(MaxK - MinK);
		if (Count < 4 || Span < 3.0)
		{
			return false;
		}
		const double MeanK = SumK / Count;
		const double MeanT = SumT / Count;
		const double VarK = SumKK / Count - MeanK * MeanK;
		if (VarK <= 0.0)
		{
			return false;
		}
		const double Period = (SumKT / Count - MeanK * MeanT) / VarK;
		if (FMath::Abs(Period - CoarsePeriod) > MaxRefinement * CoarsePeriod)
		{
			return false;
		}
		const double Intercept = MeanT - Period * MeanK;
		OutPeriod = Period;
		OutBeatTime = ReferenceTime + Intercept + double(MaxK) * Period;
		// Beats the window could hold, against the beats that had an onset.
		const double WindowBeats = FMath::Max(1.0, FMath::Min(WindowSeconds, Onsets.Last().Time - Onsets[0].Time) / Period);
		OutHitFraction = float(FMath::Clamp(double(Hit.Num()) / WindowBeats, 0.0, 1.0));
		return true;
	}

	bool FTempoTracker::Update(double NowTime)
	{
		if (NowTime - LastUpdateTime < UpdateSeconds)
		{
			return false;
		}
		const int32 WindowFrames = FMath::Min(int32(FramesPushed), FMath::RoundToInt32(WindowSeconds * FrameRate));
		if (WindowFrames < FMath::RoundToInt32(MinimumSeconds * FrameRate))
		{
			return false;
		}
		LastUpdateTime = NowTime;

		// The window, oldest first, without its mean (a constant would correlate at every lag).
		const int64 First = FramesPushed - WindowFrames;
		float Window[Capacity];
		double Mean = 0.0;
		for (int32 Index = 0; Index < WindowFrames; ++Index)
		{
			Window[Index] = Envelope(First + Index);
			Mean += Window[Index];
		}
		Mean /= WindowFrames;
		for (int32 Index = 0; Index < WindowFrames; ++Index)
		{
			Window[Index] -= float(Mean);
		}

		// Normalised autocorrelation r(L) for the lags of 60..200 BPM, one lag either side for the peak interpolation.
		const int32 MinLag = FMath::FloorToInt32(FrameRate * 60.0 / MaxBPM) - 1;
		const int32 MaxLag = FMath::CeilToInt32(FrameRate * 60.0 / MinBPM) + 1;
		double Energy = 0.0;
		for (int32 Index = 0; Index < WindowFrames; ++Index)
		{
			Energy += double(Window[Index]) * Window[Index];
		}
		Energy /= WindowFrames;
		double R[128] = {};
		check(MaxLag < UE_ARRAY_COUNT(R));
		if (Energy <= 1e-9)
		{
			Estimate.Confidence *= 0.5f;
			return true;
		}
		for (int32 Lag = MinLag; Lag <= MaxLag; ++Lag)
		{
			double Sum = 0.0;
			for (int32 Index = Lag; Index < WindowFrames; ++Index)
			{
				Sum += double(Window[Index]) * Window[Index - Lag];
			}
			R[Lag] = Sum / double(WindowFrames - Lag) / Energy;
		}

		auto IsPeak = [&R](int32 Lag) { return R[Lag] >= R[Lag - 1] && R[Lag] >= R[Lag + 1]; };
		auto ToBPM = [](double Lag) { return 60.0 * FrameRate / Lag; };
		int32 Best = INDEX_NONE;
		double BestScore = 0.0;
		for (int32 Lag = MinLag + 1; Lag < MaxLag; ++Lag)
		{
			const double BPM = ToBPM(Lag);
			if (BPM < MinBPM - 1.0 || BPM > MaxBPM + 1.0 || !IsPeak(Lag) || R[Lag] <= 0.0)
			{
				continue;
			}
			const double Octaves = FMath::Log2(BPM / PriorCentreBPM) / PriorOctaves;
			const double Score = R[Lag] * FMath::Exp(-0.5 * Octaves * Octaves);
			if (Score > BestScore)
			{
				BestScore = Score;
				Best = Lag;
			}
		}
		if (Best == INDEX_NONE || R[Best] < 0.05)
		{
			Estimate.Confidence *= 0.5f;
			AgreeingEstimates = 0;
			return true;
		}
		// A strong pulse at half the period is the beat; its double is a bar half.
		for (;;)
		{
			const int32 Half = FMath::RoundToInt32(Best * 0.5);
			int32 HalfPeak = INDEX_NONE;
			for (int32 Lag = Half - 1; Lag <= Half + 1; ++Lag)
			{
				if (Lag > MinLag && Lag < MaxLag && IsPeak(Lag) && (HalfPeak == INDEX_NONE || R[Lag] > R[HalfPeak]))
				{
					HalfPeak = Lag;
				}
			}
			if (HalfPeak == INDEX_NONE || ToBPM(HalfPeak) > MaxBPM + 1.0 || R[HalfPeak] < OctaveRatio * R[Best])
			{
				break;
			}
			Best = HalfPeak;
		}

		const double Curvature = R[Best - 1] - 2.0 * R[Best] + R[Best + 1];
		const double LagOffset = Curvature < 0.0 ? FMath::Clamp(0.5 * (R[Best - 1] - R[Best + 1]) / Curvature, -0.5, 0.5) : 0.0;
		const double LagFrames = double(Best) + LagOffset;
		double Period = LagFrames / FrameRate;

		// Where the beats fall: the comb offset (frames back from the newest) that collects the most envelope over CombSeconds.
		const int64 Newest = FramesPushed - 1;
		const int32 CombBeats = FMath::Max(1, FMath::FloorToInt32(FMath::Min(CombSeconds, WindowFrames / FrameRate) / Period));
		int32 BestOffset = 0;
		double BestComb = -1.0;
		const int32 Offsets = FMath::CeilToInt32(LagFrames);
		for (int32 Offset = 0; Offset < Offsets; ++Offset)
		{
			double Comb = 0.0;
			for (int32 Beat = 0; Beat < CombBeats; ++Beat)
			{
				const double Back = Offset + Beat * LagFrames;
				const int32 Whole = FMath::FloorToInt32(Back);
				if (Whole + 1 >= WindowFrames)
				{
					break;
				}
				const float Fraction = float(Back - Whole);
				Comb += FMath::Lerp(Envelope(Newest - Whole), Envelope(Newest - Whole - 1), Fraction);
			}
			if (Comb > BestComb)
			{
				BestComb = Comb;
				BestOffset = Offset;
			}
		}
		double BeatTime = FrameTimes[Newest & (Capacity - 1)] - BestOffset / FrameRate;

		// The onsets on that grid pin the period and the beat down below a frame.
		float HitFraction = 0.f;
		double FittedPeriod = Period;
		double FittedBeat = BeatTime;
		if (FitBeats(Period, BeatTime, FittedPeriod, FittedBeat, HitFraction))
		{
			Period = FittedPeriod;
			BeatTime = FittedBeat;
		}

		const float BPM = float(FMath::Clamp(60.0 / Period, double(MinBPM), double(MaxBPM)));
		AgreeingEstimates = FMath::Abs(BPM - PreviousBPM) <= AgreeBPM ? AgreeingEstimates + 1 : 0;
		PreviousBPM = BPM;
		const float Stability = FMath::Min(1.f, float(AgreeingEstimates + 1) / 3.f);
		const float Peak = FMath::Min(1.f, float(R[Best]) * 1.5f);
		Estimate.BPM = BPM;
		Estimate.BeatTime = BeatTime;
		Estimate.Confidence = FMath::Clamp((0.5f * HitFraction + 0.5f * Peak) * (0.5f + 0.5f * Stability), 0.f, 1.f);
		return true;
	}

	// --- Beat ---

	void FBeatTracker::Reset()
	{
		*this = FBeatTracker();
	}

	void FBeatTracker::Advance(double Time, const FTempoEstimate& Estimate)
	{
		if (!bStarted)
		{
			if (Estimate.BPM <= 0.f || Estimate.BeatTime == 0.0)
			{
				return;
			}
			BPM = Estimate.BPM;
			Beats = Frac((Time - Estimate.BeatTime) * BPM / 60.0);
			LastTime = Time;
			bStarted = true;
			return;
		}
		const double Elapsed = FMath::Max(0.0, Time - LastTime);
		LastTime = Time;
		if (Estimate.BPM > 0.f)
		{
			BPM = Estimate.BPM;
		}
		double Step = Elapsed * BPM / 60.0;
		if (Estimate.BeatTime != 0.0)
		{
			const double Target = (Time - Estimate.BeatTime) * BPM / 60.0;
			Step += PullPerFrame * WrapHalf(Target - (Beats + Step));
		}
		// Beats never run backwards: a pull against the music at most holds the phase still, so no beat is counted twice.
		Beats += FMath::Max(0.0, Step);
	}

	// --- Resampler ---

	void FLinearResampler::Init(int32 InInputRate, int32 InOutputRate)
	{
		InputRate = FMath::Max(InInputRate, 1);
		OutputRate = FMath::Max(InOutputRate, 1);
		Step = double(InputRate) / double(OutputRate);
		Reset();
	}

	void FLinearResampler::Reset()
	{
		Position = 0.0;
		Previous = 0.f;
		bHasPrevious = false;
	}

	void FLinearResampler::Process(TConstArrayView<float> Input, TArray<float>& Out)
	{
		if (InputRate == OutputRate)
		{
			Out.Append(Input.GetData(), Input.Num());
			return;
		}
		for (const float Sample : Input)
		{
			if (!bHasPrevious)
			{
				Previous = Sample;
				bHasPrevious = true;
				continue;
			}
			// Output samples between Previous (position 0) and Sample (position 1).
			while (Position < 1.0)
			{
				Out.Add(FMath::Lerp(Previous, Sample, float(Position)));
				Position += Step;
			}
			Position -= 1.0;
			Previous = Sample;
		}
	}

	// --- Pipeline ---

	FFeatureExtractor::FFeatureExtractor() = default;
	FFeatureExtractor::~FFeatureExtractor() = default;

	bool FFeatureExtractor::Init(const FStageAudioReactiveSettings& Settings)
	{
		Audio::FFFTSettings FFTSettings;
		FFTSettings.Log2Size = FMath::FloorLog2(FFTSize);
		FFTSettings.bArrays128BitAligned = true; // the engine's implementations refuse unaligned arrays (G0)
		FFTSettings.bEnableHardwareAcceleration = true;
		FFT = Audio::FFFTFactory::NewFFTAlgorithm(FFTSettings);
		if (!FFT)
		{
			return false;
		}
		Window.SetNumUninitialized(FFTSize);
		WindowPowerSum = 0.f;
		for (int32 Index = 0; Index < FFTSize; ++Index)
		{
			Window[Index] = 0.5f - 0.5f * FMath::Cos(2.f * PI * float(Index) / float(FFTSize - 1));
			WindowPowerSum += Window[Index] * Window[Index];
		}
		Ring.SetNumZeroed(FFTSize);
		Frame.SetNumZeroed(FFT->NumInputFloats());
		Spectrum.SetNumZeroed(FFT->NumOutputFloats());
		Power.SetNumZeroed(NumBins);
		Magnitudes.SetNumZeroed(NumBins);
		Bands.Init(FBandEdges::FromSettings(Settings));
		Reset();
		return true;
	}

	void FFeatureExtractor::Reset()
	{
		FMemory::Memzero(Ring.GetData(), Ring.Num() * sizeof(float));
		Filled = 0;
		Bands.Reset();
		Onsets.Reset();
		Tempo.Reset();
		Beat.Reset();
		OnsetCount = 0;
	}

	void FFeatureExtractor::Push(TConstArrayView<float> Samples, double TimeOfFirstSample, TFunctionRef<void(const FStageAudioFeatures&)> OnFrame)
	{
		if (!FFT)
		{
			return;
		}
		int32 Index = 0;
		while (Index < Samples.Num())
		{
			const int32 Count = FMath::Min(Samples.Num() - Index, FFTSize - Filled);
			FMemory::Memcpy(Ring.GetData() + Filled, Samples.GetData() + Index, Count * sizeof(float));
			Filled += Count;
			Index += Count;
			if (Filled == FFTSize)
			{
				ProcessFrame(TimeOfFirstSample + double(Index - 1) / double(SampleRate), OnFrame);
				FMemory::Memmove(Ring.GetData(), Ring.GetData() + HopSize, (FFTSize - HopSize) * sizeof(float));
				Filled = FFTSize - HopSize;
			}
		}
	}

	void FFeatureExtractor::ProcessFrame(double TimeOfLastSample, TFunctionRef<void(const FStageAudioFeatures&)> OnFrame)
	{
		for (int32 Index = 0; Index < FFTSize; ++Index)
		{
			Frame[Index] = Ring[Index] * Window[Index];
		}
		FFT->ForwardRealToComplex(Frame.GetData(), Spectrum.GetData());
		const float Scale = UnitaryScale(FFT->ForwardScaling());
		for (int32 Bin = 0; Bin < NumBins; ++Bin)
		{
			const float Re = Spectrum[2 * Bin] * Scale;
			const float Im = Spectrum[2 * Bin + 1] * Scale;
			Power[Bin] = Re * Re + Im * Im;
			Magnitudes[Bin] = FMath::Sqrt(Power[Bin]);
		}

		FBandFrame BandFrame;
		Bands.Process(Power, WindowPowerSum, BandFrame);

		// Onsets and the tempo envelope are placed at the middle of the window.
		const double FrameMiddle = TimeOfLastSample - 0.5 * double(FFTSize - 1) / double(SampleRate);
		float Flux = 0.f;
		float LowFlux = 0.f;
		FOnset Onset;
		if (Onsets.Process(Magnitudes, FrameMiddle, Flux, LowFlux, Onset))
		{
			++OnsetCount;
			Tempo.PushOnset(Onset);
			if (OnsetSink)
			{
				OnsetSink->Add(Onset);
			}
		}
		Tempo.PushFrame(Flux, LowFlux, FrameMiddle);
		Tempo.Update(FrameMiddle);
		Beat.Advance(TimeOfLastSample, Tempo.GetEstimate());

		FStageAudioFeatures Features;
		Features.SubBass = BandFrame.Bands[0];
		Features.Bass = BandFrame.Bands[1];
		Features.Mid = BandFrame.Bands[2];
		Features.High = BandFrame.Bands[3];
		Features.Level = BandFrame.Level;
		Features.bSignal = BandFrame.bSignal;
		Features.OnsetCount = OnsetCount;
		Features.BPM = Beat.GetBPM();
		Features.BeatPhase = Beat.GetPhase();
		Features.BeatCount = Beat.GetBeatCount();
		Features.Confidence = Tempo.GetEstimate().Confidence;
		Features.Timestamp = TimeOfLastSample;
		OnFrame(Features);
	}
}

namespace StageAudioTestSignal
{
	namespace
	{
		constexpr double KickSeconds = 0.150;
		constexpr double HatSeconds = 0.030;

		/** Deterministic white noise in [-1, 1) from a sample index and a stream id (no state, so chunking never matters). */
		float Noise(int64 Index, uint32 Stream)
		{
			uint64 X = uint64(Index) * 0x9E3779B97F4A7C15ull + uint64(Stream) * 0xD1B54A32D192ED03ull;
			X ^= X >> 33;
			X *= 0xff51afd7ed558ccdull;
			X ^= X >> 33;
			X *= 0xc4ceb9fe1a85ec53ull;
			X ^= X >> 33;
			return float(double(X >> 11) / double(1ull << 53)) * 2.f - 1.f;
		}
	}

	TArray<double> KickTimes(const FParams& Params, double Seconds)
	{
		TArray<double> Times;
		if (Params.bNoiseOnly || Params.BPM <= 0.f)
		{
			return Times;
		}
		const double Period = 60.0 / Params.BPM;
		for (double Time = Params.FirstBeatSeconds; Time < Seconds; Time += Period)
		{
			Times.Add(Time);
		}
		return Times;
	}

	void Generate(const FParams& Params, int64 StartSample, TArrayView<float> Out)
	{
		const double Rate = double(FMath::Max(Params.SampleRate, 1));
		const double Period = Params.BPM > 0.f ? 60.0 / Params.BPM : 1.0;
		for (int32 Index = 0; Index < Out.Num(); ++Index)
		{
			const int64 Sample = StartSample + Index;
			const double Time = double(Sample) / Rate;
			double Value = 0.004 * Noise(Sample, 1); // noise floor, about -50 dBFS
			if (Params.bNoiseOnly)
			{
				Value = 0.3 * Noise(Sample, 2);
			}
			else
			{
				const double SinceFirst = Time - Params.FirstBeatSeconds;
				if (SinceFirst >= 0.0)
				{
					// Kick: a sine falling from 150 to 50 Hz under a fast exponential decay.
					const double BeatIndex = FMath::FloorToDouble(SinceFirst / Period);
					const double SinceKick = SinceFirst - BeatIndex * Period;
					if (SinceKick < KickSeconds)
					{
						const double Phase = 2.0 * PI * (50.0 * SinceKick + (100.0 * 0.03) * (1.0 - FMath::Exp(-SinceKick / 0.03)));
						Value += 0.8 * FMath::Exp(-SinceKick / 0.045) * FMath::Sin(Phase);
					}
					// Hat: a short burst of differentiated noise half a beat later.
					const double SinceHat = SinceKick - 0.5 * Period;
					if (Params.bHiHat && SinceHat >= 0.0 && SinceHat < HatSeconds)
					{
						const double Burst = Noise(Sample, 3) - Noise(Sample - 1, 3);
						Value += 0.15 * FMath::Exp(-SinceHat / 0.008) * Burst;
					}
				}
				if (Params.bPad)
				{
					Value += 0.03 * FMath::Sin(2.0 * PI * 1000.0 * Time);
				}
			}
			Out[Index] = float(Value * Params.Gain);
		}
	}
}
