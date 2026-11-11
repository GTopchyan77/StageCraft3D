// Copyright Epic Games, Inc. All Rights Reserved.

// MIDI and OSC remote control commands for scripted runs and diagnosis (Docs/ADR/0035). Settings and learn go through the player's
// requests; Send and SendMidi feed the subsystem as a device would. Development only: compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ModularSceneBuilder.h"
#include "Player/ModularPlayerController.h"
#include "Remote/StageRemoteControlSubsystem.h"
#include "Remote/StageRemoteMapping.h"

namespace StageRemoteConsoleCommands
{
	AModularPlayerController* FindController(UWorld* World)
	{
		return World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	UStageRemoteControlSubsystem* FindRemote(UWorld* World)
	{
		UStageRemoteControlSubsystem* Remote = UWorld::GetSubsystem<UStageRemoteControlSubsystem>(World);
		if (!Remote)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Remote: no stage in this world."));
		}
		return Remote;
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		const UStageRemoteControlSubsystem* Remote = FindRemote(World);
		if (!Remote)
		{
			return;
		}
		const FStageRemoteStatus& State = Remote->GetStatus();
		const FStageRemoteSettings& Settings = Remote->GetSettings();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Remote.Status: OSC %s (%s, feedback port %d, feedback to %s); MIDI %s; map %s (%d bindings); learning %s"),
			State.bOscListening ? TEXT("on") : TEXT("off"), *State.OscEndpoint, Settings.OscFeedbackPort,
			Settings.OscFeedbackPort > 0 ? TEXT("last sender") : TEXT("nobody"),
			State.bMidiOpen ? *State.MidiDevice : TEXT("off"), Settings.bCustomMidiMap ? TEXT("custom") : TEXT("default"),
			StageRemote::EffectiveMidiBindings(Settings).Num(), *UEnum::GetValueAsString(State.Learning));
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Remote.Status: received %lld, applied %lld, refused %lld, ignored %lld, dropped %lld; last input \"%s\"; problem \"%s\"; last refusal \"%s\""),
			State.Received, State.Applied, State.Refused, State.Ignored, State.Dropped, *State.LastInput, *State.LastProblem.ToString(), *State.LastRefusal.ToString());
		if (Args.Contains(TEXT("Map")))
		{
			for (const FStageMidiBinding& Binding : StageRemote::EffectiveMidiBindings(Settings))
			{
				UE_LOG(LogStageCraft, Display, TEXT("  %s %d ch %d -> %s"), Binding.Trigger == EStageMidiTrigger::Note ? TEXT("note") : TEXT("CC"),
					Binding.Number, Binding.Channel, *StageRemote::DescribeAction(Binding.Action, Binding.Playback).ToString());
			}
		}
		if (Args.Contains(TEXT("Devices")))
		{
			for (const FString& Name : UStageRemoteControlSubsystem::FindMidiInputs())
			{
				UE_LOG(LogStageCraft, Display, TEXT("  MIDI input: %s"), *Name);
			}
		}
	}

	/** Off|Local|Network [Port=n] [Feedback=n] [Midi=name|Off] [Map=Default] */
	void Settings(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = FindController(World);
		const UStageRemoteControlSubsystem* Remote = FindRemote(World);
		if (!Controller || !Remote)
		{
			return;
		}
		FStageRemoteSettings Settings = Remote->GetSettings();
		for (const FString& Arg : Args)
		{
			FString Key;
			FString Value;
			if (!Arg.Split(TEXT("="), &Key, &Value))
			{
				Key = Arg;
			}
			if (Key.Equals(TEXT("Off"), ESearchCase::IgnoreCase)) { Settings.OscScope = EStageOscScope::Off; }
			else if (Key.Equals(TEXT("Local"), ESearchCase::IgnoreCase)) { Settings.OscScope = EStageOscScope::ThisComputer; }
			else if (Key.Equals(TEXT("Network"), ESearchCase::IgnoreCase)) { Settings.OscScope = EStageOscScope::Network; }
			else if (Key.Equals(TEXT("Port"), ESearchCase::IgnoreCase)) { Settings.OscPort = FCString::Atoi(*Value); }
			else if (Key.Equals(TEXT("Feedback"), ESearchCase::IgnoreCase)) { Settings.OscFeedbackPort = FCString::Atoi(*Value); }
			else if (Key.Equals(TEXT("Midi"), ESearchCase::IgnoreCase)) { Settings.MidiInputName = Value.Equals(TEXT("Off"), ESearchCase::IgnoreCase) ? FString() : Value; }
			else if (Key.Equals(TEXT("Map"), ESearchCase::IgnoreCase) && Value.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
			{
				Settings.MidiBindings.Reset();
				Settings.bCustomMidiMap = false;
			}
			else
			{
				UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Remote.Settings: unknown argument %s."), *Arg);
				return;
			}
		}
		const FStageEconomyResultInfo Result = Controller->RequestSetRemoteControl(Settings);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Remote.Settings: %s"), Result.IsSuccess() ? TEXT("applied") : *Result.Message.ToString());
		Status({}, World);
	}

	/** <Action> [Playback] [Cue] | Cancel */
	void Learn(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = FindController(World);
		if (!Controller || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Remote.Learn <Action> [Playback 1-10] [Cue] | Cancel"));
			return;
		}
		EStageRemoteAction Action = EStageRemoteAction::None;
		if (!Args[0].Equals(TEXT("Cancel"), ESearchCase::IgnoreCase))
		{
			const int64 Value = StaticEnum<EStageRemoteAction>()->GetValueByNameString(Args[0]);
			if (Value == INDEX_NONE)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Remote.Learn: unknown action %s."), *Args[0]);
				return;
			}
			Action = static_cast<EStageRemoteAction>(Value);
		}
		const int32 Playback = Args.IsValidIndex(1) ? FCString::Atoi(*Args[1]) : 1;
		const float Cue = Args.IsValidIndex(2) ? FCString::Atof(*Args[2]) : 1.f;
		const FStageEconomyResultInfo Result = Controller->RequestMidiLearn(Action, Playback, Cue);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Remote.Learn: %s"), Result.IsSuccess() ? TEXT("waiting for a control") : *Result.Message.ToString());
	}

	/** <address> [value] [Now]: as if an OSC message arrived (no sender, so no feedback). Now carries it out at once. */
	void Send(const TArray<FString>& Args, UWorld* World)
	{
		UStageRemoteControlSubsystem* Remote = FindRemote(World);
		if (!Remote || Args.IsEmpty())
		{
			return;
		}
		TOptional<float> Value;
		if (Args.IsValidIndex(1) && !Args[1].Equals(TEXT("Now"), ESearchCase::IgnoreCase))
		{
			Value = FCString::Atof(*Args[1]);
		}
		Remote->ReceiveOsc(Args[0], Value, FString());
		if (Args.Contains(TEXT("Now")))
		{
			Remote->FlushCommands();
		}
	}

	/** <type hex: 8 9 B> <channel> <data1> <data2>: as if the MIDI input reported the event. */
	void SendMidi(const TArray<FString>& Args, UWorld* World)
	{
		UStageRemoteControlSubsystem* Remote = FindRemote(World);
		if (!Remote || Args.Num() < 4)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Remote.SendMidi <type 8|9|B> <channel 1-16> <data1> <data2>"));
			return;
		}
		Remote->ReceiveMidi(FParse::HexNumber(*Args[0]), FCString::Atoi(*Args[1]), FCString::Atoi(*Args[2]), FCString::Atoi(*Args[3]));
	}

	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Remote.Status"),
		TEXT("Logs the MIDI and OSC inputs, their counters and the last input. [Map] lists the MIDI map, [Devices] the MIDI inputs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs SettingsCommand(TEXT("StageCraft.Remote.Settings"),
		TEXT("Changes the remote settings through the player's request: Off|Local|Network [Port=n] [Feedback=n] [Midi=<name>|Off] [Map=Default]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Settings));
	FAutoConsoleCommandWithWorldAndArgs LearnCommand(TEXT("StageCraft.Remote.Learn"),
		TEXT("MIDI learn through the player's request: <Action> [Playback] [Cue] | Cancel. Actions: CueGo, CueBack, CueGoTo, PlaybackLevel, PlaybackFlash, PlaybackGo, PlaybackRelease, GrandMaster, Blackout, AudioMaster."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Learn));
	FAutoConsoleCommandWithWorldAndArgs SendCommand(TEXT("StageCraft.Remote.Send"),
		TEXT("Feeds an OSC message to the remote input as if it arrived: <address> [value] [Now]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Send));
	FAutoConsoleCommandWithWorldAndArgs SendMidiCommand(TEXT("StageCraft.Remote.SendMidi"),
		TEXT("Feeds a MIDI event to the remote input as if the device sent it: <type 8|9|B> <channel> <data1> <data2>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SendMidi));
}

#endif // !UE_BUILD_SHIPPING
