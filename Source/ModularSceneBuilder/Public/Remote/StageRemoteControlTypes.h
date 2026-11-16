// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StageRemoteControlTypes.generated.h"

/**
 * What a MIDI or OSC remote can do (Docs/ADR/0035 §3): the desk's live controls, nothing that edits the show. Each one runs through
 * the player controller's request bridge, so the GameMode judges it like a click on the desk.
 */
UENUM()
enum class EStageRemoteAction : uint8
{
	None UMETA(Hidden),
	CueGo,
	CueBack,
	/** Go to CueNumber. */
	CueGoTo,
	/** Playback fader level, 0…1. */
	PlaybackLevel,
	/** Flash while the value is down (>= 0.5). */
	PlaybackFlash,
	PlaybackGo,
	PlaybackRelease,
	/** 0…1. */
	GrandMaster,
	/** On while the value is down (>= 0.5). */
	Blackout,
	/** 0…1. */
	AudioMaster,
};

/** Where the OSC input listens (Docs/ADR/0035 §4). Off by default; Network must be chosen explicitly. */
UENUM()
enum class EStageOscScope : uint8
{
	Off,
	/** 127.0.0.1 only: an OSC app on this PC. */
	ThisComputer,
	/** Every network card: a tablet on the LAN. OSC has no authentication. */
	Network,
};

/** The MIDI message a binding answers. */
UENUM()
enum class EStageMidiTrigger : uint8
{
	/** Note On / Note Off: the value is velocity / 127 while held, 0 on release. */
	Note,
	/** Control Change: the value is data / 127. */
	ControlChange,
};

/** One MIDI control mapped to an action (Docs/ADR/0035 §5). Saved per PC in the remote settings. */
USTRUCT()
struct FStageMidiBinding
{
	GENERATED_BODY()

	UPROPERTY(Config)
	EStageMidiTrigger Trigger = EStageMidiTrigger::ControlChange;

	/** 1–16, or 0 for any channel. */
	UPROPERTY(Config)
	int32 Channel = 0;

	/** Note or controller number, 0–127. */
	UPROPERTY(Config)
	int32 Number = 0;

	UPROPERTY(Config)
	EStageRemoteAction Action = EStageRemoteAction::None;

	/** The playback as the desk numbers it, 1–10 (playback actions only). */
	UPROPERTY(Config)
	int32 Playback = 1;

	/** The cue to go to (CueGoTo only). */
	UPROPERTY(Config)
	float CueNumber = 1.f;

	/** Same MIDI control (trigger, channel, number): a newer binding for it replaces the older one. */
	bool HasSameTrigger(const FStageMidiBinding& Other) const
	{
		return Trigger == Other.Trigger && Channel == Other.Channel && Number == Other.Number;
	}

	bool operator==(const FStageMidiBinding& Other) const
	{
		return HasSameTrigger(Other) && Action == Other.Action && Playback == Other.Playback && CueNumber == Other.CueNumber;
	}
};

/**
 * MIDI and OSC remote control of this PC (Docs/ADR/0035 §4), saved in GameUserSettings. UStageRemoteControlSubsystem is the only writer;
 * StageRemote::Sanitize runs on load and on every set, so a hand-edited ini cannot open a privileged port or bind nonsense.
 */
USTRUCT()
struct FStageRemoteSettings
{
	GENERATED_BODY()

	UPROPERTY(Config)
	EStageOscScope OscScope = EStageOscScope::Off;

	/** The UDP port OSC listens on, 1024–65535. */
	UPROPERTY(Config)
	int32 OscPort = 8000;

	/** The show's state goes back to the last sender on this port (TouchOSC's default receive port); 0 = no feedback. */
	UPROPERTY(Config)
	int32 OscFeedbackPort = 9000;

	/** The MIDI input device, by name; empty = MIDI off. */
	UPROPERTY(Config)
	FString MidiInputName;

	/**
	 * The MIDI output device that receives the show's state (motor faders, LEDs; Docs/ADR/0035 §12), by name; empty = no MIDI feedback.
	 * Usually the same controller as the input, which lists itself once as an input and once as an output.
	 */
	UPROPERTY(Config)
	FString MidiOutputName;

	/** The MIDI map. Empty while bCustomMidiMap is false means the default map (StageRemote::DefaultMidiBindings). */
	UPROPERTY(Config)
	TArray<FStageMidiBinding> MidiBindings;

	/** The user changed the map (learn): MidiBindings is used as it is, even empty. */
	UPROPERTY(Config)
	bool bCustomMidiMap = false;

	bool operator==(const FStageRemoteSettings& Other) const
	{
		return OscScope == Other.OscScope && OscPort == Other.OscPort && OscFeedbackPort == Other.OscFeedbackPort
			&& MidiInputName == Other.MidiInputName && MidiOutputName == Other.MidiOutputName && MidiBindings == Other.MidiBindings
			&& bCustomMidiMap == Other.bCustomMidiMap;
	}
};

/** Which remote a command came from: the GameMode judges each kind with its own rule (Docs/ADR/0036 §2). */
enum class EStageRemoteSource : uint8
{
	/** MIDI or OSC (EvaluateRemoteControl). */
	Controller,
	/** A Web Remote page or HTTP client (EvaluateWebRemote). */
	Web,
};

/** One thing a remote asked for, resolved from an OSC message, a MIDI event or a Web Remote command. Plain data, any thread. */
struct FStageRemoteCommand
{
	EStageRemoteAction Action = EStageRemoteAction::None;
	EStageRemoteSource Source = EStageRemoteSource::Controller;
	/** 0-based playback index (playback actions). */
	int32 Playback = 0;
	/** Level 0…1, or a press value (>= 0.5 is down). */
	float Value = 1.f;
	/** CueGoTo only. */
	float CueNumber = 0.f;

	bool IsDown() const { return Value >= 0.5f; }

	bool operator==(const FStageRemoteCommand& Other) const
	{
		return Action == Other.Action && Source == Other.Source && Playback == Other.Playback && Value == Other.Value && CueNumber == Other.CueNumber;
	}
};

/**
 * What a MIDI controller shows now, as far as MIDI feedback knows (StageRemote::CollectMidiFeedback), so only changes are sent. Reset()
 * when the output opens or the map changes: every bound control is then sent once. HeardChannel keeps the channel each control was last
 * heard on, so an "any channel" binding answers on it. Keys are StageRemote's (trigger, channel, number). Plain data, game thread.
 */
struct FStageMidiFeedbackMemory
{
	TMap<uint32, int32> Shown;
	TMap<uint32, int32> HeardChannel;

	/**
	 * When the hand last moved each fader or knob (the caller's clock, seconds). Feedback leaves it alone for StageRemote::MidiHoldSeconds:
	 * the desk applies a move a frame later, so answering during a move would send the hand an older value.
	 */
	TMap<uint32, double> LastMoved;

	/** A message sent to the output, kept for StageRemote::MidiEchoSeconds so its echo (an output looped into the input) is dropped. */
	struct FSent
	{
		uint32 Key = 0;
		int32 Value = 0;
		double Time = 0.0;
	};
	TArray<FSent> RecentSent;

	/** Everything is sent again on the next feedback; the hand's holds and the channels heard stay. */
	void Reset() { Shown.Reset(); }
};

/** What the remote inputs are doing, for the Preferences panel and the status command. */
struct FStageRemoteStatus
{
	/** "port 8000, this PC only" / "port 8000, every network", or empty while OSC is off. */
	FString OscEndpoint;
	bool bOscListening = false;
	/** The open MIDI input device; empty while MIDI is off or the device is missing. */
	FString MidiDevice;
	bool bMidiOpen = false;
	/** The open MIDI output device (feedback); empty while it is off or missing. */
	FString MidiOutputDevice;
	bool bMidiOutputOpen = false;
	/** The target MIDI learn waits to bind, None when not learning; LearningPlayback is its playback (1–10) for playback actions. */
	EStageRemoteAction Learning = EStageRemoteAction::None;
	int32 LearningPlayback = 0;

	/** Messages and events received (OSC messages, MIDI channel events). */
	int64 Received = 0;
	/** Commands the show carried out. */
	int64 Applied = 0;
	/** Commands the GameMode or the show refused. */
	int64 Refused = 0;
	/** Inputs that matched no action (unknown address, unmapped control, bad argument). */
	int64 Ignored = 0;
	/** Commands over the per-frame limit. */
	int64 Dropped = 0;
	/** MIDI feedback messages sent to the output device. */
	int64 MidiSent = 0;

	/** "OSC /stagecraft/playback/1/level 0.50", "MIDI CC 0 ch 1 = 64": the last input, for checking a controller. */
	FString LastInput;
	/** Why OSC or MIDI could not open (port in use, device missing); empty when both opened as asked. Shown as a warning. */
	FText LastProblem;
	/** Why the last refused command was refused (the session's rules, nothing to do). Information, not a fault. */
	FText LastRefusal;
};

DECLARE_MULTICAST_DELEGATE(FOnStageRemoteStatusChanged);
