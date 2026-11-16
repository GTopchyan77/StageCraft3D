// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for MIDI and OSC remote control (Docs/ADR/0035): the OSC address space, MIDI resolution and learn, the frame queue, settings
// sanitizing, the mapping editor's operations and MIDI feedback (§11, §12), the subsystem with a test command handler, a real MIDI output,
// the GameMode rule, and a real OSC round trip on loopback (server, feedback).
// The controller's request path is exercised by the scripted -game run (STATE.md #89).
// Run: Scripts/Tests/RunAutomation.ps1 -Filter StageCraft.Remote

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <limits>
#include "UObject/StrongObjectPtr.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/StageCraftGameModeBase.h"
#include "Game/StageSessionTypes.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "MIDIDeviceManager.h"
#include "Misc/AutomationTest.h"
#include "OSCClient.h"
#include "OSCManager.h"
#include "OSCMessage.h"
#include "OSCServer.h"
#include "Remote/StageRemoteControlSubsystem.h"
#include "Remote/StageRemoteMapping.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/ShowControlTypes.h"
#include "SocketSubsystem.h"
#include "Subsystems/ShowControlSubsystem.h"

namespace StageRemoteControlTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	struct FScopedWorld
	{
		UWorld* World = nullptr;

		FScopedWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("StageRemoteTestWorld"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FScopedWorld()
		{
			World->BeginTearingDown();
			World->EndPlay(EEndPlayReason::Quit);
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			GEngine->DestroyWorldContext(World);
		}

		UStageRemoteControlSubsystem* GetRemote() const { return World->GetSubsystem<UStageRemoteControlSubsystem>(); }
	};

	/**
	 * Sets the remote settings in memory only and puts the user's back on destruction. ApplySettings saves only when the stored value
	 * changes, so applying the value already set never writes the user's GameUserSettings.ini.
	 */
	struct FScopedRemoteSettings
	{
		FStageRemoteSettings Original;
		UStageCraftUserSettings* UserSettings = nullptr;

		explicit FScopedRemoteSettings(const FStageRemoteSettings& Settings)
		{
			UserSettings = UStageCraftUserSettings::Get();
			if (UserSettings)
			{
				Original = UserSettings->GetRemoteSettings();
				UserSettings->SetRemoteSettings(Settings);
			}
		}

		~FScopedRemoteSettings()
		{
			if (UserSettings)
			{
				UserSettings->SetRemoteSettings(Original);
			}
		}
	};

	FStageRemoteCommand ParseOk(FAutomationTestBase& Test, const TCHAR* What, const TCHAR* Address, TOptional<float> Value)
	{
		FStageRemoteCommand Command;
		Test.TestTrue(What, StageRemote::ParseOsc(Address, Value, Command));
		return Command;
	}

	bool Refused(const TCHAR* Address, TOptional<float> Value)
	{
		FStageRemoteCommand Command;
		return !StageRemote::ParseOsc(Address, Value, Command);
	}

	TArray<FStageRemoteCommand> Resolve(int32 Type, int32 Channel, int32 Data1, int32 Data2, TConstArrayView<FStageMidiBinding> Bindings)
	{
		TArray<FStageRemoteCommand> Commands;
		StageRemote::ResolveMidi(Type, Channel, Data1, Data2, Bindings, Commands);
		return Commands;
	}

	FStageMidiBinding MakeBinding(EStageMidiTrigger Trigger, int32 Channel, int32 Number, EStageRemoteAction Action, int32 Playback = 1)
	{
		FStageMidiBinding Binding;
		Binding.Trigger = Trigger;
		Binding.Channel = Channel;
		Binding.Number = Number;
		Binding.Action = Action;
		Binding.Playback = Playback;
		return Binding;
	}

	FStageRemoteCommand MakeCommand(EStageRemoteAction Action, int32 Playback = 0, float Value = 1.f)
	{
		FStageRemoteCommand Command;
		Command.Action = Action;
		Command.Playback = Playback;
		Command.Value = Value;
		return Command;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteOscTest, "StageCraft.Remote.Osc", StageRemoteControlTests::Flags)
bool FStageRemoteOscTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;

	// Triggers: no argument fires, a press fires, a release is ignored; case does not matter.
	TestEqual(TEXT("GO without argument"), ParseOk(*this, TEXT("go"), TEXT("/stagecraft/cue/go"), {}).Action, EStageRemoteAction::CueGo);
	TestEqual(TEXT("GO pressed"), ParseOk(*this, TEXT("go 1"), TEXT("/stagecraft/cue/go"), 1.f).Action, EStageRemoteAction::CueGo);
	TestTrue(TEXT("GO released is ignored"), Refused(TEXT("/stagecraft/cue/go"), 0.f));
	TestEqual(TEXT("Case-insensitive"), ParseOk(*this, TEXT("GO caps"), TEXT("/StageCraft/Cue/GO"), {}).Action, EStageRemoteAction::CueGo);
	TestEqual(TEXT("BACK"), ParseOk(*this, TEXT("back"), TEXT("/stagecraft/cue/back"), {}).Action, EStageRemoteAction::CueBack);

	// Go to cue: the argument is the cue.
	const FStageRemoteCommand GoTo = ParseOk(*this, TEXT("goto"), TEXT("/stagecraft/cue/goto"), 12.5f);
	TestEqual(TEXT("Go to cue action"), GoTo.Action, EStageRemoteAction::CueGoTo);
	TestEqual(TEXT("Go to cue number"), GoTo.CueNumber, 12.5f);
	TestTrue(TEXT("Go to cue without a number is ignored"), Refused(TEXT("/stagecraft/cue/goto"), {}));

	// Playbacks: 1-based in the address, 0-based in the command.
	const FStageRemoteCommand Level = ParseOk(*this, TEXT("level"), TEXT("/stagecraft/playback/3/level"), 0.5f);
	TestEqual(TEXT("Level action"), Level.Action, EStageRemoteAction::PlaybackLevel);
	TestEqual(TEXT("Playback 3 is index 2"), Level.Playback, 2);
	TestEqual(TEXT("Level value"), Level.Value, 0.5f);
	TestEqual(TEXT("Playback 10"), ParseOk(*this, TEXT("pb10"), TEXT("/stagecraft/playback/10/go"), {}).Playback, 9);
	TestEqual(TEXT("Leading zero"), ParseOk(*this, TEXT("pb03"), TEXT("/stagecraft/playback/03/release"), {}).Playback, 2);
	TestTrue(TEXT("Level without a value is ignored"), Refused(TEXT("/stagecraft/playback/1/level"), {}));
	TestTrue(TEXT("NaN is ignored"), Refused(TEXT("/stagecraft/playback/1/level"), std::numeric_limits<float>::quiet_NaN()));
	TestTrue(TEXT("Infinity is ignored"), Refused(TEXT("/stagecraft/master/grand"), std::numeric_limits<float>::infinity()));
	TestTrue(TEXT("Playback 0"), Refused(TEXT("/stagecraft/playback/0/level"), 1.f));
	TestTrue(TEXT("Playback 11"), Refused(TEXT("/stagecraft/playback/11/level"), 1.f));
	TestTrue(TEXT("Playback 123"), Refused(TEXT("/stagecraft/playback/123/level"), 1.f));
	TestTrue(TEXT("Playback x"), Refused(TEXT("/stagecraft/playback/x/level"), 1.f));
	TestTrue(TEXT("Playback -1"), Refused(TEXT("/stagecraft/playback/-1/level"), 1.f));

	// Flash and Blackout follow the value: the release is a command too.
	TestTrue(TEXT("Flash down"), ParseOk(*this, TEXT("flash 1"), TEXT("/stagecraft/playback/1/flash"), 1.f).IsDown());
	TestFalse(TEXT("Flash up is a command"), ParseOk(*this, TEXT("flash 0"), TEXT("/stagecraft/playback/1/flash"), 0.f).IsDown());
	TestFalse(TEXT("Blackout off is a command"), ParseOk(*this, TEXT("blackout 0"), TEXT("/stagecraft/master/blackout"), 0.f).IsDown());
	TestEqual(TEXT("Grand Master"), ParseOk(*this, TEXT("grand"), TEXT("/stagecraft/master/grand"), 0.25f).Value, 0.25f);
	TestEqual(TEXT("Audio master"), ParseOk(*this, TEXT("audio"), TEXT("/stagecraft/master/audio"), 0.75f).Action, EStageRemoteAction::AudioMaster);

	// Not ours or malformed.
	for (const TCHAR* Address : { TEXT("/stagecraft/cue"), TEXT("/other/cue/go"), TEXT("stagecraft/cue/go"), TEXT("/stagecraft/cue/go/extra"),
		TEXT("/stagecraft/cue/go/"), TEXT("/stagecraft//go"), TEXT("/stagecraft/cue/stop"), TEXT("/stagecraft/playback/1"), TEXT(""), TEXT("/") })
	{
		TestTrue(FString::Printf(TEXT("Ignored: '%s'"), Address), Refused(Address, 1.f));
	}
	TestTrue(TEXT("Over-long address"), Refused(*(TEXT("/stagecraft/cue/go") + FString::ChrN(200, TEXT('x'))), 1.f));

	// Every action's own address parses back to it (feedback and docs use OscAddressFor).
	for (uint8 Raw = 1; Raw <= static_cast<uint8>(EStageRemoteAction::AudioMaster); ++Raw)
	{
		const EStageRemoteAction Action = static_cast<EStageRemoteAction>(Raw);
		FStageRemoteCommand Command;
		TestTrue(FString::Printf(TEXT("Round trip %d"), Raw), StageRemote::ParseOsc(StageRemote::OscAddressFor(Action, 4), 1.f, Command));
		TestEqual(FString::Printf(TEXT("Round trip action %d"), Raw), Command.Action, Action);
		if (StageRemote::UsesPlayback(Action))
		{
			TestEqual(FString::Printf(TEXT("Round trip playback %d"), Raw), Command.Playback, 4);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteMidiTest, "StageCraft.Remote.Midi", StageRemoteControlTests::Flags)
bool FStageRemoteMidiTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	using namespace StageRemote;
	const TConstArrayView<FStageMidiBinding> Defaults = DefaultMidiBindings();

	// The default map (nanoKONTROL2): every binding valid, no control bound twice.
	TestEqual(TEXT("Default map size"), Defaults.Num(), 37);
	for (int32 Index = 0; Index < Defaults.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("Default binding %d is valid"), Index), IsValidBinding(Defaults[Index]));
		for (int32 Other = Index + 1; Other < Defaults.Num(); ++Other)
		{
			TestFalse(FString::Printf(TEXT("Default bindings %d and %d share a control"), Index, Other), Defaults[Index].HasSameTrigger(Defaults[Other]));
		}
	}

	// Fader 1 (CC 0) → playback 1 level, on any channel.
	TArray<FStageRemoteCommand> Commands = Resolve(MidiControlChange, 1, 0, 64, Defaults);
	if (TestEqual(TEXT("Fader 1 gives one command"), Commands.Num(), 1))
	{
		TestEqual(TEXT("Fader 1 action"), Commands[0].Action, EStageRemoteAction::PlaybackLevel);
		TestEqual(TEXT("Fader 1 playback"), Commands[0].Playback, 0);
		TestEqual(TEXT("Fader 1 value"), Commands[0].Value, 64.f / 127.f);
	}
	TestEqual(TEXT("Fader 8 on channel 16"), Resolve(MidiControlChange, 16, 7, 127, Defaults)[0].Playback, 7);
	TestEqual(TEXT("Fader at 0 still moves"), Resolve(MidiControlChange, 1, 0, 0, Defaults)[0].Value, 0.f);

	// Buttons: ▶ is GO on press only; S is a flash that follows the button.
	TestEqual(TEXT("Play pressed"), Resolve(MidiControlChange, 1, 41, 127, Defaults)[0].Action, EStageRemoteAction::CueGo);
	TestEqual(TEXT("Play released does nothing"), Resolve(MidiControlChange, 1, 41, 0, Defaults).Num(), 0);
	TestTrue(TEXT("S1 down"), Resolve(MidiControlChange, 1, 32, 127, Defaults)[0].IsDown());
	TestFalse(TEXT("S1 up"), Resolve(MidiControlChange, 1, 32, 0, Defaults)[0].IsDown());
	TestEqual(TEXT("An unmapped CC"), Resolve(MidiControlChange, 1, 100, 127, Defaults).Num(), 0);

	// Notes on a channel: velocity is the press; Note On with velocity 0 is a release.
	const TArray<FStageMidiBinding> Notes = { MakeBinding(EStageMidiTrigger::Note, 10, 60, EStageRemoteAction::CueGo),
		MakeBinding(EStageMidiTrigger::Note, 0, 62, EStageRemoteAction::PlaybackLevel, 2), MakeBinding(EStageMidiTrigger::Note, 0, 62, EStageRemoteAction::Blackout) };
	TestEqual(TEXT("Note 60 ch 10 soft"), Resolve(MidiNoteOn, 10, 60, 20, Notes).Num(), 1);
	TestEqual(TEXT("Note 60 on another channel"), Resolve(MidiNoteOn, 1, 60, 100, Notes).Num(), 0);
	TestEqual(TEXT("Note On velocity 0 is a release"), Resolve(MidiNoteOn, 10, 60, 0, Notes).Num(), 0);
	TestEqual(TEXT("Note Off"), Resolve(MidiNoteOff, 10, 60, 64, Notes).Num(), 0);
	TestEqual(TEXT("A CC does not match a note binding"), Resolve(MidiControlChange, 10, 60, 127, Notes).Num(), 0);
	Commands = Resolve(MidiNoteOn, 3, 62, 127, Notes);
	if (TestEqual(TEXT("One control, two bindings"), Commands.Num(), 2))
	{
		TestEqual(TEXT("Note level is velocity"), Commands[0].Value, 1.f);
		TestEqual(TEXT("Note level playback"), Commands[0].Playback, 1);
		TestTrue(TEXT("Blackout down"), Commands[1].IsDown());
	}
	Commands = Resolve(MidiNoteOff, 3, 62, 0, Notes);
	if (TestEqual(TEXT("Release of both"), Commands.Num(), 2))
	{
		TestEqual(TEXT("Note level falls to 0"), Commands[0].Value, 0.f);
		TestFalse(TEXT("Blackout up"), Commands[1].IsDown());
	}

	// Nothing from malformed or other messages.
	TestEqual(TEXT("Channel 0"), Resolve(MidiControlChange, 0, 0, 64, Defaults).Num(), 0);
	TestEqual(TEXT("Channel 17"), Resolve(MidiControlChange, 17, 0, 64, Defaults).Num(), 0);
	TestEqual(TEXT("Data over 127"), Resolve(MidiControlChange, 1, 0, 128, Defaults).Num(), 0);
	TestEqual(TEXT("Negative data"), Resolve(MidiControlChange, 1, -1, 64, Defaults).Num(), 0);
	TestEqual(TEXT("Pitch bend"), Resolve(0xE, 1, 0, 64, Defaults).Num(), 0);
	TestEqual(TEXT("Program change"), Resolve(0xC, 1, 0, 64, Defaults).Num(), 0);
	TArray<FStageMidiBinding> Broken = { MakeBinding(EStageMidiTrigger::ControlChange, 0, 5, EStageRemoteAction::PlaybackGo, 11) };
	TestEqual(TEXT("An invalid binding never fires"), Resolve(MidiControlChange, 1, 5, 127, Broken).Num(), 0);

	// Learn: a press with a value can be learned; a release or another message cannot.
	FStageMidiBinding Learned = MakeBinding(EStageMidiTrigger::Note, 0, 0, EStageRemoteAction::GrandMaster);
	TestTrue(TEXT("Learn a CC"), LearnFromMidi(MidiControlChange, 2, 7, 10, Learned));
	TestEqual(TEXT("Learned trigger"), Learned.Trigger, EStageMidiTrigger::ControlChange);
	TestEqual(TEXT("Learned channel"), Learned.Channel, 2);
	TestEqual(TEXT("Learned number"), Learned.Number, 7);
	TestEqual(TEXT("Learn keeps the action"), Learned.Action, EStageRemoteAction::GrandMaster);
	TestTrue(TEXT("Learn a note"), LearnFromMidi(MidiNoteOn, 1, 36, 90, Learned) && Learned.Trigger == EStageMidiTrigger::Note);
	TestFalse(TEXT("No learning a release"), LearnFromMidi(MidiNoteOn, 1, 36, 0, Learned));
	TestFalse(TEXT("No learning a CC at 0"), LearnFromMidi(MidiControlChange, 1, 7, 0, Learned));
	TestFalse(TEXT("No learning Note Off"), LearnFromMidi(MidiNoteOff, 1, 36, 64, Learned));
	TestFalse(TEXT("No learning pitch bend"), LearnFromMidi(0xE, 1, 0, 64, Learned));

	TestEqual(TEXT("Describe CC"), DescribeMidi(MidiControlChange, 1, 7, 64), FString(TEXT("MIDI CC 7 ch 1 = 64")));
	TestEqual(TEXT("Describe note"), DescribeMidi(MidiNoteOn, 10, 60, 0), FString(TEXT("MIDI note 60 ch 10 off")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteQueueTest, "StageCraft.Remote.Queue", StageRemoteControlTests::Flags)
bool FStageRemoteQueueTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;

	TArray<FStageRemoteCommand> Pending;
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::PlaybackLevel, 0, 0.1f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::CueGo));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::PlaybackLevel, 0, 0.2f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::PlaybackLevel, 1, 0.9f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::CueGo));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::PlaybackLevel, 0, 0.3f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::GrandMaster, 0, 0.5f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::GrandMaster, 0, 0.4f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::PlaybackFlash, 2, 1.f));
	StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::PlaybackFlash, 2, 0.f));

	// One per continuous target, in its first place with the latest value; triggers and flashes all kept, in order.
	const TArray<FStageRemoteCommand> Expected = { MakeCommand(EStageRemoteAction::PlaybackLevel, 0, 0.3f), MakeCommand(EStageRemoteAction::CueGo),
		MakeCommand(EStageRemoteAction::PlaybackLevel, 1, 0.9f), MakeCommand(EStageRemoteAction::CueGo), MakeCommand(EStageRemoteAction::GrandMaster, 0, 0.4f),
		MakeCommand(EStageRemoteAction::PlaybackFlash, 2, 1.f), MakeCommand(EStageRemoteAction::PlaybackFlash, 2, 0.f) };
	TestTrue(TEXT("Coalesced in order"), Pending == Expected);

	// Full: new commands are dropped, a queued continuous target still takes the latest value.
	Pending.Reset();
	int32 Accepted = 0;
	for (int32 Index = 0; Index < StageRemote::MaxCommandsPerFrame + 20; ++Index)
	{
		Accepted += StageRemote::Enqueue(Pending, MakeCommand(Index == 0 ? EStageRemoteAction::GrandMaster : EStageRemoteAction::CueGo, 0, 0.1f)) ? 1 : 0;
	}
	TestEqual(TEXT("At most the frame's limit"), Pending.Num(), StageRemote::MaxCommandsPerFrame);
	TestEqual(TEXT("The rest dropped"), Accepted, StageRemote::MaxCommandsPerFrame);
	TestTrue(TEXT("A queued fader still updates when full"), StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::GrandMaster, 0, 0.8f)));
	TestEqual(TEXT("Its latest value"), Pending[0].Value, 0.8f);
	TestFalse(TEXT("A new fader target is dropped when full"), StageRemote::Enqueue(Pending, MakeCommand(EStageRemoteAction::AudioMaster, 0, 0.8f)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteSettingsTest, "StageCraft.Remote.Settings", StageRemoteControlTests::Flags)
bool FStageRemoteSettingsTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;

	FStageRemoteSettings Clean;
	TestFalse(TEXT("Defaults are already clean"), StageRemote::Sanitize(Clean));
	TestEqual(TEXT("OSC is off by default"), Clean.OscScope, EStageOscScope::Off);
	TestTrue(TEXT("MIDI is off by default"), Clean.MidiInputName.IsEmpty());
	TestEqual(TEXT("Default map in force"), StageRemote::EffectiveMidiBindings(Clean).Num(), StageRemote::DefaultMidiBindings().Num());

	FStageRemoteSettings Dirty;
	Dirty.OscScope = static_cast<EStageOscScope>(7);
	Dirty.OscPort = 80;
	Dirty.OscFeedbackPort = 70000;
	Dirty.MidiInputName = TEXT("  nanoKONTROL2  ");
	Dirty.MidiOutputName = TEXT("\tnanoKONTROL2 ");
	Dirty.bCustomMidiMap = true;
	FStageMidiBinding NaNCue = MakeBinding(EStageMidiTrigger::Note, 1, 1, EStageRemoteAction::CueGoTo);
	NaNCue.CueNumber = std::numeric_limits<float>::quiet_NaN();
	Dirty.MidiBindings = {
		MakeBinding(EStageMidiTrigger::ControlChange, 1, 7, EStageRemoteAction::GrandMaster),
		MakeBinding(EStageMidiTrigger::ControlChange, 1, 8, EStageRemoteAction::None),
		MakeBinding(EStageMidiTrigger::ControlChange, 1, 9, EStageRemoteAction::PlaybackGo, 11),
		MakeBinding(EStageMidiTrigger::ControlChange, 17, 10, EStageRemoteAction::CueGo),
		MakeBinding(EStageMidiTrigger::ControlChange, 1, 128, EStageRemoteAction::CueGo),
		NaNCue,
		MakeBinding(static_cast<EStageMidiTrigger>(9), 1, 11, EStageRemoteAction::CueGo),
		MakeBinding(EStageMidiTrigger::ControlChange, 1, 7, EStageRemoteAction::AudioMaster) };
	TestTrue(TEXT("Dirty settings change"), StageRemote::Sanitize(Dirty));
	TestEqual(TEXT("Unknown scope is Off"), Dirty.OscScope, EStageOscScope::Off);
	TestEqual(TEXT("Privileged port reset"), Dirty.OscPort, StageRemote::DefaultOscPort);
	TestEqual(TEXT("Feedback port reset"), Dirty.OscFeedbackPort, StageRemote::DefaultFeedbackPort);
	TestEqual(TEXT("Device name trimmed"), Dirty.MidiInputName, FString(TEXT("nanoKONTROL2")));
	TestEqual(TEXT("Output name trimmed"), Dirty.MidiOutputName, FString(TEXT("nanoKONTROL2")));
	if (TestEqual(TEXT("Only the valid, newest binding of CC 7 is kept"), Dirty.MidiBindings.Num(), 1))
	{
		TestEqual(TEXT("The later binding wins"), Dirty.MidiBindings[0].Action, EStageRemoteAction::AudioMaster);
	}
	TestFalse(TEXT("Sanitizing is stable"), StageRemote::Sanitize(Dirty));

	// An "any channel" binding and a channel-3 binding of CC 7 both answer CC 7 on channel 3: the later one is kept.
	FStageRemoteSettings Overlap;
	Overlap.bCustomMidiMap = true;
	Overlap.MidiBindings = { MakeBinding(EStageMidiTrigger::ControlChange, 0, 7, EStageRemoteAction::GrandMaster),
		MakeBinding(EStageMidiTrigger::ControlChange, 3, 7, EStageRemoteAction::AudioMaster),
		MakeBinding(EStageMidiTrigger::Note, 0, 7, EStageRemoteAction::Blackout) };
	TestTrue(TEXT("Overlapping bindings repaired"), StageRemote::Sanitize(Overlap));
	if (TestEqual(TEXT("The overlap is resolved, the note is another control"), Overlap.MidiBindings.Num(), 2))
	{
		TestEqual(TEXT("The later binding of CC 7 wins"), Overlap.MidiBindings[0].Action, EStageRemoteAction::AudioMaster);
	}

	FStageRemoteSettings NoFeedback;
	NoFeedback.OscFeedbackPort = 0;
	TestFalse(TEXT("Feedback port 0 means off and is kept"), StageRemote::Sanitize(NoFeedback));

	FStageRemoteSettings EmptyCustom;
	EmptyCustom.bCustomMidiMap = true;
	TestEqual(TEXT("A custom empty map is empty"), StageRemote::EffectiveMidiBindings(EmptyCustom).Num(), 0);

	FStageRemoteSettings Long;
	Long.MidiInputName = FString::ChrN(500, TEXT('m'));
	Long.MidiOutputName = FString::ChrN(500, TEXT('o'));
	StageRemote::Sanitize(Long);
	TestEqual(TEXT("Device name cut"), Long.MidiInputName.Len(), StageRemote::MaxDeviceNameLength);
	TestEqual(TEXT("Output name cut"), Long.MidiOutputName.Len(), StageRemote::MaxDeviceNameLength);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteMidiMapTest, "StageCraft.Remote.MidiMap", StageRemoteControlTests::Flags)
bool FStageRemoteMidiMapTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	using namespace StageRemote;

	// Targets: the action, the playback for playback actions, the cue for go to cue.
	const FStageMidiBinding Fader3 = MakeBinding(EStageMidiTrigger::ControlChange, 0, 2, EStageRemoteAction::PlaybackLevel, 3);
	TestTrue(TEXT("Same playback, same target"), IsSameTarget(Fader3, EStageRemoteAction::PlaybackLevel, 3));
	TestFalse(TEXT("Another playback"), IsSameTarget(Fader3, EStageRemoteAction::PlaybackLevel, 4));
	TestFalse(TEXT("Another action"), IsSameTarget(Fader3, EStageRemoteAction::PlaybackFlash, 3));
	const FStageMidiBinding Grand = MakeBinding(EStageMidiTrigger::ControlChange, 0, 16, EStageRemoteAction::GrandMaster, 4);
	TestTrue(TEXT("The playback does not matter for a master"), IsSameTarget(Grand, EStageRemoteAction::GrandMaster, 9));
	FStageMidiBinding GoTo5 = MakeBinding(EStageMidiTrigger::Note, 1, 40, EStageRemoteAction::CueGoTo);
	GoTo5.CueNumber = 5.f;
	TestTrue(TEXT("Go to cue 5"), IsSameTarget(GoTo5, EStageRemoteAction::CueGoTo, 1, 5.f));
	TestFalse(TEXT("Go to cue 6 is another target"), IsSameTarget(GoTo5, EStageRemoteAction::CueGoTo, 1, 6.f));
	TestFalse(TEXT("None is no target"), IsSameTarget(MakeBinding(EStageMidiTrigger::Note, 1, 1, EStageRemoteAction::None), EStageRemoteAction::None, 1));

	const TArray<FStageMidiBinding> Found = BindingsForTarget(DefaultMidiBindings(), EStageRemoteAction::PlaybackLevel, 3);
	if (TestEqual(TEXT("The default map drives playback 3's fader once"), Found.Num(), 1))
	{
		TestEqual(TEXT("With fader 3 (CC 2)"), Found[0].Number, 2);
	}
	TestEqual(TEXT("Nothing drives playback 9's fader by default"), BindingsForTarget(DefaultMidiBindings(), EStageRemoteAction::PlaybackLevel, 9).Num(), 0);
	TestEqual(TEXT("Describe any channel"), DescribeControl(Fader3), FString(TEXT("CC 2")));
	TestEqual(TEXT("Describe a note on a channel"), DescribeControl(GoTo5), FString(TEXT("Note 40 ch 1")));

	// LEARN from the default map: the map becomes the user's, the target keeps only the new control, the control only this target.
	FStageRemoteSettings Settings;
	FStageMidiBinding Learned = MakeBinding(EStageMidiTrigger::ControlChange, 2, 41, EStageRemoteAction::GrandMaster);
	TestTrue(TEXT("Learn a control"), AssignLearned(Settings, Learned));
	TestTrue(TEXT("The map is the user's now"), Settings.bCustomMidiMap);
	TArray<FStageMidiBinding> GrandControls = BindingsForTarget(EffectiveMidiBindings(Settings), EStageRemoteAction::GrandMaster, 1);
	if (TestEqual(TEXT("The Grand Master has one control"), GrandControls.Num(), 1))
	{
		TestTrue(TEXT("The learned one"), GrandControls[0] == Learned);
	}
	TestEqual(TEXT("Knob 1 (CC 16) drives nothing now"), Resolve(MidiControlChange, 1, 16, 64, EffectiveMidiBindings(Settings)).Num(), 0);
	// ▶ is CC 41 on any channel in the default map, so it would also answer CC 41 on channel 2: learning takes the control from Cue GO.
	TestEqual(TEXT("The Grand Master's knob and Cue GO's any-channel binding left; the learned one came"), Settings.MidiBindings.Num(), DefaultMidiBindings().Num() - 1);
	const TArray<FStageRemoteCommand> Pressed = Resolve(MidiControlChange, 2, 41, 127, EffectiveMidiBindings(Settings));
	TestTrue(TEXT("CC 41 on channel 2 drives only the Grand Master"), Pressed.Num() == 1 && Pressed[0].Action == EStageRemoteAction::GrandMaster);
	TestEqual(TEXT("Cue GO has no control now"), BindingsForTarget(EffectiveMidiBindings(Settings), EStageRemoteAction::CueGo, 1).Num(), 0);
	TestTrue(TEXT("Overlap: any channel and channel 2"), OverlapsTrigger(MakeBinding(EStageMidiTrigger::ControlChange, 0, 41, EStageRemoteAction::CueGo), Learned));
	TestFalse(TEXT("No overlap: channels 1 and 2"), OverlapsTrigger(MakeBinding(EStageMidiTrigger::ControlChange, 1, 41, EStageRemoteAction::CueGo), Learned));
	TestFalse(TEXT("No overlap: a note and a CC"), OverlapsTrigger(MakeBinding(EStageMidiTrigger::Note, 0, 41, EStageRemoteAction::CueGo), Learned));

	// The same control learned for another target leaves the first one.
	FStageMidiBinding Again = Learned;
	Again.Action = EStageRemoteAction::AudioMaster;
	TestTrue(TEXT("Learn the same control again"), AssignLearned(Settings, Again));
	TestEqual(TEXT("The Grand Master lost it"), BindingsForTarget(EffectiveMidiBindings(Settings), EStageRemoteAction::GrandMaster, 1).Num(), 0);
	GrandControls = BindingsForTarget(EffectiveMidiBindings(Settings), EStageRemoteAction::AudioMaster, 1);
	TestTrue(TEXT("The audio master has only it"), GrandControls.Num() == 1 && GrandControls[0].Number == 41);
	TestFalse(TEXT("Sanitize finds nothing to repair"), Sanitize(Settings));

	FStageRemoteSettings Untouched;
	TestFalse(TEXT("An invalid binding is not learned"), AssignLearned(Untouched, MakeBinding(EStageMidiTrigger::ControlChange, 1, 5, EStageRemoteAction::PlaybackGo, 11)));
	TestFalse(TEXT("And the map stays the default"), Untouched.bCustomMidiMap);

	// CLEAR: from the default map too; nothing to clear changes nothing.
	FStageRemoteSettings Cleared;
	TestTrue(TEXT("Clear playback 2's GO"), ClearTarget(Cleared, EStageRemoteAction::PlaybackGo, 2));
	TestTrue(TEXT("Clearing made the map the user's"), Cleared.bCustomMidiMap);
	TestEqual(TEXT("One control fewer"), Cleared.MidiBindings.Num(), DefaultMidiBindings().Num() - 1);
	TestEqual(TEXT("M2 (CC 49) does nothing now"), Resolve(MidiControlChange, 1, 49, 127, EffectiveMidiBindings(Cleared)).Num(), 0);
	TestFalse(TEXT("Clearing it again changes nothing"), ClearTarget(Cleared, EStageRemoteAction::PlaybackGo, 2));
	FStageRemoteSettings Default;
	TestFalse(TEXT("Nothing drives playback 9's flash: nothing to clear"), ClearTarget(Default, EStageRemoteAction::PlaybackFlash, 9));
	TestFalse(TEXT("The default map stays the default"), Default.bCustomMidiMap);

	// Which changes need the GameMode: switching on or moving an input, never closing or a map edit.
	FStageRemoteSettings Off;
	FStageRemoteSettings MidiOn = Off;
	MidiOn.MidiInputName = TEXT("nanoKONTROL2");
	FStageRemoteSettings OutputOn = Off;
	OutputOn.MidiOutputName = TEXT("nanoKONTROL2");
	FStageRemoteSettings MapEdited = MidiOn;
	ClearTarget(MapEdited, EStageRemoteAction::CueGo, 1);
	TestFalse(TEXT("Nothing on"), HasInputOn(Off));
	TestTrue(TEXT("An output alone counts"), HasInputOn(OutputOn));
	TestTrue(TEXT("Switching MIDI on is judged"), NeedsVerdict(Off, MidiOn));
	TestTrue(TEXT("Switching the output on is judged"), NeedsVerdict(Off, OutputOn));
	TestFalse(TEXT("Switching off is not"), NeedsVerdict(MidiOn, Off));
	TestTrue(TEXT("A map edit keeps the inputs"), HasSameInputs(MidiOn, MapEdited));
	TestFalse(TEXT("A map edit is not judged"), NeedsVerdict(MidiOn, MapEdited));
	TestTrue(TEXT("Applying the same settings again (a retry) is judged"), NeedsVerdict(MidiOn, MidiOn));
	FStageRemoteSettings OtherPort = MidiOn;
	OtherPort.OscPort = 9001;
	TestFalse(TEXT("Another port is another input"), HasSameInputs(MidiOn, OtherPort));
	FStageRemoteSettings MapAndDevice = MapEdited;
	MapAndDevice.MidiOutputName = TEXT("Other");
	TestTrue(TEXT("A map edit with a new device is judged"), NeedsVerdict(MidiOn, MapAndDevice));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteMidiFeedbackTest, "StageCraft.Remote.MidiFeedback", StageRemoteControlTests::Flags)
bool FStageRemoteMidiFeedbackTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	using namespace StageRemote;

	FMidiShowState State;
	State.Levels.Init(0.f, StagePlaybackRules::Count);
	State.Flashing.Init(false, StagePlaybackRules::Count);
	State.Active.Init(false, StagePlaybackRules::Count);
	State.Levels[0] = 0.5f;
	State.Active[0] = true;
	State.Flashing[1] = true;
	State.GrandMaster = 1.f;
	State.AudioMaster = 0.f;
	State.bBlackout = true;

	// What each kind of control shows.
	TestEqual(TEXT("A fader shows its level"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 0, EStageRemoteAction::PlaybackLevel, 1), State).Get(-1), 64);
	TestEqual(TEXT("Flash lit while flashing"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 33, EStageRemoteAction::PlaybackFlash, 2), State).Get(-1), 127);
	TestEqual(TEXT("Playback GO lit while live"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 48, EStageRemoteAction::PlaybackGo, 1), State).Get(-1), 127);
	TestEqual(TEXT("Playback GO dark while released"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 49, EStageRemoteAction::PlaybackGo, 2), State).Get(-1), 0);
	TestEqual(TEXT("Blackout lit"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 45, EStageRemoteAction::Blackout), State).Get(-1), 127);
	TestEqual(TEXT("Grand Master at full"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 16, EStageRemoteAction::GrandMaster), State).Get(-1), 127);
	TestFalse(TEXT("Cue GO shows nothing"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 41, EStageRemoteAction::CueGo), State).IsSet());
	TestFalse(TEXT("Release shows nothing"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 64, EStageRemoteAction::PlaybackRelease, 1), State).IsSet());
	TestFalse(TEXT("An invalid binding shows nothing"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 5, EStageRemoteAction::PlaybackLevel, 11), State).IsSet());
	FMidiShowState Broken = State;
	Broken.GrandMaster = std::numeric_limits<float>::quiet_NaN();
	Broken.AudioMaster = 3.f;
	TestEqual(TEXT("A NaN level shows 0"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 16, EStageRemoteAction::GrandMaster), Broken).Get(-1), 0);
	TestEqual(TEXT("Over full shows 127"), FeedbackValue(MakeBinding(EStageMidiTrigger::ControlChange, 0, 17, EStageRemoteAction::AudioMaster), Broken).Get(-1), 127);

	// The default map: everything with a state is sent once, then only changes.
	const TConstArrayView<FStageMidiBinding> Defaults = DefaultMidiBindings();
	FStageMidiFeedbackMemory Memory;
	TArray<FMidiOutMessage> Messages;
	double HoldUntil = 0.0;
	double Now = 100.0;
	const auto Collect = [&](TConstArrayView<FStageMidiBinding> Bindings, FStageMidiFeedbackMemory& InMemory)
	{
		return CollectMidiFeedback(Bindings, State, InMemory, Now, Messages, HoldUntil);
	};
	TestEqual(TEXT("First send: 8 faders, 8 flashes, 8 GOs, two masters and Blackout"), Collect(Defaults, Memory), 27);
	const FMidiOutMessage* Fader1 = Messages.FindByPredicate([](const FMidiOutMessage& Message) { return Message.Data1 == 0; });
	if (TestNotNull(TEXT("Fader 1 sent"), Fader1))
	{
		TestTrue(TEXT("As CC 0 = 64 on channel 1"), *Fader1 == FMidiOutMessage{ MidiControlChange, 1, 0, 64 });
	}
	Messages.Reset();
	TestEqual(TEXT("Nothing changed, nothing sent"), Collect(Defaults, Memory), 0);
	State.Levels[2] = 1.f;
	TestEqual(TEXT("One fader moved"), Collect(Defaults, Memory), 1);
	TestTrue(TEXT("Fader 3 to the top"), Messages.Num() == 1 && Messages[0] == FMidiOutMessage{ MidiControlChange, 1, 2, 127 });

	// A fader's own move is not sent back; a button's press is not taken for its light.
	Messages.Reset();
	TestTrue(TEXT("Fader 4 moved by hand"), NoteMidiInput(MidiControlChange, 1, 3, 100, Defaults, Memory, Now));
	State.Levels[3] = 100.f / 127.f;
	TestEqual(TEXT("The desk followed fader 4: nothing to send"), Collect(Defaults, Memory), 0);

	// During a move the desk is a frame behind the hand: the older value must not be sent back while the hand holds the fader.
	Now += 0.02;
	NoteMidiInput(MidiControlChange, 1, 3, 101, Defaults, Memory, Now);
	TestEqual(TEXT("The desk is still at 100: held, nothing sent"), Collect(Defaults, Memory), 0);
	TestEqual(TEXT("The hold ends half a second after the last move"), HoldUntil, Now + MidiHoldSeconds);
	State.Levels[3] = 101.f / 127.f;
	Now += 1.0;
	TestEqual(TEXT("After the hold, the desk caught up: nothing to send"), Collect(Defaults, Memory), 0);
	TestEqual(TEXT("No hold left"), HoldUntil, 0.0);

	// A refused move: held while the hand is on it, then the fader returns to the desk's level.
	NoteMidiInput(MidiControlChange, 1, 4, 90, Defaults, Memory, Now);
	TestEqual(TEXT("Refused fader 5, still held"), Collect(Defaults, Memory), 0);
	Now += MidiHoldSeconds + 0.01;
	TestEqual(TEXT("A refused move snaps fader 5 back after the hold"), Collect(Defaults, Memory), 1);
	TestTrue(TEXT("To the desk's level"), Messages.Num() == 1 && Messages[0] == FMidiOutMessage{ MidiControlChange, 1, 4, 0 });

	// The fader's own value coming back from the output is an echo; another value, or a late one, is a hand.
	TestTrue(TEXT("Echo of the value just sent"), IsMidiEcho(MidiControlChange, 1, 4, 0, Memory, Now + 0.05));
	TestFalse(TEXT("Another value is a hand"), IsMidiEcho(MidiControlChange, 1, 4, 5, Memory, Now + 0.05));
	TestFalse(TEXT("Another channel is a hand"), IsMidiEcho(MidiControlChange, 2, 4, 0, Memory, Now + 0.05));
	TestFalse(TEXT("Too late to be an echo"), IsMidiEcho(MidiControlChange, 1, 4, 0, Memory, Now + 0.5));
	TestEqual(TEXT("Old sent messages are forgotten"), Memory.RecentSent.Num(), 0);

	Messages.Reset();
	TestFalse(TEXT("A button is not a fader"), NoteMidiInput(MidiControlChange, 1, 34, 127, Defaults, Memory, Now));
	State.Flashing[2] = true;
	TestEqual(TEXT("S3 pressed: its light is still sent at once"), Collect(Defaults, Memory), 1);
	TestTrue(TEXT("S3 lit"), Messages.Num() == 1 && Messages[0] == FMidiOutMessage{ MidiControlChange, 1, 34, 127 });

	// Reset (output reopened, map changed): everything again.
	Messages.Reset();
	Memory.Reset();
	TestEqual(TEXT("After a reset everything is sent"), Collect(Defaults, Memory), 27);

	// The map changed: lit or raised controls no binding answers any more go dark; the rest wait for the full send.
	TArray<FStageMidiBinding> Fewer(Defaults.GetData(), Defaults.Num());
	Fewer.RemoveAll([](const FStageMidiBinding& Binding) { return Binding.Action == EStageRemoteAction::Blackout || Binding.Action == EStageRemoteAction::GrandMaster; });
	Messages.Reset();
	TestEqual(TEXT("Blackout and the Grand Master knob were lit or raised: both sent 0"), CollectUnboundOff(Fewer, Memory, Now, Messages), 2);
	TestTrue(TEXT("Knob 1 to 0"), Messages.Contains(FMidiOutMessage{ MidiControlChange, 1, 16, 0 }));
	TestTrue(TEXT("Blackout light off"), Messages.Contains(FMidiOutMessage{ MidiControlChange, 1, 45, 0 }));
	Messages.Reset();
	TestEqual(TEXT("Nothing more to darken"), CollectUnboundOff(Fewer, Memory, Now, Messages), 0);

	// An "any channel" control answers on the channel it was heard on; a note binding answers with a note; one answer per control.
	const TArray<FStageMidiBinding> Pads = {
		MakeBinding(EStageMidiTrigger::Note, 0, 36, EStageRemoteAction::Blackout),
		MakeBinding(EStageMidiTrigger::Note, 0, 36, EStageRemoteAction::PlaybackFlash, 1),
		MakeBinding(EStageMidiTrigger::ControlChange, 5, 20, EStageRemoteAction::GrandMaster) };
	FStageMidiFeedbackMemory PadMemory;
	NoteMidiInput(MidiNoteOn, 10, 36, 100, Pads, PadMemory, Now);
	Messages.Reset();
	TestEqual(TEXT("Two controls answer"), Collect(Pads, PadMemory), 2);
	if (TestEqual(TEXT("Two messages"), Messages.Num(), 2))
	{
		TestTrue(TEXT("The pad lights as Blackout, with a note on the channel it was heard on"), Messages[0] == FMidiOutMessage{ MidiNoteOn, 10, 36, 127 });
		TestTrue(TEXT("The CC on its own channel"), Messages[1] == FMidiOutMessage{ MidiControlChange, 5, 20, 127 });
	}
	State.bBlackout = false;
	Messages.Reset();
	TestEqual(TEXT("The pad goes dark with Blackout, whatever the flash does"), Collect(Pads, PadMemory), 1);
	TestTrue(TEXT("Velocity 0"), Messages.Num() == 1 && Messages[0] == FMidiOutMessage{ MidiNoteOn, 10, 36, 0 });

	// Bad input never enters the memory.
	FStageMidiFeedbackMemory Clean;
	NoteMidiInput(MidiControlChange, 0, 3, 64, Defaults, Clean, Now);
	NoteMidiInput(MidiControlChange, 17, 3, 64, Defaults, Clean, Now);
	NoteMidiInput(MidiControlChange, 1, 300, 64, Defaults, Clean, Now);
	NoteMidiInput(0xE, 1, 3, 64, Defaults, Clean, Now);
	TestEqual(TEXT("Nothing remembered from bad input"), Clean.Shown.Num() + Clean.HeardChannel.Num() + Clean.LastMoved.Num(), 0);
	TestFalse(TEXT("Bad input is no echo"), IsMidiEcho(0xE, 1, 3, 64, Clean, Now));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteSubsystemTest, "StageCraft.Remote.Subsystem", StageRemoteControlTests::Flags)
bool FStageRemoteSubsystemTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	// The default map and both inputs off, in memory only: whatever this PC saved does not open a port or change the map here.
	const FScopedRemoteSettings Settings{ FStageRemoteSettings() };
	FScopedWorld Stage;
	UStageRemoteControlSubsystem* Remote = Stage.GetRemote();
	if (!TestNotNull(TEXT("Remote subsystem in a game world"), Remote))
	{
		return false;
	}
	TestFalse(TEXT("OSC is closed"), Remote->GetStatus().bOscListening);
	TestFalse(TEXT("MIDI is closed"), Remote->GetStatus().bMidiOpen);

	// Without a controller there is nobody to carry commands out: refused and counted.
	Remote->ReceiveOsc(TEXT("/stagecraft/cue/go"), {}, FString());
	Remote->FlushCommands();
	TestEqual(TEXT("No handler: refused"), Remote->GetStatus().Refused, int64(1));
	TestFalse(TEXT("No handler: the reason is shown"), Remote->GetStatus().LastRefusal.IsEmpty());

	TArray<FStageRemoteCommand> Carried;
	bool bRefuse = false;
	Remote->SetCommandHandler(UStageRemoteControlSubsystem::FCommandHandler::CreateLambda([&Carried, &bRefuse](const FStageRemoteCommand& Command)
	{
		Carried.Add(Command);
		return bRefuse ? FStageEconomyResultInfo::Fail(EStageEconomyResult::SessionRuleViolation, INVTEXT("Not now")) : FStageEconomyResultInfo::Ok();
	}));

	// A frame of input: a fader's burst becomes one command, GO keeps its place; nothing runs before the flush.
	Remote->ReceiveOsc(TEXT("/stagecraft/playback/1/level"), 0.2f, FString());
	Remote->ReceiveOsc(TEXT("/stagecraft/cue/go"), 1.f, FString());
	Remote->ReceiveOsc(TEXT("/stagecraft/playback/1/level"), 0.6f, FString());
	Remote->ReceiveOsc(TEXT("/stagecraft/nothing"), 1.f, FString());
	Remote->ReceiveMidi(StageRemote::MidiControlChange, 1, 16, 127);
	Remote->ReceiveMidi(0xF, 1, 0, 0);
	TestEqual(TEXT("Nothing before the flush"), Carried.Num(), 0);
	Remote->FlushCommands();
	if (TestEqual(TEXT("Three commands carried out"), Carried.Num(), 3))
	{
		TestEqual(TEXT("Fader first, with its latest value"), Carried[0].Value, 0.6f);
		TestEqual(TEXT("Then GO"), Carried[1].Action, EStageRemoteAction::CueGo);
		TestEqual(TEXT("Then knob 1: the Grand Master"), Carried[2].Action, EStageRemoteAction::GrandMaster);
	}
	const FStageRemoteStatus& Status = Remote->GetStatus();
	TestEqual(TEXT("Received (system messages not counted)"), Status.Received, int64(6));
	TestEqual(TEXT("Applied"), Status.Applied, int64(3));
	TestEqual(TEXT("Ignored: the unknown address"), Status.Ignored, int64(1));
	TestEqual(TEXT("Last input"), Status.LastInput, FString(TEXT("MIDI CC 16 ch 1 = 127")));

	// The GameMode's refusal is counted and shown.
	bRefuse = true;
	Remote->ReceiveOsc(TEXT("/stagecraft/master/blackout"), 1.f, FString());
	Remote->FlushCommands();
	TestEqual(TEXT("Refused counted"), Status.Refused, int64(2));
	TestEqual(TEXT("Refusal reason"), Status.LastRefusal.ToString(), FString(TEXT("Not now")));
	bRefuse = false;

	// A flooding sender: one frame carries out at most the limit.
	Carried.Reset();
	for (int32 Index = 0; Index < StageRemote::MaxCommandsPerFrame + 44; ++Index)
	{
		Remote->ReceiveOsc(TEXT("/stagecraft/cue/back"), {}, FString());
	}
	Remote->FlushCommands();
	TestEqual(TEXT("At most the limit per frame"), Carried.Num(), StageRemote::MaxCommandsPerFrame);
	TestEqual(TEXT("The rest dropped"), Status.Dropped, int64(44));

	// Learn needs an open MIDI input; a cancel always works.
	FText Error;
	TestFalse(TEXT("Learn without MIDI is refused"), Remote->StartMidiLearn(EStageRemoteAction::CueGo, 1, 1.f, Error));
	TestFalse(TEXT("Learn says why"), Error.IsEmpty());
	TestTrue(TEXT("Cancel learn"), Remote->StartMidiLearn(EStageRemoteAction::None, 1, 1.f, Error));

	// A handler bound by another owner is not cleared by a stranger.
	const AActor* Stranger = Stage.World->SpawnActor<AActor>();
	Remote->ClearCommandHandler(Stranger);
	Remote->ReceiveOsc(TEXT("/stagecraft/cue/go"), {}, FString());
	Remote->FlushCommands();
	TestEqual(TEXT("Handler kept"), Carried.Last().Action, EStageRemoteAction::CueGo);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteMidiOutputTest, "StageCraft.Remote.MidiOutput", StageRemoteControlTests::Flags)
bool FStageRemoteMidiOutputTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	// Windows' own synthesizer is a MIDI output on every Windows PC. The default map sends it control changes only (no notes), so
	// nothing is heard.
	const FString Synth = TEXT("Microsoft GS Wavetable Synth");
	TArray<FMIDIDeviceInfo> InputInfos;
	TArray<FMIDIDeviceInfo> OutputInfos;
	UMIDIDeviceManager::FindAllMIDIDeviceInfo(InputInfos, OutputInfos);
	const FMIDIDeviceInfo* SynthInfo = OutputInfos.FindByPredicate([&Synth](const FMIDIDeviceInfo& Info) { return Info.DeviceName == Synth; });
	if (!SynthInfo || SynthInfo->bIsAlreadyInUse)
	{
		AddInfo(TEXT("No free Microsoft GS Wavetable Synth output on this PC: the real-device check is skipped."));
		return true;
	}

	// In memory only, so the world opens the output at BeginPlay and the user's ini is never written. Kept in shared state: the settle
	// after a hand's hold is a world timer, and the world's timers run once per engine frame, so the end of the test is latent.
	struct FOutputState
	{
		TUniquePtr<FScopedRemoteSettings> Settings;
		TUniquePtr<FScopedWorld> Stage;
		double Deadline = 0.0;
	};
	TSharedRef<FOutputState> State = MakeShared<FOutputState>();
	FStageRemoteSettings WithOutput;
	WithOutput.MidiOutputName = Synth;
	State->Settings = MakeUnique<FScopedRemoteSettings>(WithOutput);
	State->Stage = MakeUnique<FScopedWorld>();
	UStageRemoteControlSubsystem* Remote = State->Stage->GetRemote();
	UShowControlSubsystem* Show = State->Stage->World->GetSubsystem<UShowControlSubsystem>();
	if (!TestNotNull(TEXT("Remote subsystem"), Remote) || !TestNotNull(TEXT("Show"), Show))
	{
		return false;
	}
	const FStageRemoteStatus& Status = Remote->GetStatus();
	TestTrue(TEXT("The output opened"), Status.bMidiOutputOpen);
	TestEqual(TEXT("By its name"), Status.MidiOutputDevice, Synth);
	TestFalse(TEXT("The input stays off"), Status.bMidiOpen);
	TestTrue(TEXT("No problem shown"), Status.LastProblem.IsEmpty());

	Remote->SendFeedback();
	TestEqual(TEXT("The controller is sent the whole default map once"), Status.MidiSent, int64(27));

	// Fader 1 was just sent 0: the same value arriving now is the output's echo, not a hand, and carries nothing out.
	const int64 IgnoredBefore = Status.Ignored;
	Remote->ReceiveMidi(StageRemote::MidiControlChange, 1, 0, 0);
	TestEqual(TEXT("The echo is ignored"), Status.Ignored, IgnoredBefore + 1);
	TestTrue(TEXT("And shown as an echo"), Status.LastInput.Contains(TEXT("echo")));

	Remote->SendFeedback();
	TestEqual(TEXT("Then nothing while the show stands still"), Status.MidiSent, int64(27));
	Show->SetGrandMaster(0.5f);
	Remote->SendFeedback();
	TestEqual(TEXT("The Grand Master moved: one message"), Status.MidiSent, int64(28));

	// Knob 2 turned with nobody to carry it out: the audio master stays at full. Nothing is sent while the hand is on the knob; the
	// world's timers then settle it on the desk's value with no call from here (the show did not change, so only the settle timer can).
	Remote->ReceiveMidi(StageRemote::MidiControlChange, 1, 17, 64);
	Remote->FlushCommands();
	Remote->SendFeedback();
	TestEqual(TEXT("Held while the hand is on it"), Status.MidiSent, int64(28));

	State->Deadline = FPlatformTime::Seconds() + 10.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		UStageRemoteControlSubsystem* LatentRemote = State->Stage->GetRemote();
		const FStageRemoteStatus& LatentStatus = LatentRemote->GetStatus();
		// A tenth of a second of world time per engine frame: the hold (half a second) ends within a few frames.
		State->Stage->World->Tick(LEVELTICK_All, 0.1f);
		const bool bTimedOut = FPlatformTime::Seconds() > State->Deadline;
		if (LatentStatus.MidiSent < 29 && !bTimedOut)
		{
			return false;
		}
		TestEqual(TEXT("A refused move is answered with the desk's value after the hold"), LatentStatus.MidiSent, int64(29));

		// Applying the same settings reopens the output and sends everything again. This also proves the closed controller is not
		// restarted by the device listing the reopen makes (it would hold the device, and the new one could not open).
		FText Error;
		TestTrue(TEXT("Reopen"), LatentRemote->ApplySettings(LatentRemote->GetSettings(), Error));
		LatentRemote->SendFeedback();
		TestEqual(TEXT("Everything again after reopening"), LatentStatus.MidiSent, int64(29 + 27));
		State->Stage.Reset();
		State->Settings.Reset();
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteRulesTest, "StageCraft.Remote.Rules", StageRemoteControlTests::Flags)
bool FStageRemoteRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	FScopedWorld Stage;
	AStageCraftGameModeBase* GameMode = Stage.World->SpawnActor<AStageCraftGameModeBase>();
	const APlayerController* Player = Stage.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Game mode"), GameMode) || !TestNotNull(TEXT("Player"), Player))
	{
		return false;
	}
	TestFalse(TEXT("No player: refused"), GameMode->EvaluateRemoteControl(nullptr).IsSuccess());
	TestTrue(TEXT("Allowed by default"), GameMode->EvaluateRemoteControl(Player).IsSuccess());

	// SessionRules is a protected, defaults-only property; a test may set it through reflection, game code never does.
	const FStructProperty* RulesProperty = FindFProperty<FStructProperty>(AStageCraftGameModeBase::StaticClass(), TEXT("SessionRules"));
	if (!TestNotNull(TEXT("SessionRules property"), RulesProperty))
	{
		return false;
	}
	RulesProperty->ContainerPtrToValuePtr<FStageSessionRules>(GameMode)->bAllowRemoteControl = false;
	const FStageEconomyResultInfo Verdict = GameMode->EvaluateRemoteControl(Player);
	TestEqual(TEXT("A curated session refuses"), Verdict.Code, EStageEconomyResult::SessionRuleViolation);
	TestFalse(TEXT("And says why"), Verdict.Message.IsEmpty());
	return true;
}

namespace StageRemoteControlTests
{
	/** The loopback test's state, shared by its latent steps (the engine runs frames, and so the OSC pump, between them). */
	struct FLoopbackState
	{
		static constexpr int32 InputPort = 48765;
		static constexpr int32 FeedbackPort = 48766;

		TUniquePtr<FScopedRemoteSettings> Settings;
		TUniquePtr<FScopedWorld> Stage;
		TArray<FStageRemoteCommand> Carried;
		TArray<FString> FeedbackAddresses;
		TStrongObjectPtr<UOSCServer> Tablet;
		TStrongObjectPtr<UOSCClient> Sender;
		TStrongObjectPtr<UOSCClient> LanSender;
		bool bLanChecked = false;
		FDelegateHandle FeedbackHandle;
		double Deadline = 0.0;
		int32 FirstFeedback = 0;
		int32 Step = 0;

		void Send(const TCHAR* Address, TOptional<float> Value) const
		{
			FOSCMessage Message;
			Message.SetAddress(FOSCAddress(Address));
			if (Value.IsSet())
			{
				UOSCManager::AddFloat(Message, Value.GetValue());
			}
			Sender->SendOSCMessage(Message);
		}

		/** The world's timers carry out the commands and send the feedback; this test world is not ticked by the engine. */
		void TickStage() const
		{
			Stage->World->Tick(LEVELTICK_All, 0.01f);
		}

		void Close()
		{
			if (Tablet)
			{
				Tablet->OnOscMessageReceivedNative.Remove(FeedbackHandle);
				Tablet->Stop();
			}
			Tablet.Reset();
			Sender.Reset();
			LanSender.Reset();
			Stage.Reset();
			Settings.Reset();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageRemoteOscLoopbackTest, "StageCraft.Remote.OscLoopback", StageRemoteControlTests::Flags)
bool FStageRemoteOscLoopbackTest::RunTest(const FString& Parameters)
{
	using namespace StageRemoteControlTests;
	TSharedRef<FLoopbackState> State = MakeShared<FLoopbackState>();

	// Ports no one else uses on loopback; the input listens on 127.0.0.1 only (This computer).
	FStageRemoteSettings Loopback;
	Loopback.OscScope = EStageOscScope::ThisComputer;
	Loopback.OscPort = FLoopbackState::InputPort;
	Loopback.OscFeedbackPort = FLoopbackState::FeedbackPort;
	State->Settings = MakeUnique<FScopedRemoteSettings>(Loopback);
	State->Stage = MakeUnique<FScopedWorld>();

	UStageRemoteControlSubsystem* Remote = State->Stage->GetRemote();
	if (!TestNotNull(TEXT("Remote subsystem"), Remote))
	{
		State->Close();
		return false;
	}
	// OnWorldBeginPlay opened OSC from the settings.
	TestTrue(TEXT("OSC listening"), Remote->GetStatus().bOscListening);
	TestEqual(TEXT("This computer only"), Remote->GetStatus().OscEndpoint, FString::Printf(TEXT("port %d, this PC only"), FLoopbackState::InputPort));
	Remote->SetCommandHandler(UStageRemoteControlSubsystem::FCommandHandler::CreateLambda([WeakState = TWeakPtr<FLoopbackState>(State)](const FStageRemoteCommand& Command)
	{
		if (const TSharedPtr<FLoopbackState> Pinned = WeakState.Pin())
		{
			Pinned->Carried.Add(Command);
		}
		return FStageEconomyResultInfo::Ok();
	}));

	// A tablet: its feedback server, and the client that sends to us.
	State->Tablet.Reset(UOSCManager::CreateOSCServer(TEXT("0.0.0.0"), FLoopbackState::FeedbackPort, false, true, FString(), GetTransientPackage()));
	State->Sender.Reset(UOSCManager::CreateOSCClient(TEXT("127.0.0.1"), FLoopbackState::InputPort, FString(), GetTransientPackage()));
	if (!TestTrue(TEXT("Feedback receiver listening"), State->Tablet && State->Tablet->IsActive()) || !TestTrue(TEXT("Sender ready"), State->Sender && State->Sender->IsActive()))
	{
		State->Close();
		return false;
	}
	State->FeedbackHandle = State->Tablet->OnOscMessageReceivedNative.AddLambda([WeakState = TWeakPtr<FLoopbackState>(State)](const FOSCMessage& Message, const FString&, uint16)
	{
		if (const TSharedPtr<FLoopbackState> Pinned = WeakState.Pin())
		{
			Pinned->FeedbackAddresses.Add(Message.GetAddress().GetFullPath());
		}
	});

	State->Send(TEXT("/stagecraft/playback/2/level"), 0.75f);
	FOSCMessage Go;
	Go.SetAddress(FOSCAddress(TEXT("/stagecraft/cue/go")));
	UOSCManager::AddInt32(Go, 1);
	State->Sender->SendOSCMessage(Go);
	State->Deadline = FPlatformTime::Seconds() + 5.0;

	// The socket thread receives, the core ticker pumps between frames, the stage's next tick carries out and sends the feedback.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]() -> bool
	{
		State->TickStage();
		const bool bTimedOut = FPlatformTime::Seconds() > State->Deadline;
		switch (State->Step)
		{
		case 0:
			if (State->Carried.Num() < 2 && !bTimedOut)
			{
				return false;
			}
			if (const UStageRemoteControlSubsystem* Remote = State->Stage->GetRemote())
			{
				const FStageRemoteStatus& Status = Remote->GetStatus();
				AddInfo(FString::Printf(TEXT("Remote after the first messages: listening %d, received %lld, applied %lld, refused %lld, ignored %lld, last \"%s\"."),
					Status.bOscListening, Status.Received, Status.Applied, Status.Refused, Status.Ignored, *Status.LastInput));
			}
			if (TestEqual(TEXT("Both messages arrived and were carried out"), State->Carried.Num(), 2))
			{
				TestEqual(TEXT("Playback 2 level"), State->Carried[0].Playback, 1);
				TestEqual(TEXT("Level value"), State->Carried[0].Value, 0.75f);
				TestEqual(TEXT("GO from an int argument"), State->Carried[1].Action, EStageRemoteAction::CueGo);
			}
			State->Step = 1;
			State->Deadline = FPlatformTime::Seconds() + 5.0;
			return false;
		case 1:
			if (!State->FeedbackAddresses.Contains(TEXT("/stagecraft/master/audio")) && !bTimedOut)
			{
				return false;
			}
			TestTrue(TEXT("Feedback: the active cue"), State->FeedbackAddresses.Contains(TEXT("/stagecraft/cue/active")));
			TestTrue(TEXT("Feedback: playback 10 level"), State->FeedbackAddresses.Contains(TEXT("/stagecraft/playback/10/level")));
			TestTrue(TEXT("Feedback: the Grand Master"), State->FeedbackAddresses.Contains(TEXT("/stagecraft/master/grand")));
			TestTrue(TEXT("Feedback: Blackout"), State->FeedbackAddresses.Contains(TEXT("/stagecraft/master/blackout")));
			TestEqual(TEXT("The new sender got each value once"), State->FeedbackAddresses.Num(), 2 + StagePlaybackRules::Count + 2);
			State->FirstFeedback = State->FeedbackAddresses.Num();
			// Nothing changes on the desk (the test handler does not touch the show): the next message sends no feedback.
			State->Send(TEXT("/stagecraft/cue/back"), {});
			State->Step = 2;
			State->Deadline = FPlatformTime::Seconds() + 5.0;
			return false;
		case 2:
			if (State->Carried.Num() < 3 && !bTimedOut)
			{
				return false;
			}
			TestEqual(TEXT("BACK carried out"), State->Carried.Num(), 3);
			// "This computer": a message that reaches the port from this PC's network address (as one from another machine on the
			// network would) is not dispatched. Skipped on a PC without a network address.
			{
				bool bCanBind = false;
				uint32 LanIp = 0;
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLocalHostAddr(*GLog, bCanBind)->GetIp(LanIp);
				State->bLanChecked = LanIp != 0 && !FIPv4Address(LanIp).IsLoopbackAddress();
				if (State->bLanChecked)
				{
					State->LanSender.Reset(UOSCManager::CreateOSCClient(FIPv4Address(LanIp).ToString(), FLoopbackState::InputPort, FString(), GetTransientPackage()));
					FOSCMessage FromLan;
					FromLan.SetAddress(FOSCAddress(TEXT("/stagecraft/cue/go")));
					State->LanSender->SendOSCMessage(FromLan);
				}
			}
			State->Step = 3;
			State->Deadline = FPlatformTime::Seconds() + 0.5;
			return false;
		default:
			if (!bTimedOut)
			{
				return false;
			}
			TestEqual(TEXT("Only changes are sent"), State->FeedbackAddresses.Num(), State->FirstFeedback);
			if (State->bLanChecked)
			{
				TestEqual(TEXT("This computer: nothing from the network address"), State->Carried.Num(), 3);
			}
			else
			{
				AddInfo(TEXT("No network address on this PC: the allow-list check was skipped."));
			}
			State->Close();
			return true;
		}
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
