// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Audio/StageAudioAnalysis.h"
#include "Audio/StageAudioReactiveTypes.h"
#include "Audio/StageAudioTap.h"
#include "HAL/Runnable.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

/** Everything the analyser needs to start. */
struct FStageAudioAnalyzerConfig
{
	FStageAudioReactiveSettings Settings;
	/** Added to every frame's stream time: when the audience hears it (StageAudioReactive::GetHeardDelaySeconds). */
	double HeardDelaySeconds = 0.0;
	/** The analyser produces the TestSignal itself into the tap, in real time, at 48 kHz. */
	bool bGenerateTestSignal = false;
	StageAudioTestSignal::FParams TestSignal;
};

/**
 * The analysis thread of audio-reactive lighting (Docs/ADR/0029-audio-analysis-and-audio-reactive-effects.md §3): reads the tap,
 * resamples to 48 kHz, runs StageAudioAnalysis::FFeatureExtractor and hands the frames to the game thread through a lock-free
 * triple buffer. Each frame is held until the moment the audience hears the audio it describes (its Timestamp), so a reader
 * always gets the music of "now", not the music the analyser has just seen.
 *
 * Threads: Start, Shutdown and the getters run on the game thread; the thread is the tap's only reader. Shutdown is idempotent and
 * joins the thread; the destructor shuts down. Not a UObject (UStageAudioReactiveSubsystem owns it). Allocates at Start only.
 */
class FStageAudioAnalyzer : private FRunnable
{
public:
	FStageAudioAnalyzer();
	virtual ~FStageAudioAnalyzer() override;
	FStageAudioAnalyzer(const FStageAudioAnalyzer&) = delete;
	FStageAudioAnalyzer& operator=(const FStageAudioAnalyzer&) = delete;

	/** Starts reading Tap. False, with OutError, when the FFT or the thread cannot be created; nothing is left running. */
	bool Start(const TSharedRef<FStageAudioTap>& Tap, const FStageAudioAnalyzerConfig& Config, FText& OutError);

	/** Joins the thread. Safe when not running and when called twice. */
	void Shutdown();

	bool IsRunning() const { return Thread.IsValid(); }

	/**
	 * Game thread: a new heard delay (a latency offset changed), used from the next analysed block on. The analysis keeps running,
	 * so the tempo is not found again. Frames already held keep the delay they were stamped with.
	 */
	void SetHeardDelaySeconds(double Seconds) { HeardDelaySeconds.store(Seconds, std::memory_order_relaxed); }

	/**
	 * Game thread: the newest released frame. When nothing has been released for StaleSeconds (the source went quiet or stalled),
	 * the frame is returned without signal and with the bands at 0, so effects fall back instead of freezing on old music.
	 */
	FStageAudioFeatures GetFeatures(double NowSeconds);

	/** Analyser thread time per second of audio analysed, in % of one core. */
	float GetCpuPercent() const;
	double GetAnalysedSeconds() const;

	static constexpr double StaleSeconds = 1.0;

private:
	//~ Begin FRunnable Interface
	virtual uint32 Run() override;
	virtual void Stop() override { bStopping = true; }
	//~ End FRunnable Interface

	void GenerateTestSignal(double NowSeconds);
	void Analyse(double NowSeconds);
	void Hold(const FStageAudioFeatures& Frame);
	void Release(double NowSeconds);

	FStageAudioAnalyzerConfig Config;
	TSharedPtr<FStageAudioTap> Tap;
	TUniquePtr<class FRunnableThread> Thread;
	std::atomic<bool> bStopping{ false };
	/** Config.HeardDelaySeconds at Start, then SetHeardDelaySeconds: written by the game thread, read by the thread per block. */
	std::atomic<double> HeardDelaySeconds{ 0.0 };

	// The thread's.
	StageAudioAnalysis::FFeatureExtractor Extractor;
	StageAudioAnalysis::FLinearResampler Resampler;
	TArray<float> Raw;
	TArray<float> Resampled;
	double NextStreamTime = 0.0;
	double TestSignalStart = 0.0;
	int64 TestSignalSamples = 0;
	TArray<float> TestBlock;
	/** Frames waiting for their Timestamp: a fixed ring. */
	TArray<FStageAudioFeatures> Held;
	int32 HeldFirst = 0;
	int32 HeldCount = 0;

	TStageTripleBuffer<FStageAudioFeatures> Published;
	std::atomic<int64> ProcessingCycles{ 0 };
	std::atomic<int64> AnalysedSamples{ 0 };
};
