// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the threaded side of audio-reactive lighting (Docs/ADR/0029 §3, §5, §9): the tap's ring buffer and the triple buffer,
// the analyser thread on the test signal, the Media Foundation file decoder and the world subsystem. They never record anything:
// the test signal is generated, the WAV files are written to the automation temp folder, and system audio, when a test opens it,
// is analysed in memory and closed again.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Audio; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "AudioDeviceManager.h"
#include "Audio/StageAudioAnalysis.h"
#include "Audio/StageAudioAnalyzer.h"
#include "Audio/StageAudioFileDecoder.h"
#include "Audio/StageAudioTap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Settings/StageCraftUserSettings.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"

namespace StageAudioAnalyzerTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	/** The engine ticks its audio devices every frame; a test that blocks the game thread does it itself, so sounds start and play. */
	void UpdateAudio()
	{
		if (FAudioDeviceManager* Devices = GEngine ? GEngine->GetAudioDeviceManager() : nullptr)
		{
			Devices->UpdateActiveAudioDevices(/*bGameTicking*/ true);
		}
	}

	/** Lets results that sources post to the game thread arrive while a test waits on the game thread. */
	void Pump(double Seconds)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			UpdateAudio();
			FPlatformProcess::Sleep(0.01f);
		}
	}

	/** Pumps until Done is true or Seconds pass. */
	bool PumpUntil(double Seconds, TFunctionRef<bool()> Done)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < End)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			UpdateAudio();
			if (Done())
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return Done();
	}

	/** A 16-bit PCM WAV of the test signal (interleaved, the same on every channel). Returns the samples written, as floats. */
	TArray<float> WriteWav(const FString& Path, int32 Rate, int32 Channels, double Seconds)
	{
		StageAudioTestSignal::FParams Params;
		Params.SampleRate = Rate;
		Params.BPM = 120.f;
		const int32 Frames = int32(Seconds * Rate);
		TArray<float> Mono;
		Mono.SetNumUninitialized(Frames);
		StageAudioTestSignal::Generate(Params, 0, Mono);

		TArray<uint8> Bytes;
		auto U32 = [&Bytes](uint32 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		auto U16 = [&Bytes](uint16 Value) { Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2); };
		const uint32 DataBytes = uint32(Frames * Channels * 2);
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4); U32(36 + DataBytes); Bytes.Append(reinterpret_cast<const uint8*>("WAVE"), 4);
		Bytes.Append(reinterpret_cast<const uint8*>("fmt "), 4); U32(16); U16(1); U16(uint16(Channels)); U32(uint32(Rate));
		U32(uint32(Rate * Channels * 2)); U16(uint16(Channels * 2)); U16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4); U32(DataBytes);
		TArray<float> Written;
		Written.Reserve(Frames * Channels);
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			const int16 Value = int16(FMath::Clamp(FMath::RoundToInt32(Mono[Frame] * 32767.f), -32768, 32767));
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				U16(uint16(Value));
				Written.Add(float(Value) / 32768.f);
			}
		}
		FFileHelper::SaveArrayToFile(Bytes, *Path);
		return Written;
	}

	/**
	 * A producer thread for the ring buffer test: writes a counting sequence in blocks of 480 at ten times the real-time rate of
	 * 48 kHz, paced by the clock (a sub-millisecond sleep is only a yield on Windows). Values stay below 2^24, exact in float.
	 */
	class FCountingProducer : public FRunnable
	{
	public:
		static constexpr double SamplesPerSecond = 480000.0;

		FCountingProducer(FStageAudioTap& InTap, double InSeconds) : Tap(InTap), Seconds(InSeconds) {}
		virtual uint32 Run() override
		{
			TArray<float> Block;
			Block.SetNumUninitialized(480);
			const double Start = FPlatformTime::Seconds();
			while (FPlatformTime::Seconds() - Start < Seconds)
			{
				while (double(Next) < (FPlatformTime::Seconds() - Start) * SamplesPerSecond)
				{
					for (float& Value : Block)
					{
						Value = float(Next++);
					}
					Tap.Write(Block.GetData(), Block.Num(), 1, FPlatformTime::Seconds());
				}
				FPlatformProcess::Sleep(0.001f);
			}
			return 0;
		}
		FStageAudioTap& Tap;
		double Seconds;
		int64 Next = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioRingBufferTest, "StageCraft.Audio.RingBuffer", StageAudioAnalyzerTests::Flags)

bool FStageAudioRingBufferTest::RunTest(const FString& Parameters)
{
	FStageAudioTap Tap;
	Tap.Init(1000, 48000);
	TestEqual(TEXT("Capacity rounds up to a power of two (and at least 1024)"), Tap.GetCapacity(), 1024);
	const float* Storage = Tap.GetStorageForTest();

	// Stereo down-mix, order across the wrap, and the clock of each block.
	TArray<float> Stereo;
	for (int32 Frame = 0; Frame < 300; ++Frame)
	{
		Stereo.Add(float(Frame));
		Stereo.Add(float(Frame) + 2.f);
	}
	int64 Expected = 0;
	bool bOrdered = true;
	bool bTimed = true;
	TArray<float> Out;
	Out.SetNumUninitialized(256);
	for (int32 Round = 0; Round < 20; ++Round)
	{
		Tap.Write(Stereo.GetData(), 300, 2, 100.0 + Round, 1.f);
		int32 Read = 0;
		while (Read < 300)
		{
			double Time = 0.0;
			const int32 Count = Tap.Read(Out, Time);
			if (Count <= 0)
			{
				break;
			}
			bTimed &= FMath::IsNearlyEqual(Time, 100.0 + Round + double(Read) / 48000.0, 1e-9);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				bOrdered &= Out[Index] == float((Read + Index) % 300) + 1.f;
			}
			Read += Count;
			Expected += Count;
		}
		TestEqual(TEXT("Each block read whole"), Read, 300);
	}
	TestTrue(TEXT("Mono down-mix, in order across the wrap"), bOrdered);
	TestTrue(TEXT("Each sample placed on the block's clock"), bTimed);
	TestEqual(TEXT("Nothing dropped"), Tap.GetDroppedSamples(), int64(0));

	// Overrun: the reader skips to the newest intact samples and counts what it lost.
	Tap.Reset();
	TArray<float> Mono;
	for (int32 Index = 0; Index < 3000; ++Index)
	{
		Mono.Add(float(Index));
	}
	Tap.Write(Mono.GetData(), Mono.Num(), 1, 1.0);
	double Time = 0.0;
	TArray<float> All;
	All.SetNumUninitialized(4096);
	const int32 Count = Tap.Read(All, Time);
	const int32 Safe = 1024 - 1024 / 4;
	TestEqual(TEXT("Overrun: the newest intact part is read"), Count, Safe);
	TestEqual(TEXT("Overrun: it starts at the oldest kept sample"), All[0], float(3000 - Safe));
	TestEqual(TEXT("Overrun: the rest counted as dropped"), Tap.GetDroppedSamples(), int64(3000 - Safe));
	TestTrue(TEXT("Overrun: its clock follows the block"), FMath::IsNearlyEqual(Time, 1.0 + double(3000 - Safe) / 48000.0, 1e-9));

	// No reallocation after Init.
	for (int32 Round = 0; Round < 2000; ++Round)
	{
		Tap.Write(Mono.GetData(), 500, 1, 2.0);
		Tap.Read(All, Time);
	}
	TestTrue(TEXT("Storage never moves after Init (10^6 samples)"), Tap.GetStorageForTest() == Storage && Tap.GetCapacity() == 1024);

	// A real producer thread against this consumer: the sequence arrives in order, gaps only where drops were counted.
	{
		FStageAudioTap Live;
		Live.Init(1 << 15, 48000);
		StageAudioAnalyzerTests::FCountingProducer Producer(Live, 1.0);
		TUniquePtr<FRunnableThread> Thread(FRunnableThread::Create(&Producer, TEXT("StageAudioRingBufferTest")));
		int64 Last = -1;
		int64 Received = 0;
		int64 Gaps = 0;
		bool bMonotonic = true;
		TArray<float> Block;
		Block.SetNumUninitialized(4096);
		const double Start = FPlatformTime::Seconds();
		bool bStalled = false;
		while (FPlatformTime::Seconds() - Start < 1.3)
		{
			double BlockTime = 0.0;
			const int32 Got = Live.Read(Block, BlockTime);
			for (int32 Index = 0; Index < Got; ++Index)
			{
				const int64 Value = int64(Block[Index]);
				if (Last >= 0 && Value != Last + 1)
				{
					bMonotonic &= Value > Last; // a drop skips forward, never back
					Gaps += Value - Last - 1;
				}
				Last = Value;
			}
			Received += Got;
			// Once, halfway, the consumer stalls for 120 ms: the producer laps the ring (24 576 samples is 51 ms here).
			if (!bStalled && FPlatformTime::Seconds() - Start > 0.5)
			{
				bStalled = true;
				FPlatformProcess::Sleep(0.120f);
			}
			else
			{
				FPlatformProcess::Sleep(0.002f);
			}
		}
		Thread->WaitForCompletion();
		AddInfo(FString::Printf(TEXT("Concurrent: %lld samples received, %lld dropped (%lld in gaps), last value %lld of %lld written"),
			Received, Live.GetDroppedSamples(), Gaps, Last, Producer.Next));
		TestTrue(TEXT("Concurrent: samples arrive"), Received > 100000);
		TestTrue(TEXT("Concurrent: order kept"), bMonotonic);
		TestTrue(TEXT("Concurrent: the stall dropped samples"), Live.GetDroppedSamples() > 0);
		TestEqual(TEXT("Concurrent: every gap was counted as a drop, and every drop is a gap"), Gaps, Live.GetDroppedSamples());
		TestEqual(TEXT("Concurrent: everything written was read or dropped"), Received + Live.GetDroppedSamples(), Producer.Next);
	}

	// Triple buffer: a writer thread publishes 1..N; the reader only ever sees complete, non-decreasing values, and the last one.
	{
		struct FPair { int64 A = 0; int64 B = 0; };
		TStageTripleBuffer<FPair> Buffer;
		std::atomic<bool> bDone{ false };
		constexpr int64 Last = 200000;
		TFuture<void> Writer = Async(EAsyncExecution::Thread, [&Buffer, &bDone]()
			{
				for (int64 Value = 1; Value <= Last; ++Value)
				{
					Buffer.Publish(FPair{ Value, -Value });
				}
				bDone = true;
			});
		int64 Previous = 0;
		bool bComplete = true;
		bool bNonDecreasing = true;
		while (!bDone)
		{
			const FPair Value = Buffer.Read();
			bComplete &= Value.A == -Value.B;
			bNonDecreasing &= Value.A >= Previous;
			Previous = Value.A;
		}
		Writer.Wait();
		TestTrue(TEXT("Triple buffer: never a torn value"), bComplete);
		TestTrue(TEXT("Triple buffer: never goes back"), bNonDecreasing);
		TestEqual(TEXT("Triple buffer: the last value arrives"), Buffer.Read().A, Last);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioAnalyzerTest, "StageCraft.Audio.Analyzer", StageAudioAnalyzerTests::Flags)

bool FStageAudioAnalyzerTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioAnalyzerTests;

	TSharedRef<FStageAudioTap> Tap = MakeShared<FStageAudioTap>();
	Tap->Init(1 << 17, StageAudioAnalysis::SampleRate);
	FStageAudioAnalyzer Analyzer;
	FStageAudioAnalyzerConfig Config;
	Config.bGenerateTestSignal = true;
	Config.TestSignal.BPM = 128.f;
	Config.HeardDelaySeconds = 0.25; // held a quarter of a second, as a file's output delay would be
	FText Error;
	TestTrue(TEXT("Starts"), Analyzer.Start(Tap, Config, Error));

	// Features reach the game thread, and the tempo is found within 8 s.
	const bool bLocked = PumpUntil(9.0, [&Analyzer]()
		{
			const FStageAudioFeatures Features = Analyzer.GetFeatures(FPlatformTime::Seconds());
			return Features.bSignal && FMath::Abs(Features.BPM - 128.f) <= 1.f && Features.Confidence > 0.5f;
		});
	const double Now = FPlatformTime::Seconds();
	const FStageAudioFeatures First = Analyzer.GetFeatures(Now);
	AddInfo(FString::Printf(TEXT("After %.1f s analysed: BPM %.2f, confidence %.2f, beats %lld, CPU %.3f %%"), Analyzer.GetAnalysedSeconds(), First.BPM,
		First.Confidence, First.BeatCount, Analyzer.GetCpuPercent()));
	TestTrue(FString::Printf(TEXT("128 BPM found (%.2f)"), First.BPM), bLocked);

	// Held until heard: the newest frame is never in the future, and not older than the hold plus a few frames.
	TestTrue(TEXT("Released no earlier than its heard time"), First.Timestamp <= Now + 1e-3);
	TestTrue(FString::Printf(TEXT("Released on time (%.0f ms old)"), (Now - First.Timestamp) * 1000.0), Now - First.Timestamp < 0.06);

	// Beats keep counting.
	Pump(1.0);
	const FStageAudioFeatures Later = Analyzer.GetFeatures(FPlatformTime::Seconds());
	TestTrue(FString::Printf(TEXT("Beats counted in a second (%lld)"), Later.BeatCount - First.BeatCount), Later.BeatCount - First.BeatCount >= 2);
	TestTrue(FString::Printf(TEXT("Analyser under 2 %% of a core (%.3f %%)"), Analyzer.GetCpuPercent()), Analyzer.GetCpuPercent() < 2.f);
	TestEqual(TEXT("Nothing dropped"), Tap->GetDroppedSamples(), int64(0));

	// Stop joins; twice is safe; stale features say there is no signal.
	Analyzer.Shutdown();
	TestFalse(TEXT("Stopped"), Analyzer.IsRunning());
	Analyzer.Shutdown();
	const FStageAudioFeatures Stale = Analyzer.GetFeatures(FPlatformTime::Seconds() + FStageAudioAnalyzer::StaleSeconds + 0.5);
	TestFalse(TEXT("Stale features have no signal"), Stale.bSignal);
	TestEqual(TEXT("Stale features have no level"), Stale.Level, 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioFileDecoderTest, "StageCraft.Audio.FileDecoder", StageAudioAnalyzerTests::Flags)

bool FStageAudioFileDecoderTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioAnalyzerTests;
#if PLATFORM_WINDOWS
	const FString Folder = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageAudioFileDecoder"));
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
	const FString Wav = FPaths::Combine(Folder, TEXT("Kicks.wav"));
	const TArray<float> Written = WriteWav(Wav, 44100, 2, 2.0);

	// Refused before any decoding.
	FText Error;
	TestFalse(TEXT("Missing file refused"), FStageAudioFileDecoder::ValidatePath(FPaths::Combine(Folder, TEXT("Nope.wav")), Error));
	const FString Text = FPaths::Combine(Folder, TEXT("Notes.txt"));
	FFileHelper::SaveStringToFile(TEXT("not music"), *Text);
	TestFalse(TEXT("Unsupported extension refused"), FStageAudioFileDecoder::ValidatePath(Text, Error));
	const FString Empty = FPaths::Combine(Folder, TEXT("Empty.wav"));
	FFileHelper::SaveArrayToFile(TArray<uint8>(), *Empty);
	TestFalse(TEXT("Empty file refused"), FStageAudioFileDecoder::ValidatePath(Empty, Error));
	TestTrue(TEXT("The WAV is accepted"), FStageAudioFileDecoder::ValidatePath(Wav, Error));

	// Decodes to exactly what was written (16-bit PCM is exact in float), with the file's rate and channels.
	{
		FStageAudioFileDecoder Decoder;
		std::atomic<int32> Opened{ 0 };
		FStageAudioFileInfo OpenedInfo;
		TestTrue(TEXT("Decoder starts"), Decoder.Start(Wav, 0.0, /*bLoop*/ false,
			[&Opened, &OpenedInfo](bool bOk, const FText&, const FStageAudioFileInfo& Info) { OpenedInfo = Info; Opened = bOk ? 1 : 2; }, Error));
		PumpUntil(5.0, [&Opened]() { return Opened != 0; });
		TestEqual(TEXT("Opened"), Opened.load(), 1);
		TestEqual(TEXT("Rate"), OpenedInfo.SampleRate, 44100);
		TestEqual(TEXT("Channels"), OpenedInfo.Channels, 2);
		TestTrue(FString::Printf(TEXT("Duration (%.3f s)"), OpenedInfo.DurationSeconds), FMath::IsNearlyEqual(OpenedInfo.DurationSeconds, 2.0, 0.01));

		TArray<float> Decoded;
		TArray<float> Block;
		Block.SetNumUninitialized(1024 * 2);
		PumpUntil(5.0, [&]()
			{
				const int32 Frames = Decoder.Read(Block.GetData(), 1024);
				Decoded.Append(Block.GetData(), Frames * 2);
				return Decoder.HasEnded();
			});
		TestEqual(TEXT("Every sample decoded"), Decoded.Num(), Written.Num());
		float Worst = 0.f;
		for (int32 Index = 0; Index < FMath::Min(Decoded.Num(), Written.Num()); ++Index)
		{
			Worst = FMath::Max(Worst, FMath::Abs(Decoded[Index] - Written[Index]));
		}
		TestTrue(FString::Printf(TEXT("Samples match (worst %.6f)"), Worst), Worst < 1e-4f);
		TestTrue(TEXT("Ended"), Decoder.HasEnded());
		Decoder.Shutdown();
		Decoder.Shutdown();
	}

	// Looping: the samples after the end are the start again.
	{
		FStageAudioFileDecoder Decoder;
		std::atomic<int32> Opened{ 0 };
		Decoder.Start(Wav, 0.0, /*bLoop*/ true, [&Opened](bool bOk, const FText&, const FStageAudioFileInfo&) { Opened = bOk ? 1 : 2; }, Error);
		PumpUntil(5.0, [&Opened]() { return Opened != 0; });
		TArray<float> Decoded;
		TArray<float> Block;
		Block.SetNumUninitialized(1024 * 2);
		PumpUntil(8.0, [&]()
			{
				const int32 Frames = Decoder.Read(Block.GetData(), 1024);
				Decoded.Append(Block.GetData(), Frames * 2);
				return Decoded.Num() >= Written.Num() + 44100 * 2;
			});
		TestTrue(TEXT("Looped past the end"), Decoded.Num() >= Written.Num() + 44100 * 2);
		bool bWrapped = Decoded.Num() >= Written.Num() + 2000;
		for (int32 Index = 0; bWrapped && Index < 2000; ++Index)
		{
			bWrapped = FMath::IsNearlyEqual(Decoded[Written.Num() + Index], Written[Index], 1e-4f);
		}
		TestTrue(TEXT("After the end comes the start"), bWrapped);
		TestFalse(TEXT("A looping file never ends"), Decoder.HasEnded());
	}

	// Seeking on open: starts one second in.
	{
		FStageAudioFileDecoder Decoder;
		std::atomic<int32> Opened{ 0 };
		Decoder.Start(Wav, 1.0, /*bLoop*/ false, [&Opened](bool bOk, const FText&, const FStageAudioFileInfo&) { Opened = bOk ? 1 : 2; }, Error);
		PumpUntil(5.0, [&Opened]() { return Opened != 0; });
		TArray<float> Block;
		Block.SetNumUninitialized(256 * 2);
		int32 Frames = 0;
		PumpUntil(5.0, [&]() { Frames = Decoder.Read(Block.GetData(), 256); return Frames > 0; });
		const int32 Start = 44100 * 2;
		TestTrue(TEXT("Seek on open lands on 1.0 s"), Frames > 0 && FMath::IsNearlyEqual(Block[0], Written[Start], 1e-4f) && FMath::IsNearlyEqual(Block[200], Written[Start + 200], 1e-4f));
	}

	// Garbage with a music extension: accepted by the path checks, refused by the decoder, with a reason.
	{
		const FString Garbage = FPaths::Combine(Folder, TEXT("Garbage.mp3"));
		TArray<uint8> Noise;
		for (int32 Index = 0; Index < 20000; ++Index)
		{
			Noise.Add(uint8((Index * 7919) ^ (Index >> 3)));
		}
		FFileHelper::SaveArrayToFile(Noise, *Garbage);
		FStageAudioFileDecoder Decoder;
		std::atomic<int32> Opened{ 0 };
		FText Reason;
		TestTrue(TEXT("Garbage: the decoder thread starts"), Decoder.Start(Garbage, 0.0, false,
			[&Opened, &Reason](bool bOk, const FText& InError, const FStageAudioFileInfo&) { Reason = InError; Opened = bOk ? 1 : 2; }, Error));
		PumpUntil(5.0, [&Opened]() { return Opened != 0; });
		TestEqual(TEXT("Garbage is refused"), Opened.load(), 2);
		TestFalse(TEXT("with a reason"), Reason.IsEmpty());
		TArray<float> Block;
		Block.SetNumUninitialized(64);
		TestEqual(TEXT("and reads nothing"), Decoder.Read(Block.GetData(), 32), 0);
	}
	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAudioSubsystemTest, "StageCraft.Audio.Subsystem", StageAudioAnalyzerTests::Flags)

bool FStageAudioSubsystemTest::RunTest(const FString& Parameters)
{
	using namespace StageAudioAnalyzerTests;

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageAudioReactiveTestWorld"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->GetWorldSettings()->NotifyBeginPlay();
	ON_SCOPE_EXIT
	{
		World->BeginTearingDown();
		World->EndPlay(EEndPlayReason::Quit);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		GEngine->DestroyWorldContext(World);
	};

	UStageAudioReactiveSubsystem* Audio = World->GetSubsystem<UStageAudioReactiveSubsystem>();
	if (!TestNotNull(TEXT("Subsystem exists in a game world"), Audio))
	{
		return false;
	}
	// The user's own settings are never touched: the subsystem reads and writes a fresh object.
	UStageCraftUserSettings* Settings = NewObject<UStageCraftUserSettings>();
	Settings->SetToDefaults();
	Audio->UseSettingsForTests(Settings);
	FText Error;
	Audio->SetSource(EStageAudioSource::Off, Error); // whatever this PC remembered was opened at BeginPlay

	int32 Changes = 0;
	const FDelegateHandle Handle = Audio->OnAudioStatusChanged.AddLambda([&Changes]() { ++Changes; });

	// The GameMode allows a player and refuses no player.
	AStageCraftGameModeBase* GameMode = World->SpawnActor<AStageCraftGameModeBase>();
	APlayerController* Player = World->SpawnActor<APlayerController>();
	TestTrue(TEXT("GameMode allows a player"), GameMode->EvaluateAudioRequest(Player, EStageAudioRequest::SetSource).IsSuccess());
	TestFalse(TEXT("GameMode refuses no player"), GameMode->EvaluateAudioRequest(nullptr, EStageAudioRequest::SetSource).IsSuccess());

	// File is chosen with OpenFile; bad paths change nothing.
	TestFalse(TEXT("SetSource(File) refused"), Audio->SetSource(EStageAudioSource::File, Error));
	TestFalse(TEXT("Wrong extension refused"), Audio->OpenFile(TEXT("C:/nowhere/song.txt"), Error));
	TestFalse(TEXT("Missing file refused"), Audio->OpenFile(TEXT("C:/nowhere/song.mp3"), Error));
	TestEqual(TEXT("Still off"), Audio->GetState(), EStageAudioSourceState::Off);
	FStageAudioTransportRequest Play;
	TestFalse(TEXT("Transport without a file refused"), Audio->Transport(Play, Error));

	// TestSignal: runs at once, finds its tempo, is remembered in the (test) settings.
	Changes = 0;
	TestTrue(TEXT("TestSignal starts"), Audio->SetSource(EStageAudioSource::TestSignal, Error));
	TestEqual(TEXT("Running"), Audio->GetState(), EStageAudioSourceState::Running);
	TestTrue(TEXT("Listeners told"), Changes > 0);
	TestEqual(TEXT("Remembered"), Settings->GetAudioReactiveSettings().LastSource, EStageAudioSource::TestSignal);
	const bool bTempo = PumpUntil(9.0, [Audio]() { const FStageAudioFeatures F = Audio->GetFeatures(); return F.bSignal && FMath::Abs(F.BPM - 128.f) <= 1.f; });
	const FStageAudioStatus Status = Audio->GetStatus();
	AddInfo(FString::Printf(TEXT("TestSignal: BPM %.2f, analyser CPU %.3f %%"), Status.Features.BPM, Status.AnalyzerCpuPercent));
	TestTrue(TEXT("TestSignal: 128 BPM through the subsystem"), bTempo);

	// Settings: hostile values sanitised; a running source keeps running with the new ones.
	FStageAudioReactiveSettings Hostile = Audio->GetSettings();
	Hostile.FileOutputDelayMs = 99999.f;
	Hostile.BassTopHz = 10.f;
	TestTrue(TEXT("Settings applied"), Audio->ApplySettings(Hostile, Error));
	TestEqual(TEXT("Delay clamped"), Audio->GetSettings().FileOutputDelayMs, StageAudioReactive::MaxFileOutputDelayMs);
	TestEqual(TEXT("Edges fall back"), Audio->GetSettings().BassTopHz, FStageAudioReactiveSettings().BassTopHz);
	TestEqual(TEXT("LastSource is not a setting"), Audio->GetSettings().LastSource, EStageAudioSource::TestSignal);
	TestEqual(TEXT("Still running"), Audio->GetState(), EStageAudioSourceState::Running);

	// System audio: opens on its own thread and never stays Starting (Running here, or Failed on a PC without a playback device).
	if (Audio->SetSource(EStageAudioSource::SystemAudio, Error))
	{
		PumpUntil(5.0, [Audio]() { return Audio->GetState() != EStageAudioSourceState::Starting; });
		AddInfo(FString::Printf(TEXT("System audio: %s %s"), *UEnum::GetValueAsString(Audio->GetState()), *Audio->GetStatus().Error.ToString()));
		TestTrue(TEXT("System audio settles"), Audio->GetState() == EStageAudioSourceState::Running || Audio->GetState() == EStageAudioSourceState::Failed);
	}

	// A WAV through the whole File path: opens, plays (if this session has an audio device), and the position moves.
	const FString Folder = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StageAudioSubsystem"));
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FString Wav = FPaths::Combine(Folder, TEXT("Kicks.wav"));
	WriteWav(Wav, 48000, 2, 4.0);
	TestTrue(TEXT("File opens"), Audio->OpenFile(Wav, Error));
	TestEqual(TEXT("File source"), Audio->GetSource(), EStageAudioSource::File);
	PumpUntil(5.0, [Audio]() { return Audio->GetState() != EStageAudioSourceState::Starting; });
	AddInfo(FString::Printf(TEXT("File: %s %s"), *UEnum::GetValueAsString(Audio->GetState()), *Audio->GetStatus().Error.ToString()));
	TestEqual(TEXT("A file is never remembered"), Settings->GetAudioReactiveSettings().LastSource, EStageAudioSource::Off);
	if (Audio->GetState() == EStageAudioSourceState::Running)
	{
		FStageAudioTransportRequest Seek;
		Seek.Command = EStageAudioTransportCommand::Seek;
		Seek.Seconds = 2.0;
		TestTrue(TEXT("Seek"), Audio->Transport(Seek, Error));
		PumpUntil(5.0, [Audio]() { return Audio->GetState() == EStageAudioSourceState::Running; });
		const bool bMoving = PumpUntil(3.0, [Audio]() { return Audio->GetStatus().PositionSeconds > 2.1; });
		AddInfo(FString::Printf(TEXT("File position after seek: %.2f s"), Audio->GetStatus().PositionSeconds));
		TestTrue(TEXT("Plays on from the seek point"), bMoving);
		FStageAudioTransportRequest Bad;
		Bad.Command = EStageAudioTransportCommand::Seek;
		Bad.Seconds = -5.0;
		TestFalse(TEXT("Negative seek refused"), Audio->Transport(Bad, Error));
	}

	// Off stops everything and forgets the source; features go quiet at once.
	TestTrue(TEXT("Off"), Audio->SetSource(EStageAudioSource::Off, Error));
	TestEqual(TEXT("State off"), Audio->GetState(), EStageAudioSourceState::Off);
	TestFalse(TEXT("No signal when off"), Audio->GetFeatures().bSignal);
	TestEqual(TEXT("Off remembered"), Settings->GetAudioReactiveSettings().LastSource, EStageAudioSource::Off);

	Audio->OnAudioStatusChanged.Remove(Handle);
	Audio->UseSettingsForTests(nullptr);
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
