// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "DSP/AlignedBuffer.h"
#include "DSP/FFTAlgorithm.h"
#include "Templates/UniquePtr.h"

/**
 * The analysis maths of audio-reactive lighting (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §3), free of engine
 * state: no world, no UObject, no clock, no thread. FStageAudioAnalyzer runs it on its own thread; tests feed it samples directly.
 * Every stage is deterministic for a given input (Rules.md §54).
 *
 * Pipeline per hop of 512 mono samples at 48 kHz (93.75 frames a second):
 *   FFT 2048 under a Hann window -> FBandAnalyzer (bands, level, signal) and FOnsetDetector (spectral flux, onsets)
 *   -> FTempoTracker (autocorrelation tempo every 0.5 s) -> FBeatTracker (beat phase and count).
 * FFeatureExtractor wires them together.
 */
namespace StageAudioAnalysis
{
	constexpr int32 SampleRate = 48000;
	constexpr int32 FFTSize = 2048;
	constexpr int32 HopSize = 512;
	constexpr int32 NumBins = FFTSize / 2 + 1;
	constexpr double FrameRate = double(SampleRate) / double(HopSize);

	constexpr float MinBPM = 60.f;
	constexpr float MaxBPM = 200.f;

	/** Edges in Hz from the sanitized settings: [lowest, sub-bass top, bass top, mid top, high top]. */
	struct FBandEdges
	{
		float Hz[5] = { 20.f, 60.f, 250.f, 4000.f, 16000.f };
		static FBandEdges FromSettings(const FStageAudioReactiveSettings& Settings);
	};

	/** The band values of one frame, before they become FStageAudioFeatures. */
	struct FBandFrame
	{
		float Bands[4] = { 0.f, 0.f, 0.f, 0.f };
		float Level = 0.f;
		/** Band energy before normalisation, dB of the mean square. */
		float BandDb[4] = { -120.f, -120.f, -120.f, -120.f };
		/** Mean square of the frame in dBFS (a full-scale sine is -3). */
		float LevelDb = -120.f;
		bool bSignal = false;
	};

	/**
	 * Band energy in dB normalised to the music's own loudness: an adaptive ceiling with an instant attack that falls by the range
	 * over 8 s, a range of 30 dB below it, an absolute floor at -70 dBFS; then attack (10 ms) and release (150 ms) smoothing.
	 * The signal is lost after 2 s below -60 dBFS and back as soon as a frame is above it.
	 */
	class MODULARSCENEBUILDER_API FBandAnalyzer
	{
	public:
		static constexpr float RangeDb = 30.f;
		static constexpr float CeilingFallSeconds = 8.f;
		static constexpr float AbsoluteFloorDb = -70.f;
		static constexpr float SilenceDb = -60.f;
		static constexpr float SilenceSeconds = 2.f;
		static constexpr float AttackSeconds = 0.010f;
		static constexpr float ReleaseSeconds = 0.150f;

		void Init(const FBandEdges& Edges);
		void Reset();

		/**
		 * PowerSpectrum: |X|^2 of NumBins bins of a Hann-windowed FFTSize frame. WindowPowerSum: the sum of the squared window,
		 * which turns the spectrum back into the signal's mean square (Parseval).
		 */
		void Process(TConstArrayView<float> PowerSpectrum, float WindowPowerSum, FBandFrame& Out);

	private:
		struct FChannel
		{
			float CeilingDb = AbsoluteFloorDb;
			float Smoothed = 0.f;
		};
		float Normalise(FChannel& Channel, float Db) const;

		int32 FirstBin[5] = { 0, 0, 0, 0, 0 };
		FChannel Channels[5];
		float SilentSeconds = SilenceSeconds;
		bool bSignal = false;
	};

	/** One detected onset. */
	struct FOnset
	{
		/** The middle of the analysis window that peaked, interpolated between frames (seconds of the stream clock). */
		double Time = 0.0;
		float Strength = 0.f;
	};

	/**
	 * Spectral-flux onsets (ADR 0029 §2.1, the detector G0 measured: 38/38 kicks, 27 ms): log-compressed magnitudes, half-wave
	 * rectified flux over all bins, a local maximum above median(16) x 1.5 + 5 % of a decaying maximum, 100 ms apart. Also keeps
	 * a low-band flux (below 250 Hz) for the tempo, where kicks dominate.
	 */
	class MODULARSCENEBUILDER_API FOnsetDetector
	{
	public:
		static constexpr double RefractorySeconds = 0.100;

		void Reset();

		/**
		 * Magnitudes of NumBins bins; FrameTime is the middle of the frame's window. Returns true when the previous frame was an
		 * onset (a peak is only known one frame later), filling OutOnset. OutFlux / OutLowFlux are this frame's flux values.
		 */
		bool Process(TConstArrayView<float> Magnitudes, double FrameTime, float& OutFlux, float& OutLowFlux, FOnset& OutOnset);

	private:
		TArray<float> PreviousLog;
		TArray<float> PreviousMagnitude;
		float History[16] = {};
		int32 HistoryCount = 0;
		int32 HistoryNext = 0;
		float RunningMax = 0.f;
		float Values[3] = { 0.f, 0.f, 0.f }; // [frame - 2, frame - 1, frame]
		double Times[3] = { 0.0, 0.0, 0.0 };
		float PreviousThreshold = TNumericLimits<float>::Max();
		double LastOnsetTime = -1.0e9;
	};

	/** The tempo estimate. */
	struct FTempoEstimate
	{
		float BPM = 0.f;
		/** 0..1 */
		float Confidence = 0.f;
		/** A time at which a beat falls (stream clock), the phase reference for the beat tracker. 0 when unknown. */
		double BeatTime = 0.0;
	};

	/**
	 * Tempo from the autocorrelation of an onset envelope over the last 6 s, re-estimated every 0.5 s, 60-200 BPM.
	 * - The envelope weights the low band (kicks) over the full flux, so off-beat hats do not double the tempo.
	 * - A mild prior around 120 BPM breaks near ties; then the shorter of two octave-related periods wins when it is nearly as strong
	 *   (174 is not reported as 87).
	 * - The peak is interpolated, then the period and the beat reference are fitted (least squares) to the onsets that fall on beats,
	 *   which gives about 0.1 BPM where the frame grid alone would give 5 BPM at 174.
	 * - Confidence: how many predicted beats had an onset, how distinct the autocorrelation peak is, and whether successive estimates agree.
	 */
	class MODULARSCENEBUILDER_API FTempoTracker
	{
	public:
		static constexpr double WindowSeconds = 6.0;
		static constexpr double MinimumSeconds = 3.0;
		static constexpr double UpdateSeconds = 0.5;

		void Reset();

		/** One frame of the envelope: the full and the low-band flux at FrameTime (frames arrive in order, FrameRate apart). */
		void PushFrame(float Flux, float LowFlux, double FrameTime);

		void PushOnset(const FOnset& Onset);

		/** Re-estimates when UpdateSeconds have passed since the last estimate; true when it did. */
		bool Update(double NowTime);

		const FTempoEstimate& GetEstimate() const { return Estimate; }

	private:
		static constexpr int32 Capacity = 1024; // > WindowSeconds * FrameRate

		float Envelope(int64 Frame) const { return Env[Frame & (Capacity - 1)]; }
		/** Least-squares fit of period and beat time to the onsets near the beats of a coarse period. False with too few. */
		bool FitBeats(double CoarsePeriod, double ReferenceTime, double& OutPeriod, double& OutBeatTime, float& OutHitFraction) const;

		float Env[Capacity] = {};
		double FrameTimes[Capacity] = {};
		int64 FramesPushed = 0;
		float FluxMax = 1e-6f;
		float LowFluxMax = 1e-6f;
		TArray<FOnset> Onsets; // the last WindowSeconds
		double LastUpdateTime = -1.0e9;
		FTempoEstimate Estimate;
		float PreviousBPM = 0.f;
		int32 AgreeingEstimates = 0;
	};

	/**
	 * The beat clock: phase advances at the tempo between estimates and is pulled smoothly toward each new beat reference, so
	 * a correction never makes it jump. BeatCount counts the times the phase wraps. Holds the last tempo when the estimate goes away.
	 */
	class MODULARSCENEBUILDER_API FBeatTracker
	{
	public:
		/** Fraction of the phase error removed per frame (about 50 ms to settle at 94 frames a second). */
		static constexpr double PullPerFrame = 0.2;

		void Reset();

		/** Advances to Time (stream clock) with the current estimate. */
		void Advance(double Time, const FTempoEstimate& Estimate);

		float GetPhase() const { return float(Beats - FMath::FloorToDouble(Beats)); }
		int64 GetBeatCount() const { return int64(FMath::FloorToDouble(Beats)); }
		float GetBPM() const { return float(BPM); }

	private:
		/** Beats since the tempo was first found; never decreases. Phase is its fraction, the count its whole part. */
		double Beats = 0.0;
		double BPM = 0.0;
		double LastTime = 0.0;
		bool bStarted = false;
	};

	/** Stateful linear resampler, mono. Good enough for analysis (bands up to 16 kHz, onsets); never used for playback. */
	class MODULARSCENEBUILDER_API FLinearResampler
	{
	public:
		void Init(int32 InputRate, int32 OutputRate);
		void Reset();

		/** Appends the resampled Input to Out. */
		void Process(TConstArrayView<float> Input, TArray<float>& Out);

		int32 GetInputRate() const { return InputRate; }

	private:
		int32 InputRate = SampleRate;
		int32 OutputRate = SampleRate;
		double Step = 1.0;
		/** Position of the next output sample relative to Previous (0..1 between Previous and the next input sample). */
		double Position = 0.0;
		float Previous = 0.f;
		bool bHasPrevious = false;
	};

	/**
	 * The whole pipeline: mono 48 kHz samples in, one FStageAudioFeatures per hop out. Timestamps are on the caller's clock: the
	 * caller says when the first sample of each block happened (TimeOfFirstSample) and adds the heard delay itself.
	 * Allocates in Init only (Rules.md §63: the analyser's hot path).
	 */
	class MODULARSCENEBUILDER_API FFeatureExtractor
	{
	public:
		FFeatureExtractor();
		~FFeatureExtractor();

		/** False when the platform has no FFT of this size (never on Windows; the analyser then reports Failed). */
		bool Init(const FStageAudioReactiveSettings& Settings);
		void Reset();

		/**
		 * Feeds mono 48 kHz samples. OnFrame is called once per completed hop with the features at the time of that hop's last
		 * sample (Timestamp = stream time, not yet the heard time).
		 */
		void Push(TConstArrayView<float> Samples, double TimeOfFirstSample, TFunctionRef<void(const FStageAudioFeatures&)> OnFrame);

		/** Onsets found so far, for tests (the detector's own times, interpolated). */
		void SetOnsetSink(TArray<FOnset>* Sink) { OnsetSink = Sink; }

	private:
		void ProcessFrame(double TimeOfLastSample, TFunctionRef<void(const FStageAudioFeatures&)> OnFrame);

		TUniquePtr<Audio::IFFTAlgorithm> FFT;
		TArray<float> Window;
		float WindowPowerSum = 0.f;
		TArray<float> Ring; // the last FFTSize samples, oldest first
		int32 Filled = 0;
		int32 SinceHop = 0;
		Audio::FAlignedFloatBuffer Frame;
		Audio::FAlignedFloatBuffer Spectrum;
		TArray<float> Power;
		TArray<float> Magnitudes;

		FBandAnalyzer Bands;
		FOnsetDetector Onsets;
		FTempoTracker Tempo;
		FBeatTracker Beat;
		int64 OnsetCount = 0;
		TArray<FOnset>* OnsetSink = nullptr;
	};
}

/** The deterministic test source (ADR 0029 §3 TestSignal). */
namespace StageAudioTestSignal
{
	struct FParams
	{
		float BPM = 128.f;
		/** The first kick, in seconds from sample 0. */
		double FirstBeatSeconds = 0.5;
		/** An off-beat hi-hat (noise burst) between the kicks. */
		bool bHiHat = true;
		/** A quiet 1 kHz pad and a noise floor, so the bands and the noise floor are never empty. */
		bool bPad = true;
		/** Gain on everything, linear. */
		float Gain = 1.f;
		/** Only noise (no kick, hat or pad): a signal with no tempo. */
		bool bNoiseOnly = false;
		int32 SampleRate = StageAudioAnalysis::SampleRate;
	};

	/** The kick times of Params in [0, Seconds). */
	MODULARSCENEBUILDER_API TArray<double> KickTimes(const FParams& Params, double Seconds);

	/** Writes Out.Num() mono samples starting at absolute sample StartSample. The same sample index always gives the same value. */
	MODULARSCENEBUILDER_API void Generate(const FParams& Params, int64 StartSample, TArrayView<float> Out);
}
