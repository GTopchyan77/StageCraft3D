// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for MIDI and OSC remote control (Docs/ADR/0035): the OSC address space, MIDI resolution and learn, the frame queue, settings
// sanitizing, the subsystem with a test command handler, the GameMode rule, and a real OSC round trip on loopback (server, feedback).
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
	if (TestEqual(TEXT("Only the valid, newest binding of CC 7 is kept"), Dirty.MidiBindings.Num(), 1))
	{
		TestEqual(TEXT("The later binding wins"), Dirty.MidiBindings[0].Action, EStageRemoteAction::AudioMaster);
	}
	TestFalse(TEXT("Sanitizing is stable"), StageRemote::Sanitize(Dirty));

	FStageRemoteSettings NoFeedback;
	NoFeedback.OscFeedbackPort = 0;
	TestFalse(TEXT("Feedback port 0 means off and is kept"), StageRemote::Sanitize(NoFeedback));

	FStageRemoteSettings EmptyCustom;
	EmptyCustom.bCustomMidiMap = true;
	TestEqual(TEXT("A custom empty map is empty"), StageRemote::EffectiveMidiBindings(EmptyCustom).Num(), 0);

	FStageRemoteSettings Long;
	Long.MidiInputName = FString::ChrN(500, TEXT('m'));
	StageRemote::Sanitize(Long);
	TestEqual(TEXT("Device name cut"), Long.MidiInputName.Len(), StageRemote::MaxDeviceNameLength);
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
