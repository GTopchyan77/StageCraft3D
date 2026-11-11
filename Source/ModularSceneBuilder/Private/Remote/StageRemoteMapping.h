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

	/** A binding that can be carried out: a real action, channel 0–16, number 0–127, playback 1–Count, a finite cue number. */
	bool IsValidBinding(const FStageMidiBinding& Binding);

	/**
	 * Repairs settings from an untrusted source (the ini): an unknown scope is Off, a port outside 1024–65535 the default, a feedback
	 * port outside that range (and not 0) the default, the device name trimmed and cut, invalid bindings dropped, a later binding of
	 * the same trigger replacing an earlier one, at most MaxMidiBindings. Returns true if anything changed.
	 */
	bool Sanitize(FStageRemoteSettings& Settings);

	/**
	 * Adds Command to one frame's queue. A continuous command replaces the queued one with the same action and playback (the latest
	 * value wins, in its original place); everything else keeps its order. Returns false, and drops it, when the queue already holds
	 * MaxCommandsPerFrame.
	 */
	bool Enqueue(TArray<FStageRemoteCommand>& Pending, const FStageRemoteCommand& Command);
}
