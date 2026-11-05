// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioAnalyzer.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "ModularSceneBuilder.h"

#define LOCTEXT_NAMESPACE "StageAudioAnalyzer"

namespace
{
	/** How often the thread looks at the tap and the held frames: a frame is released at most this late. */
	constexpr float WakeSeconds = 0.004f;

	/** Up to this many samples read per pass (about 85 ms at 48 kHz). */
	constexpr int32 ReadBlock = 4096;

	/** Frames held for their heard time: room for 2.7 s of frames at 94 a second, more than the 0.58 s longest delay. */
	constexpr int32 HeldCapacity = 256;

	/** The test signal is produced in 10 ms blocks, as a capture device delivers. */
	constexpr int32 TestSignalBlock = 480;
}

FStageAudioAnalyzer::FStageAudioAnalyzer() = default;

FStageAudioAnalyzer::~FStageAudioAnalyzer()
{
	Shutdown();
}

bool FStageAudioAnalyzer::Start(const TSharedRef<FStageAudioTap>& InTap, const FStageAudioAnalyzerConfig& InConfig, FText& OutError)
{
	Shutdown();
	Config = InConfig;
	Config.Settings = StageAudioReactive::Sanitize(InConfig.Settings);
	Tap = InTap;
	if (!Extractor.Init(Config.Settings))
	{
		OutError = LOCTEXT("NoFFT", "Audio analysis is not available on this platform (no FFT).");
		Tap.Reset();
		return false;
	}
	Resampler.Init(Tap->GetSampleRate(), StageAudioAnalysis::SampleRate);
	Raw.SetNumUninitialized(ReadBlock);
	Resampled.Reset();
	Resampled.Reserve(ReadBlock * 2 + 16);
	TestBlock.SetNumUninitialized(TestSignalBlock);
	Held.SetNum(HeldCapacity);
	HeldFirst = 0;
	HeldCount = 0;
	NextStreamTime = 0.0;
	TestSignalStart = FPlatformTime::Seconds();
	TestSignalSamples = 0;
	Published.Reset();
	ProcessingCycles = 0;
	AnalysedSamples = 0;

	bStopping = false;
	Thread.Reset(FRunnableThread::Create(this, TEXT("StageCraft Audio Analysis"), 0, TPri_AboveNormal));
	if (!Thread)
	{
		OutError = LOCTEXT("NoThread", "Audio analysis could not start its thread.");
		Tap.Reset();
		return false;
	}
	return true;
}

void FStageAudioAnalyzer::Shutdown()
{
	if (Thread)
	{
		bStopping = true;
		Thread->WaitForCompletion();
		Thread.Reset();
	}
	Tap.Reset();
}

FStageAudioFeatures FStageAudioAnalyzer::GetFeatures(double NowSeconds)
{
	FStageAudioFeatures Features = Published.Read();
	if (NowSeconds - Features.Timestamp > StaleSeconds)
	{
		Features.SubBass = Features.Bass = Features.Mid = Features.High = Features.Level = 0.f;
		Features.bSignal = false;
	}
	return Features;
}

float FStageAudioAnalyzer::GetCpuPercent() const
{
	const double Seconds = GetAnalysedSeconds();
	return Seconds > 0.0 ? float(double(ProcessingCycles.load()) * FPlatformTime::GetSecondsPerCycle64() / Seconds * 100.0) : 0.f;
}

double FStageAudioAnalyzer::GetAnalysedSeconds() const
{
	return double(AnalysedSamples.load()) / double(StageAudioAnalysis::SampleRate);
}

uint32 FStageAudioAnalyzer::Run()
{
	while (!bStopping)
	{
		const double Now = FPlatformTime::Seconds();
		if (Config.bGenerateTestSignal)
		{
			GenerateTestSignal(Now);
		}
		Analyse(Now);
		Release(FPlatformTime::Seconds());
		FPlatformProcess::SleepNoStats(WakeSeconds);
	}
	return 0;
}

void FStageAudioAnalyzer::GenerateTestSignal(double NowSeconds)
{
	// Everything due up to now, in device-sized blocks stamped with the time they "happened", as a capture source would deliver.
	const int64 Due = int64((NowSeconds - TestSignalStart) * double(StageAudioAnalysis::SampleRate));
	while (TestSignalSamples + TestSignalBlock <= Due && !bStopping)
	{
		StageAudioTestSignal::Generate(Config.TestSignal, TestSignalSamples, TestBlock);
		Tap->Write(TestBlock.GetData(), TestSignalBlock, 1, TestSignalStart + double(TestSignalSamples) / double(StageAudioAnalysis::SampleRate));
		TestSignalSamples += TestSignalBlock;
	}
}

void FStageAudioAnalyzer::Analyse(double NowSeconds)
{
	const double InputRate = double(Tap->GetSampleRate());
	for (;;)
	{
		double TimeOfFirst = 0.0;
		const int32 Count = Tap->Read(Raw, TimeOfFirst);
		if (Count <= 0)
		{
			return;
		}
		const uint64 StartCycles = FPlatformTime::Cycles64();
		// A block the tap could not place on the clock continues from the previous one.
		const double BlockTime = TimeOfFirst > 0.0 ? TimeOfFirst : NextStreamTime;
		NextStreamTime = BlockTime + double(Count) / InputRate;

		Resampled.Reset();
		Resampler.Process(MakeArrayView(Raw.GetData(), Count), Resampled);
		Extractor.Push(Resampled, BlockTime, [this](const FStageAudioFeatures& Frame)
			{
				FStageAudioFeatures Heard = Frame;
				Heard.Timestamp += Config.HeardDelaySeconds;
				Hold(Heard);
			});
		AnalysedSamples.fetch_add(Resampled.Num(), std::memory_order_relaxed);
		ProcessingCycles.fetch_add(int64(FPlatformTime::Cycles64() - StartCycles), std::memory_order_relaxed);
		if (bStopping)
		{
			return;
		}
	}
}

void FStageAudioAnalyzer::Hold(const FStageAudioFeatures& Frame)
{
	if (HeldCount == HeldCapacity)
	{
		// Far behind (a stall, a huge catch-up): the oldest frame goes out early rather than being lost.
		Published.Publish(Held[HeldFirst]);
		HeldFirst = (HeldFirst + 1) % HeldCapacity;
		--HeldCount;
	}
	Held[(HeldFirst + HeldCount) % HeldCapacity] = Frame;
	++HeldCount;
}

void FStageAudioAnalyzer::Release(double NowSeconds)
{
	// Only the newest due frame matters to a reader, but each is published in order so the counters stay monotonic.
	while (HeldCount > 0 && Held[HeldFirst].Timestamp <= NowSeconds)
	{
		Published.Publish(Held[HeldFirst]);
		HeldFirst = (HeldFirst + 1) % HeldCapacity;
		--HeldCount;
	}
}

#undef LOCTEXT_NAMESPACE
