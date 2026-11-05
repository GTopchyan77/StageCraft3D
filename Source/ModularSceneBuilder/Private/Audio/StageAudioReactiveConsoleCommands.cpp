// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for audio-reactive lighting (Docs/ADR/0029 §5): choose the source, drive the file transport, change
// the settings and read the analysis, through AModularPlayerController's audio requests (the path the Audio panel will use).
// For scripted runs before the G2 Audio panel exists. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Audio/StageAudioCaptureSources.h"
#include "Audio/StageAudioSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
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

	/** [FileDelay=<ms>] [FileOffset|InputOffset|SystemOffset=<ms>] [Gain=<dB>] [Device=<id>|Default] [Loop=On|Off] [Background=On|Off] */
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
		}
		if (bChanged)
		{
			Controller->RequestApplyAudioSettings(Settings);
		}
		const FStageAudioReactiveSettings Now = Audio->GetSettings();
		const UStageAudioSubsystem* Sound = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UStageAudioSubsystem>() : nullptr;
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Settings: file delay %.0f ms, offsets file %.0f / input %.0f / system %.0f ms, input gain %.1f dB, device \"%s\", loop %s, bands %.0f/%.0f/%.0f/%.0f Hz, background %s"),
			Now.FileOutputDelayMs, Now.FileLatencyOffsetMs, Now.InputLatencyOffsetMs, Now.SystemAudioLatencyOffsetMs, Now.InputGainDb, *Now.InputDeviceId,
			Now.bLoopFile ? TEXT("on") : TEXT("off"), Now.SubBassTopHz, Now.BassTopHz, Now.MidTopHz, Now.HighTopHz,
			Sound && Sound->IsPlayedInBackground() ? TEXT("on") : TEXT("off"));
	}

	void Devices(const TArray<FString>& Args, UWorld* World)
	{
		for (const TPair<FString, FString>& Device : FStageAudioInputCapture::ListDevices())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Music.Devices: \"%s\" id=%s"), *Device.Key, *Device.Value);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs SourceCommand(TEXT("StageCraft.Music.Source"), TEXT("Off | Input | SystemAudio | TestSignal [BPM=<n>] | File <path>: the audio-reactive source."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Source));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Music.Status"), TEXT("Logs the audio source, the analysis (BPM, beat, bands) and the analyser's cost."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs TransportCommand(TEXT("StageCraft.Music.Transport"), TEXT("Play | Pause | Stop | Seek <seconds> | Loop On|Off: the music file's transport."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Transport));
	FAutoConsoleCommandWithWorldAndArgs SettingsCommand(TEXT("StageCraft.Music.Settings"), TEXT("[FileDelay=<ms>] [FileOffset|InputOffset|SystemOffset=<ms>] [Gain=<dB>] [Device=<id>|Default] [Loop=On|Off] [Background=On|Off]: logs or changes the audio settings (saved on this PC)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Settings));
	FAutoConsoleCommandWithWorldAndArgs DevicesCommand(TEXT("StageCraft.Music.Devices"), TEXT("Lists the recording devices the Input source can use."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Devices));
}

#endif // !UE_BUILD_SHIPPING
