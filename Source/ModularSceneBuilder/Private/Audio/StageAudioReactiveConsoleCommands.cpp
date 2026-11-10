// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for audio-reactive lighting (Docs/ADR/0029 §5): choose the source, drive the file transport, change
// the settings and read the analysis, through AModularPlayerController's audio requests (the path the Audio panel will use).
// For scripted runs and measurements (MeasureDelay); the Audio panel is the user's way in. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Audio/StageAudioAnalyzer.h"
#include "Audio/StageAudioCaptureSources.h"
#include "Audio/StageAudioSubsystem.h"
#include "Audio/StageAudioTap.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include <atomic>
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageAudioReactiveSubsystem.h"

namespace StageAudioReactiveCommands
{
	AModularPlayerController* GetController(UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music: no AModularPlayerController in this world."));
		}
		return Controller;
	}

	bool ParseOnOff(const FString& Value, bool& bOut)
	{
		if (Value.Equals(TEXT("On"), ESearchCase::IgnoreCase) || Value == TEXT("1")) { bOut = true; return true; }
		if (Value.Equals(TEXT("Off"), ESearchCase::IgnoreCase) || Value == TEXT("0")) { bOut = false; return true; }
		return false;
	}

	void LogStatus(UStageAudioReactiveSubsystem& Audio)
	{
		const FStageAudioStatus Status = Audio.GetStatus();
		const FStageAudioFeatures& F = Status.Features;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music: %s %s%s%s"), *UEnum::GetValueAsString(Status.Source), *UEnum::GetValueAsString(Status.State),
			Status.Description.IsEmpty() ? TEXT("") : TEXT(", "), *Status.Description);
		if (!Status.Error.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music: %s"), *Status.Error.ToString());
		}
		if (Status.Source == EStageAudioSource::File)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music: file %.2f / %.2f s, %s%s, underruns %lld"), Status.PositionSeconds, Status.DurationSeconds,
				Status.bPlaying ? TEXT("playing") : (Status.bPaused ? TEXT("paused") : TEXT("stopped")), Status.bLoop ? TEXT(", loop") : TEXT(""), Status.Underruns);
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music: signal %s, BPM %.2f, confidence %.2f, beat %lld phase %.2f, onsets %lld, bands %.2f %.2f %.2f %.2f, level %.2f"),
			F.bSignal ? TEXT("yes") : TEXT("no"), F.BPM, F.Confidence, F.BeatCount, F.BeatPhase, F.OnsetCount, F.SubBass, F.Bass, F.Mid, F.High, F.Level);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music: analysed %.1f s, analyser CPU %.3f %% of a core, dropped %lld samples"),
			Status.AnalysedSeconds, Status.AnalyzerCpuPercent, Status.DroppedSamples);
	}

	/** Off | Input | SystemAudio | TestSignal [BPM=<n>] | File <path> */
	void Source(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		UStageAudioReactiveSubsystem* Audio = World ? World->GetSubsystem<UStageAudioReactiveSubsystem>() : nullptr;
		if (!Controller || !Audio || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Source Off | Input | SystemAudio | TestSignal [BPM=<n>] | File <path>"));
			return;
		}
		const FString& Name = Args[0];
		if (Name.Equals(TEXT("File"), ESearchCase::IgnoreCase))
		{
			FString Path = FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "));
			Path.TrimQuotesInline();
			Controller->RequestOpenAudioFile(Path);
			LogStatus(*Audio);
			return;
		}
		const int64 Value = StaticEnum<EStageAudioSource>()->GetValueByNameString(Name);
		if (Value == INDEX_NONE || Value == int64(EStageAudioSource::File))
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Source: unknown source \"%s\"."), *Name);
			return;
		}
		for (int32 Index = 1; Index < Args.Num(); ++Index)
		{
			FString Key, Text;
			if (Args[Index].Split(TEXT("="), &Key, &Text) && Key.Equals(TEXT("BPM"), ESearchCase::IgnoreCase))
			{
				FStageAudioReactiveSettings Settings = Audio->GetSettings();
				Settings.TestSignalBPM = FCString::Atof(*Text);
				Controller->RequestApplyAudioSettings(Settings);
			}
		}
		Controller->RequestSetAudioSource(EStageAudioSource(Value));
		LogStatus(*Audio);
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageAudioReactiveSubsystem* Audio = World ? World->GetSubsystem<UStageAudioReactiveSubsystem>() : nullptr)
		{
			LogStatus(*Audio);
		}
	}

	/** Play | Pause | Stop | Seek <seconds> | Loop On|Off */
	void Transport(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Transport Play | Pause | Stop | Seek <seconds> | Loop On|Off"));
			return;
		}
		FStageAudioTransportRequest Request;
		const FString& Command = Args[0];
		if (Command.Equals(TEXT("Play"), ESearchCase::IgnoreCase)) { Request.Command = EStageAudioTransportCommand::Play; }
		else if (Command.Equals(TEXT("Pause"), ESearchCase::IgnoreCase)) { Request.Command = EStageAudioTransportCommand::Pause; }
		else if (Command.Equals(TEXT("Stop"), ESearchCase::IgnoreCase)) { Request.Command = EStageAudioTransportCommand::Stop; }
		else if (Command.Equals(TEXT("Seek"), ESearchCase::IgnoreCase) && Args.Num() > 1)
		{
			Request.Command = EStageAudioTransportCommand::Seek;
			Request.Seconds = FCString::Atod(*Args[1]);
		}
		else if (Command.Equals(TEXT("Loop"), ESearchCase::IgnoreCase) && Args.Num() > 1 && ParseOnOff(Args[1], Request.bLoop))
		{
			Request.Command = EStageAudioTransportCommand::SetLoop;
		}
		else
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Transport: unknown command."));
			return;
		}
		Controller->RequestAudioTransport(Request);
	}

	/** [FileDelay=<ms>] [FileOffset|InputOffset|SystemOffset=<ms>] [Gain=<dB>] [Device=<id>|Default] [Loop=On|Off] [Background=On|Off] [FlashLimit=<Hz>] */
	void Settings(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		UStageAudioReactiveSubsystem* Audio = World ? World->GetSubsystem<UStageAudioReactiveSubsystem>() : nullptr;
		if (!Controller || !Audio)
		{
			return;
		}
		FStageAudioReactiveSettings Settings = Audio->GetSettings();
		bool bChanged = false;
		for (const FString& Arg : Args)
		{
			FString Key, Text;
			if (!Arg.Split(TEXT("="), &Key, &Text))
			{
				continue;
			}
			bool bFlag = false;
			if (Key.Equals(TEXT("FileDelay"), ESearchCase::IgnoreCase)) { Settings.FileOutputDelayMs = FCString::Atof(*Text); bChanged = true; }
			else if (Key.Equals(TEXT("FileOffset"), ESearchCase::IgnoreCase)) { Settings.FileLatencyOffsetMs = FCString::Atof(*Text); bChanged = true; }
			else if (Key.Equals(TEXT("InputOffset"), ESearchCase::IgnoreCase)) { Settings.InputLatencyOffsetMs = FCString::Atof(*Text); bChanged = true; }
			else if (Key.Equals(TEXT("SystemOffset"), ESearchCase::IgnoreCase)) { Settings.SystemAudioLatencyOffsetMs = FCString::Atof(*Text); bChanged = true; }
			else if (Key.Equals(TEXT("Gain"), ESearchCase::IgnoreCase)) { Settings.InputGainDb = FCString::Atof(*Text); bChanged = true; }
			else if (Key.Equals(TEXT("Device"), ESearchCase::IgnoreCase)) { Settings.InputDeviceId = Text.Equals(TEXT("Default"), ESearchCase::IgnoreCase) ? FString() : Text; bChanged = true; }
			else if (Key.Equals(TEXT("Loop"), ESearchCase::IgnoreCase) && ParseOnOff(Text, bFlag)) { Settings.bLoopFile = bFlag; bChanged = true; }
			else if (Key.Equals(TEXT("Background"), ESearchCase::IgnoreCase) && ParseOnOff(Text, bFlag)) { Controller->RequestSetPlayAudioInBackground(bFlag); }
			else if (Key.Equals(TEXT("FlashLimit"), ESearchCase::IgnoreCase)) { Settings.FlashLimitHz = FCString::Atof(*Text); bChanged = true; }
		}
		if (bChanged)
		{
			Controller->RequestApplyAudioSettings(Settings);
		}
		const FStageAudioReactiveSettings Now = Audio->GetSettings();
		const UStageAudioSubsystem* Sound = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UStageAudioSubsystem>() : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Settings: file delay %.0f ms, offsets file %.0f / input %.0f / system %.0f ms, input gain %.1f dB, device \"%s\", loop %s, bands %.0f/%.0f/%.0f/%.0f Hz, background %s, flash limit %.0f Hz"),
			Now.FileOutputDelayMs, Now.FileLatencyOffsetMs, Now.InputLatencyOffsetMs, Now.SystemAudioLatencyOffsetMs, Now.InputGainDb, *Now.InputDeviceId,
			Now.bLoopFile ? TEXT("on") : TEXT("off"), Now.SubBassTopHz, Now.BassTopHz, Now.MidTopHz, Now.HighTopHz,
			Sound && Sound->IsPlayedInBackground() ? TEXT("on") : TEXT("off"), Now.FlashLimitHz);
	}

	/** <0..1>: the AUDIO master, through the controller (Docs/ADR/0029 §7). */
	void Master(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UShowControlSubsystem* Show = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		if (!Controller || !Show)
		{
			return;
		}
		if (!Args.IsEmpty())
		{
			Controller->RequestSetAudioMaster(FCString::Atof(*Args[0]));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Master: AUDIO %.2f, KILL %s"), Show->GetAudioMaster(), Show->IsAudioKilled() ? TEXT("on") : TEXT("off"));
	}

	/** The Audio panel's FOLLOW MUSIC on the selected fixtures, through the controller. */
	void Follow(const TArray<FString>& Args, UWorld* World)
	{
		if (AModularPlayerController* Controller = GetController(World))
		{
			const TArray<FGuid> Ids = Controller->RequestFollowMusic();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Follow: %s (%d effects)."), Ids.IsEmpty() ? TEXT("refused") : TEXT("started"), Ids.Num());
		}
	}

	/** On|Off: AUDIO KILL, through the controller. */
	void Kill(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = GetController(World);
		const UShowControlSubsystem* Show = World ? World->GetSubsystem<UShowControlSubsystem>() : nullptr;
		bool bOn = false;
		if (!Controller || !Show)
		{
			return;
		}
		if (!Args.IsEmpty() && ParseOnOff(Args[0], bOn))
		{
			Controller->RequestSetAudioKill(bOn);
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Kill: KILL %s, AUDIO %.2f"), Show->IsAudioKilled() ? TEXT("on") : TEXT("off"), Show->GetAudioMaster());
	}

	void Devices(const TArray<FString>& Args, UWorld* World)
	{
		for (const TPair<FString, FString>& Device : FStageAudioInputCapture::ListDevices())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Devices: \"%s\" id=%s"), *Device.Key, *Device.Value);
		}
	}

	/**
	 * Measures the File source's heard delay (Docs/ADR/0029 §13, the G1 follow-up): while the song plays, a second WASAPI loopback
	 * hears what Windows actually outputs and a second analyser follows it (heard delay 0). Both find the same beats with the same
	 * maths, so the gap between their beat phases is the error in FileOutputDelayMs: positive means the file's frames are released
	 * too early (the lights lead the sound). Other sounds playing on the PC spoil the measurement.
	 */
	class FHeardDelayProbe
	{
	public:
		bool Start(UStageAudioReactiveSubsystem& InAudio, double InSeconds)
		{
			Audio = &InAudio;
			Seconds = InSeconds;
			Tap = MakeShared<FStageAudioTap>();
			TSharedRef<std::atomic<int32>> OpenedState = OpenState;
			FText Error;
			if (!Loopback.Start(Tap.ToSharedRef(), [OpenedState](bool bOk, const FText&, const FString&, int32, int32) { OpenedState->store(bOk ? 1 : -1); }, Error))
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: system audio did not open: %s"), *Error.ToString());
				return false;
			}
			StartedAt = FPlatformTime::Seconds();
			return true;
		}

		~FHeardDelayProbe()
		{
			Loopback.Shutdown();
			Analyzer.Shutdown();
		}

		/** One game-thread frame; false when done. */
		bool Tick()
		{
			UStageAudioReactiveSubsystem* Live = Audio.Get();
			if (!Live || Live->GetSource() != EStageAudioSource::File || OpenState->load() < 0)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: stopped (the file stopped, or system audio failed)."));
				return false;
			}
			if (!bAnalyzing)
			{
				if (OpenState->load() == 0)
				{
					return true;
				}
				FStageAudioAnalyzerConfig Config;
				Config.Settings = Live->GetSettings();
				FText Error;
				if (!Analyzer.Start(Tap.ToSharedRef(), Config, Error))
				{
					UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: %s"), *Error.ToString());
					return false;
				}
				bAnalyzing = true;
			}
			const double Now = FPlatformTime::Seconds();
			const FStageAudioFeatures File = Live->GetFeatures();
			const FStageAudioFeatures Heard = Analyzer.GetFeatures(Now);
			if (File.bSignal && Heard.bSignal && File.Confidence >= 0.5f && Heard.Confidence >= 0.5f && File.BPM > 0.f)
			{
				double Gap = static_cast<double>(File.PhaseAt(Now)) - Heard.PhaseAt(Now);
				Gap -= FMath::FloorToDouble(Gap + 0.5);
				GapsMs.Add(Gap * 60000.0 / File.BPM);
			}
			if (Now - StartedAt < Seconds)
			{
				return true;
			}
			if (GapsMs.Num() < 60)
			{
				UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: too few beat-locked frames (%d); play a steady beat louder, with nothing else playing."), GapsMs.Num());
				return false;
			}
			GapsMs.Sort();
			const double Median = GapsMs[GapsMs.Num() / 2];
			const double Low = GapsMs[GapsMs.Num() / 10];
			const double High = GapsMs[GapsMs.Num() * 9 / 10];
			const float Current = Live->GetSettings().FileOutputDelayMs;
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: the file's beat leads what Windows plays by %.1f ms (median of %d frames, 10-90 %% %.1f .. %.1f ms); FileOutputDelayMs %.0f would be %.0f."),
				Median, GapsMs.Num(), Low, High, Current, Current + Median);
			return false;
		}

	private:
		TWeakObjectPtr<UStageAudioReactiveSubsystem> Audio;
		double Seconds = 10.0;
		double StartedAt = 0.0;
		TSharedPtr<FStageAudioTap> Tap;
		FStageLoopbackCapture Loopback;
		FStageAudioAnalyzer Analyzer;
		/** 0 opening, 1 open, -1 failed; written by the capture thread. */
		TSharedRef<std::atomic<int32>> OpenState = MakeShared<std::atomic<int32>>(0);
		bool bAnalyzing = false;
		TArray<double> GapsMs;
	};

	TUniquePtr<FHeardDelayProbe> ActiveProbe;
	FTSTicker::FDelegateHandle ProbeTicker;

	void MeasureDelay(const TArray<FString>& Args, UWorld* World)
	{
		UStageAudioReactiveSubsystem* Audio = World ? World->GetSubsystem<UStageAudioReactiveSubsystem>() : nullptr;
		if (!Audio || Audio->GetSource() != EStageAudioSource::File || Audio->GetState() != EStageAudioSourceState::Running || ActiveProbe)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: needs a music file playing (StageCraft.Music.Source File <path>), one measurement at a time."));
			return;
		}
		const double Seconds = Args.IsEmpty() ? 10.0 : FMath::Clamp(FCString::Atod(*Args[0]), 3.0, 120.0);
		ActiveProbe = MakeUnique<FHeardDelayProbe>();
		if (!ActiveProbe->Start(*Audio, Seconds))
		{
			ActiveProbe.Reset();
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.MeasureDelay: listening to system audio for %.0f s..."), Seconds);
		ProbeTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			if (ActiveProbe && ActiveProbe->Tick())
			{
				return true;
			}
			ActiveProbe.Reset();
			ProbeTicker.Reset();
			return false;
		}));
	}

	FAutoConsoleCommandWithWorldAndArgs MeasureDelayCommand(TEXT("StageCraft.Music.MeasureDelay"),
		TEXT("[seconds=10] While a music file plays, compares its beat with what Windows outputs (loopback) and logs the file delay it should have."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MeasureDelay));

	FAutoConsoleCommandWithWorldAndArgs SourceCommand(TEXT("StageCraft.Music.Source"), TEXT("Off | Input | SystemAudio | TestSignal [BPM=<n>] | File <path>: the audio-reactive source."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Source));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Music.Status"), TEXT("Logs the audio source, the analysis (BPM, beat, bands) and the analyser's cost."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs TransportCommand(TEXT("StageCraft.Music.Transport"), TEXT("Play | Pause | Stop | Seek <seconds> | Loop On|Off: the music file's transport."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Transport));
	FAutoConsoleCommandWithWorldAndArgs SettingsCommand(TEXT("StageCraft.Music.Settings"), TEXT("[FileDelay=<ms>] [FileOffset|InputOffset|SystemOffset=<ms>] [Gain=<dB>] [Device=<id>|Default] [Loop=On|Off] [Background=On|Off] [FlashLimit=<Hz>]: logs or changes the audio settings (saved on this PC)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Settings));
	FAutoConsoleCommandWithWorldAndArgs MasterCommand(TEXT("StageCraft.Music.Master"), TEXT("[0..1]: logs or sets the AUDIO master of effects that follow the music."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Master));
	FAutoConsoleCommandWithWorldAndArgs KillCommand(TEXT("StageCraft.Music.Kill"), TEXT("[On|Off]: logs or sets AUDIO KILL (effects that follow the music run as without it)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Kill));
	FAutoConsoleCommandWithWorldAndArgs FollowCommand(TEXT("StageCraft.Music.Follow"), TEXT("FOLLOW MUSIC on the selected fixtures (bass intensity, mids colour, highs strobe; one undo step)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Follow));
	FAutoConsoleCommandWithWorldAndArgs DevicesCommand(TEXT("StageCraft.Music.Devices"), TEXT("Lists the recording devices the Input source can use."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Devices));
}

#endif // !UE_BUILD_SHIPPING
