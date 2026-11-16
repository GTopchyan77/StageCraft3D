// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Remote/StageRemoteControlTypes.h"

/**
 * The pure half of MIDI and OSC remote control (Docs/ADR/0035): turns untrusted input into FStageRemoteCommands, sanitizes the saved
 * settings, and queues commands for the frame. No engine state, no UObjects; any thread, though the subsystem only uses it on the
 * game thread, where both engine plugins deliver.
 */
namespace StageRemote
{
	/** At most this many commands are carried out per frame; the rest are dropped and counted (a flooding sender). */
	inline constexpr int32 MaxCommandsPerFrame = 256;
	inline constexpr int32 MaxOscAddressLength = 128;
	inline constexpr int32 MinPort = 1024;
	inline constexpr int32 MaxPort = 65535;
	inline constexpr int32 DefaultOscPort = 8000;
	inline constexpr int32 DefaultFeedbackPort = 9000;
	inline constexpr int32 MaxMidiBindings = 512;
	inline constexpr int32 MaxDeviceNameLength = 128;

	/** After the hand last moved a fader or knob, MIDI feedback leaves it alone this long, then settles it on the desk's value. */
	inline constexpr double MidiHoldSeconds = 0.5;
	/** An input equal to a message sent this recently is its echo (an output looped into the input) and is dropped. */
	inline constexpr double MidiEchoSeconds = 0.1;
	/** Sent messages kept for echo checks; a full map is far fewer per frame. */
	inline constexpr int32 MaxRecentSent = 1024;

	/** MIDI status nibbles the resolver reads (the MIDIDevice raw event's Type). */
	inline constexpr int32 MidiNoteOff = 0x8;
	inline constexpr int32 MidiNoteOn = 0x9;
	inline constexpr int32 MidiControlChange = 0xB;

	/** Levels and masters: only the latest value of a frame matters, so the queue keeps one per target. */
	bool IsContinuous(EStageRemoteAction Action);

	/** GO, BACK, go to cue, playback GO and release: fire on a press, ignore the release. */
	bool IsTrigger(EStageRemoteAction Action);

	/** The actions that need a playback. */
	bool UsesPlayback(EStageRemoteAction Action);

	/**
	 * An OSC message (Docs/ADR/0035 §3). Value is the first argument as a number, unset when the message has none. Returns false
	 * (the message is ignored) for an unknown or over-long address, a playback outside 1…StagePlaybackRules::Count, a missing or
	 * non-finite value where one is needed, and a trigger's release (value < 0.5).
	 */
	bool ParseOsc(FStringView Address, TOptional<float> Value, FStageRemoteCommand& OutCommand);

	/**
	 * A MIDI event as the MIDIDevice plugin reports it raw: Type is the status nibble (0x8 Note Off, 0x9 Note On, 0xB Control Change;
	 * others are ignored), Channel 1–16, Data1 the note or controller, Data2 the velocity or value. Note On with velocity 0 is a Note Off.
	 * Appends one command per matching binding (a trigger's release adds none). Returns how many were appended.
	 */
	int32 ResolveMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2, TConstArrayView<FStageMidiBinding> Bindings,
		TArray<FStageRemoteCommand>& OutCommands);

	/**
	 * MIDI learn: if the event is a press that can be bound (Note On with velocity, or a CC above 0), fills InOutBinding's trigger,
	 * channel and number from it and returns true. The action and its target are the caller's.
	 */
	bool LearnFromMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2, FStageMidiBinding& InOutBinding);

	/** "MIDI CC 7 ch 1 = 64", "MIDI note 60 ch 10 on 100": for the status line. */
	FString DescribeMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2);

	/** The OSC address of an action, as §3 lists it ("/stagecraft/playback/3/level" for playback index 2). Empty for None. */
	FString OscAddressFor(EStageRemoteAction Action, int32 PlaybackIndex);

	/** "Playback 3 level", "Cue GO": for logs and the learn prompt. */
	FText DescribeAction(EStageRemoteAction Action, int32 PlaybackNumber);

	/** The Korg nanoKONTROL2 factory CC layout (Docs/ADR/0035 §5). */
	const TArray<FStageMidiBinding>& DefaultMidiBindings();

	/** The map in force: the user's when they changed it, the default otherwise. */
	TConstArrayView<FStageMidiBinding> EffectiveMidiBindings(const FStageRemoteSettings& Settings);

	/**
	 * The two bindings can answer the same MIDI event: same trigger and number, and the same channel or either on any channel (0).
	 * Learning or a later binding replaces an overlapping one, so one control never drives two rows by accident.
	 */
	bool OverlapsTrigger(const FStageMidiBinding& A, const FStageMidiBinding& B);

	/** A binding that can be carried out: a real action, channel 0–16, number 0–127, playback 1–Count, a finite cue number. */
	bool IsValidBinding(const FStageMidiBinding& Binding);

	/**
	 * Repairs settings from an untrusted source (the ini): an unknown scope is Off, a port outside 1024–65535 the default, a feedback
	 * port outside that range (and not 0) the default, the device name trimmed and cut, invalid bindings dropped, a later binding of
	 * an overlapping trigger (OverlapsTrigger) replacing an earlier one, at most MaxMidiBindings. Returns true if anything changed.
	 */
	bool Sanitize(FStageRemoteSettings& Settings);

	// --- The mapping editor (Preferences › REMOTE CONTROL › MIDI MAP, Docs/ADR/0035 §11) ---

	/**
	 * A target of the mapping editor: Action, and for playback actions its playback as the desk numbers it (1–10). Go to cue is matched
	 * on its cue number too, so two learned "go to cue" buttons for different cues are two targets.
	 */
	bool IsSameTarget(const FStageMidiBinding& Binding, EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber = 0.f);

	/** The bindings that drive a target, in map order. */
	TArray<FStageMidiBinding> BindingsForTarget(TConstArrayView<FStageMidiBinding> Bindings, EStageRemoteAction Action, int32 PlaybackNumber,
		float CueNumber = 0.f);

	/** "CC 41", "CC 7 ch 2", "Note 60 ch 10": the control of a binding, for the mapping editor. */
	FString DescribeControl(const FStageMidiBinding& Binding);

	/**
	 * MIDI learn finished: the map in force becomes the user's, the target's earlier controls and any binding of the same control are
	 * removed (an "any channel" binding of it too), and Learned is added. One control per target is what LEARN means in the editor (CLEAR, then nothing drives it). Returns
	 * false, changing nothing, for an invalid binding.
	 */
	bool AssignLearned(FStageRemoteSettings& Settings, const FStageMidiBinding& Learned);

	/** CLEAR in the mapping editor: the target's controls leave the map in force, which becomes the user's. True if any did. */
	bool ClearTarget(FStageRemoteSettings& Settings, EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber = 0.f);

	/** OSC, the MIDI input or the MIDI output is asked for. */
	bool HasInputOn(const FStageRemoteSettings& Settings);

	/** Same OSC scope and ports and the same MIDI devices: a change between the two is a map edit, and nothing has to reopen. */
	bool HasSameInputs(const FStageRemoteSettings& A, const FStageRemoteSettings& B);

	/**
	 * Requested needs the GameMode's verdict (EvaluateRemoteControl): it asks for an input and is not a map edit of Current. Switching
	 * everything off never needs one; editing the map neither, because every command a control sends is judged when it runs. Applying
	 * the same settings again (to retry a device that was missing) is judged like switching on.
	 */
	bool NeedsVerdict(const FStageRemoteSettings& Current, const FStageRemoteSettings& Requested);

	// --- MIDI feedback (motor faders and LEDs, Docs/ADR/0035 §12) ---

	/** The show's state as MIDI feedback reflects it. Playback arrays are indexed 0…Count-1. */
	struct FMidiShowState
	{
		TArray<float> Levels;
		TArray<bool> Flashing;
		TArray<bool> Active;
		float GrandMaster = 1.f;
		bool bBlackout = false;
		float AudioMaster = 1.f;
	};

	/** One MIDI channel message to send: Type is the status nibble (MidiNoteOn or MidiControlChange), Channel 1–16, data 0–127. */
	struct FMidiOutMessage
	{
		int32 Type = MidiControlChange;
		int32 Channel = 1;
		int32 Data1 = 0;
		int32 Data2 = 0;

		bool operator==(const FMidiOutMessage& Other) const
		{
			return Type == Other.Type && Channel == Other.Channel && Data1 == Other.Data1 && Data2 == Other.Data2;
		}
	};

	/**
	 * The value (0–127) a bound control should show: a fader its level, flash and playback GO their LED (lit while flashing / while the
	 * playback is live), Blackout its LED, the masters their level. Unset for controls with no state (cue GO and BACK, go to cue,
	 * release) and for an invalid binding.
	 */
	TOptional<int32> FeedbackValue(const FStageMidiBinding& Binding, const FMidiShowState& State);

	/**
	 * A MIDI input event at NowSeconds: a fader or knob already shows the value it sent, so it is remembered as shown and held for
	 * MidiHoldSeconds (a motor fader would otherwise fight the hand moving it). Buttons are not remembered: their LED is the show's to
	 * light. Notes the channel. Returns true when a fader or knob moved (the caller settles it once the hold ends).
	 */
	bool NoteMidiInput(int32 Type, int32 Channel, int32 Data1, int32 Data2, TConstArrayView<FStageMidiBinding> Bindings, FStageMidiFeedbackMemory& Memory,
		double NowSeconds);

	/** The input event equals a message sent within MidiEchoSeconds: the output's echo, not a hand. Forgets older sent messages. */
	bool IsMidiEcho(int32 Type, int32 Channel, int32 Data1, int32 Data2, FStageMidiFeedbackMemory& Memory, double NowSeconds);

	/**
	 * Appends a message for every bound control whose value differs from what Memory says it shows, remembers it as shown and as sent.
	 * A control bound twice answers for its first binding only, so two actions never make it flicker. A fader or knob still held by the
	 * hand is skipped; OutHoldUntil is then the time the last hold ends (0 when none), for the caller to collect again. Returns how many
	 * were appended.
	 */
	int32 CollectMidiFeedback(TConstArrayView<FStageMidiBinding> Bindings, const FMidiShowState& State, FStageMidiFeedbackMemory& Memory,
		double NowSeconds, TArray<FMidiOutMessage>& OutMessages, double& OutHoldUntil);

	/**
	 * Before the map changes: a control Memory shows lit or raised that no binding of Bindings answers any more is sent 0, so a cleared
	 * Blackout button does not stay lit. Returns how many were appended.
	 */
	int32 CollectUnboundOff(TConstArrayView<FStageMidiBinding> Bindings, FStageMidiFeedbackMemory& Memory, double NowSeconds,
		TArray<FMidiOutMessage>& OutMessages);

	/**
	 * Adds Command to one frame's queue. A continuous command replaces the queued one with the same action, source and playback (the latest
	 * value wins, in its original place); everything else keeps its order. Returns false, and drops it, when the queue already holds
	 * MaxCommandsPerFrame.
	 */
	bool Enqueue(TArray<FStageRemoteCommand>& Pending, const FStageRemoteCommand& Command);
}
