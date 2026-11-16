// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageRemoteControlSubsystem.h"

#include "Engine/World.h"
#include "MIDIDeviceInputController.h"
#include "MIDIDeviceManager.h"
#include "MIDIDeviceOutputController.h"
#include "ModularSceneBuilder.h"
#include "OSCClient.h"
#include "OSCManager.h"
#include "OSCMessage.h"
#include "OSCServer.h"
#include "Remote/StageRemoteMapping.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/ShowControlTypes.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageRemoteControlSubsystem)

#define LOCTEXT_NAMESPACE "StageRemoteControl"

namespace StageRemoteControl
{
	/**
	 * The socket always listens on every card: the engine's OSC receiver replaces a loopback address with this PC's LAN address
	 * (FServerReceiver::InitSocket, SetAsLocalHostIfLoopback), which would both miss 127.0.0.1 and be reachable from the network.
	 * "This computer" is enforced with the server's allow-list instead (Docs/ADR/0035 §4).
	 */
	const TCHAR* ListenAddress = TEXT("0.0.0.0");
	const TCHAR* LoopbackAddress = TEXT("127.0.0.1");

	/** Large enough for a burst of fader moves between two frames; PortMidi drops what does not fit. */
	constexpr int32 MidiBufferSize = 1024;

	/** Feedback is sent when a value moved at least this much (a fader's float noise is not news). */
	constexpr float FeedbackTolerance = 0.001f;

	/** Sends Messages to Output; returns how many were sent. */
	int32 SendMidiMessages(UMIDIDeviceOutputController* Output, TConstArrayView<StageRemote::FMidiOutMessage> Messages)
	{
		if (!Output)
		{
			return 0;
		}
		for (const StageRemote::FMidiOutMessage& Message : Messages)
		{
			// Engine finding (UE 5.8): the output builds the status byte as (type << 4) | channel, so it takes channels 0–15, unlike the
			// input, which reports 1–16 (UMIDIDeviceOutputController::SendMIDIEvent). Channel 16 sent as 16 would corrupt the message type.
			const int32 WireChannel = Message.Channel - 1;
			if (Message.Type == StageRemote::MidiNoteOn)
			{
				Output->SendMIDINoteOn(WireChannel, Message.Data1, Message.Data2);
			}
			else
			{
				Output->SendMIDIControlChange(WireChannel, Message.Data1, Message.Data2);
			}
		}
		return Messages.Num();
	}

	/** The first argument of an OSC message as a number; false for a type that is not one (string, blob…). */
	bool ReadFirstNumber(const FOSCMessage& Message, TOptional<float>& OutValue)
	{
		const TArray<UE::OSC::FOSCData>& Arguments = Message.GetArgumentsChecked();
		if (Arguments.IsEmpty())
		{
			OutValue.Reset();
			return true;
		}
		const UE::OSC::FOSCData& First = Arguments[0];
		if (First.IsFloat())
		{
			OutValue = First.GetFloat();
		}
		else if (First.IsInt32())
		{
			OutValue = static_cast<float>(First.GetInt32());
		}
		else if (First.IsInt64())
		{
			OutValue = static_cast<float>(First.GetInt64());
		}
		else if (First.IsDouble())
		{
			OutValue = static_cast<float>(First.GetDouble());
		}
		else if (First.IsBool())
		{
			OutValue = First.GetBool() ? 1.f : 0.f;
		}
		else
		{
			return false;
		}
		return true;
	}
}

bool UStageRemoteControlSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Like the show it drives: play sessions only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageRemoteControlSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UShowControlSubsystem>();
	SentFeedback.Levels.SetNum(StagePlaybackRules::Count);
}

void UStageRemoteControlSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlushTimer);
		World->GetTimerManager().ClearTimer(FeedbackTimer);
		World->GetTimerManager().ClearTimer(StatusTimer);
		World->GetTimerManager().ClearTimer(MidiSettleTimer);
	}
	CloseInputs();
	PendingCommands.Reset();
	CommandHandler.Unbind();
	OnStatusChanged.Clear();
	Super::Deinitialize();
}

void UStageRemoteControlSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// The user left OSC or MIDI on last time: they open again with the controller's first SetCapabilityAllowed, once the licence has
	// been judged, so a Free licence never opens them (the controller's BeginPlay follows in this same UWorld::BeginPlay).
	bRestorePending = true;
	BroadcastStatus();
}

bool UStageRemoteControlSubsystem::SetCapabilityAllowed(bool bAllowed, const FText& Reason)
{
	CapabilityReason = Reason.IsEmpty() ? LOCTEXT("CapabilityRefused", "MIDI and OSC control is not available here.") : Reason;
	if (bAllowed == bCapabilityAllowed && !bRestorePending)
	{
		// Still refused, maybe for another reason (a session rule now, the licence before): the warning follows the latest one.
		if (!bAllowed && !Status.LastProblem.IsEmpty() && !Status.LastProblem.EqualTo(CapabilityReason))
		{
			SetProblem(CapabilityReason);
			BroadcastStatus();
		}
		return false;
	}
	bRestorePending = false;
	bCapabilityAllowed = bAllowed;

	const bool bWasOpen = Status.bOscListening || Status.bMidiOpen || Status.bMidiOutputOpen;
	CloseInputs();
	if (!bAllowed)
	{
		// Commands already queued would be refused one by one; nothing from before the lapse is carried out.
		PendingCommands.Reset();
		if (bWasOpen)
		{
			UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI and OSC closed (%s); the settings are kept."), *CapabilityReason.ToString());
		}
	}
	// Opens the saved inputs, or says why they stay closed.
	FText Error;
	OpenInputs(Error);
	BroadcastStatus();
	return true;
}

const FStageRemoteSettings& UStageRemoteControlSubsystem::GetSettings() const
{
	static const FStageRemoteSettings Defaults;
	const UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	return UserSettings ? UserSettings->GetRemoteSettings() : Defaults;
}

bool UStageRemoteControlSubsystem::ApplySettings(const FStageRemoteSettings& Settings, FText& OutError)
{
	UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	if (!UserSettings)
	{
		OutError = LOCTEXT("NoSettings", "The settings of this PC are not available.");
		return false;
	}
	const FStageRemoteSettings Before = UserSettings->GetRemoteSettings();
	if (UserSettings->SetRemoteSettings(Settings))
	{
		UserSettings->SaveSettings();
	}
	const FStageRemoteSettings& After = UserSettings->GetRemoteSettings();
	if (StageRemote::HasSameInputs(Before, After) && !(Before == After))
	{
		// A map edit (Preferences › MIDI MAP): closing the devices would drop the controller and a learn for nothing.
		OutError = FText::GetEmpty();
		RestartMidiFeedback();
		BroadcastStatus();
		return true;
	}

	// An explicit change replaces the restore of the saved settings.
	bRestorePending = false;
	CloseInputs();
	OutError = FText::GetEmpty();
	OpenInputs(OutError);
	BroadcastStatus();
	return OutError.IsEmpty();
}

TArray<FString> UStageRemoteControlSubsystem::FindMidiInputs()
{
	TArray<FString> Inputs;
	TArray<FString> Outputs;
	FindMidiDevices(Inputs, Outputs);
	return Inputs;
}

void UStageRemoteControlSubsystem::FindMidiDevices(TArray<FString>& OutInputs, TArray<FString>& OutOutputs)
{
	TArray<FMIDIDeviceInfo> Inputs;
	TArray<FMIDIDeviceInfo> Outputs;
	UMIDIDeviceManager::FindAllMIDIDeviceInfo(Inputs, Outputs);

	OutInputs.Reset(Inputs.Num());
	for (const FMIDIDeviceInfo& Input : Inputs)
	{
		OutInputs.Add(Input.DeviceName);
	}
	OutOutputs.Reset(Outputs.Num());
	for (const FMIDIDeviceInfo& Output : Outputs)
	{
		OutOutputs.Add(Output.DeviceName);
	}
}

bool UStageRemoteControlSubsystem::StartMidiLearn(EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber, FText& OutError)
{
	if (Action == EStageRemoteAction::None)
	{
		LearnTemplate.Reset();
		Status.Learning = EStageRemoteAction::None;
		Status.LearningPlayback = 0;
		BroadcastStatus();
		return true;
	}
	if (!CheckMidiDevices() || !MidiInput)
	{
		OutError = LOCTEXT("LearnNoMidi", "Choose a MIDI input first.");
		return false;
	}

	FStageMidiBinding Template;
	Template.Action = Action;
	Template.Playback = PlaybackNumber;
	Template.CueNumber = CueNumber;
	if (!StageRemote::IsValidBinding(Template))
	{
		OutError = LOCTEXT("LearnBadTarget", "That control cannot be learned: check the playback number (1–10).");
		return false;
	}
	LearnTemplate = Template;
	Status.Learning = Action;
	Status.LearningPlayback = StageRemote::UsesPlayback(Action) ? PlaybackNumber : 0;
	BroadcastStatus();
	return true;
}

void UStageRemoteControlSubsystem::ClearCommandHandler(const UObject* Owner)
{
	if (Owner && CommandHandler.IsBoundToObject(Owner))
	{
		CommandHandler.Unbind();
	}
}

// --- Opening and closing ---

void UStageRemoteControlSubsystem::OpenInputs(FText& OutError)
{
	SetProblem(FText::GetEmpty());
	if (!bCapabilityAllowed)
	{
		// Only a choice the user made is worth a warning; with both off there is nothing being held back.
		if (StageRemote::HasInputOn(GetSettings()))
		{
			OutError = CapabilityReason;
			SetProblem(OutError);
		}
		return;
	}
	FText OscError;
	FText MidiError;
	OpenOsc(OscError);
	OpenMidi(MidiError);

	// Both may fail; the user sees both.
	if (!OscError.IsEmpty() && !MidiError.IsEmpty())
	{
		OutError = FText::Format(LOCTEXT("BothFailed", "{0} {1}"), OscError, MidiError);
	}
	else
	{
		OutError = OscError.IsEmpty() ? MidiError : OscError;
	}
	SetProblem(OutError);
	UpdateShowBinding();
	RestartMidiFeedback();
}

void UStageRemoteControlSubsystem::CloseInputs()
{
	UnbindShow();
	CloseOsc();
	CloseMidi();
}

void UStageRemoteControlSubsystem::UpdateShowBinding()
{
	const bool bOscFeedback = OscServer && GetSettings().OscFeedbackPort > 0;
	if (bOscFeedback || MidiOutput)
	{
		BindShow();
	}
	else
	{
		UnbindShow();
	}
}

void UStageRemoteControlSubsystem::RestartMidiFeedback()
{
	if (MidiOutput)
	{
		// The previous map's lights and faders that no binding answers any more would otherwise stay as they were.
		TArray<StageRemote::FMidiOutMessage> Messages;
		StageRemote::CollectUnboundOff(StageRemote::EffectiveMidiBindings(GetSettings()), MidiShown, MidiClock(), Messages);
		Status.MidiSent += StageRemoteControl::SendMidiMessages(MidiOutput, Messages);
	}
	MidiShown.Reset();
	ScheduleFeedback();
}

void UStageRemoteControlSubsystem::ArmMidiSettle(double Delay)
{
	UWorld* World = GetWorld();
	if (!World || !MidiOutput)
	{
		return;
	}
	// One timer, pushed back by every move: it fires once the last hold has ended.
	constexpr double Margin = 0.02;
	World->GetTimerManager().SetTimer(MidiSettleTimer, FTimerDelegate::CreateUObject(this, &ThisClass::ScheduleFeedback),
		static_cast<float>(FMath::Max(Delay, 0.0) + Margin), /*bLoop*/ false);
}

double UStageRemoteControlSubsystem::MidiClock() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetRealTimeSeconds() : 0.0;
}

bool UStageRemoteControlSubsystem::CheckMidiDevices()
{
	const bool bInputLost = Status.bMidiOpen && !IsValid(MidiInput);
	const bool bOutputLost = Status.bMidiOutputOpen && !IsValid(MidiOutput);
	if (!bInputLost && !bOutputLost)
	{
		return true;
	}
	CloseMidi();
	SetProblem(LOCTEXT("MidiLost", "The MIDI controller was disconnected. Plug it in, then choose it again."));
	UE_LOG(LogStageCraft, Warning, TEXT("Remote: a MIDI device was disconnected (input lost: %d, output lost: %d)."), bInputLost, bOutputLost);
	BroadcastStatus();
	return false;
}

bool UStageRemoteControlSubsystem::OpenOsc(FText& OutError)
{
	const FStageRemoteSettings& Settings = GetSettings();
	if (Settings.OscScope == EStageOscScope::Off)
	{
		return true;
	}

	const FString Address = StageRemoteControl::ListenAddress;
	// An empty server name: the engine makes a unique one, so reopening never meets the previous server's object.
	UOSCServer* Server = UOSCManager::CreateOSCServer(Address, Settings.OscPort, /*bMulticastLoopback*/ false, /*bStartListening*/ true, FString(), this);
	if (Server && Settings.OscScope == EStageOscScope::ThisComputer)
	{
		// Packets are filtered when the server pumps them on the game thread, so nothing from another machine is dispatched even
		// between here and the first pump.
		Server->SetAllowlistClientsEnabled(true);
		Server->AddAllowlistedClient(StageRemoteControl::LoopbackAddress, 0);
	}
	if (!Server || !Server->IsActive())
	{
		if (Server)
		{
			Server->Stop();
		}
		OutError = FText::Format(LOCTEXT("OscPortBusy", "OSC could not listen on port {0}: another program may be using it."), FText::AsNumber(Settings.OscPort, &FNumberFormattingOptions::DefaultNoGrouping()));
		UE_LOG(LogStageCraft, Warning, TEXT("Remote: OSC could not listen on %s:%d."), *Address, Settings.OscPort);
		return false;
	}

	OscServer = Server;
	OscMessageHandle = OscServer->OnOscMessageReceivedNative.AddUObject(this, &ThisClass::HandleOscMessage);
	Status.bOscListening = true;
	Status.OscEndpoint = Settings.OscScope == EStageOscScope::ThisComputer
		? FString::Printf(TEXT("port %d, this PC only"), Settings.OscPort)
		: FString::Printf(TEXT("port %d, every network"), Settings.OscPort);
	UE_LOG(LogStageCraft, Log, TEXT("Remote: OSC listening on %s (feedback port %d)."), *Status.OscEndpoint, Settings.OscFeedbackPort);
	return true;
}

void UStageRemoteControlSubsystem::CloseOsc()
{
	if (OscServer)
	{
		OscServer->OnOscMessageReceivedNative.Remove(OscMessageHandle);
		OscServer->Stop();
		UE_LOG(LogStageCraft, Log, TEXT("Remote: OSC closed (%s)."), *Status.OscEndpoint);
	}
	OscMessageHandle.Reset();
	OscServer = nullptr;
	OscClient = nullptr;
	FeedbackHost.Reset();
	Status.bOscListening = false;
	Status.OscEndpoint.Reset();
}

bool UStageRemoteControlSubsystem::OpenMidi(FText& OutError)
{
	const FStageRemoteSettings& Settings = GetSettings();
	if (Settings.MidiInputName.IsEmpty() && Settings.MidiOutputName.IsEmpty())
	{
		return true;
	}

	// By name, every time: a controller plugged into another USB port gets another ID but keeps its name. One listing for both,
	// because each one restarts the engine's MIDI layer.
	TArray<FMIDIDeviceInfo> Inputs;
	TArray<FMIDIDeviceInfo> Outputs;
	UMIDIDeviceManager::FindAllMIDIDeviceInfo(Inputs, Outputs);
	TArray<FText> Errors;

	if (!Settings.MidiInputName.IsEmpty())
	{
		const FMIDIDeviceInfo* Found = Inputs.FindByPredicate([&Settings](const FMIDIDeviceInfo& Info) { return Info.DeviceName == Settings.MidiInputName; });
		UMIDIDeviceInputController* Input = Found && !Found->bIsAlreadyInUse
			? UMIDIDeviceManager::CreateMIDIDeviceInputController(Found->DeviceID, StageRemoteControl::MidiBufferSize) : nullptr;
		if (!Found)
		{
			Errors.Add(FText::Format(LOCTEXT("MidiMissing", "The MIDI input \"{0}\" is not connected."), FText::FromString(Settings.MidiInputName)));
		}
		else if (Found->bIsAlreadyInUse)
		{
			Errors.Add(FText::Format(LOCTEXT("MidiInUse", "The MIDI input \"{0}\" is in use by another program."), FText::FromString(Settings.MidiInputName)));
		}
		else if (!Input)
		{
			Errors.Add(FText::Format(LOCTEXT("MidiOpenFailed", "The MIDI input \"{0}\" could not be opened."), FText::FromString(Settings.MidiInputName)));
		}
		else
		{
			MidiInput = Input;
			MidiEventHandle = MidiInput->OnMIDIRawEvent.AddUObject(this, &ThisClass::HandleMidiEvent);
			Status.bMidiOpen = true;
			Status.MidiDevice = Settings.MidiInputName;
			UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI input \"%s\" open."), *Settings.MidiInputName);
		}
	}

	if (!Settings.MidiOutputName.IsEmpty())
	{
		const FMIDIDeviceInfo* Found = Outputs.FindByPredicate([&Settings](const FMIDIDeviceInfo& Info) { return Info.DeviceName == Settings.MidiOutputName; });
		UMIDIDeviceOutputController* Output = Found && !Found->bIsAlreadyInUse ? UMIDIDeviceManager::CreateMIDIDeviceOutputController(Found->DeviceID) : nullptr;
		if (!Found)
		{
			Errors.Add(FText::Format(LOCTEXT("MidiOutMissing", "The MIDI output \"{0}\" is not connected."), FText::FromString(Settings.MidiOutputName)));
		}
		else if (Found->bIsAlreadyInUse)
		{
			Errors.Add(FText::Format(LOCTEXT("MidiOutInUse", "The MIDI output \"{0}\" is in use by another program."), FText::FromString(Settings.MidiOutputName)));
		}
		else if (!Output)
		{
			Errors.Add(FText::Format(LOCTEXT("MidiOutOpenFailed", "The MIDI output \"{0}\" could not be opened."), FText::FromString(Settings.MidiOutputName)));
		}
		else
		{
			MidiOutput = Output;
			Status.bMidiOutputOpen = true;
			Status.MidiOutputDevice = Settings.MidiOutputName;
			UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI output \"%s\" open (feedback)."), *Settings.MidiOutputName);
		}
	}

	OutError = FText::Join(FText::FromString(TEXT(" ")), Errors);
	return Errors.IsEmpty();
}

void UStageRemoteControlSubsystem::CloseMidi()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(MidiSettleTimer);
	}
	// IsValid: the engine may have dropped a device already (CheckMidiDevices); its object is garbage but not yet collected.
	if (IsValid(MidiInput))
	{
		MidiInput->OnMIDIRawEvent.Remove(MidiEventHandle);
		MidiInput->ShutdownDevice();
		// Engine finding (UE 5.8): every device listing (UMIDIDeviceManager::FindAllMIDIDeviceInfo) restarts each live controller object,
		// a shut-down one too, until garbage collection removes it. It would take the device back, and reopening it here would fail as
		// "in use". Marked as garbage, the listing skips it, as the engine does with a controller it could not restart.
		MidiInput->MarkAsGarbage();
		UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI input \"%s\" closed."), *Status.MidiDevice);
	}
	MidiEventHandle.Reset();
	MidiInput = nullptr;
	LearnTemplate.Reset();
	Status.Learning = EStageRemoteAction::None;
	Status.LearningPlayback = 0;
	Status.bMidiOpen = false;
	Status.MidiDevice.Reset();

	if (IsValid(MidiOutput))
	{
		MidiOutput->ShutdownDevice();
		MidiOutput->MarkAsGarbage();
		UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI output \"%s\" closed."), *Status.MidiOutputDevice);
	}
	MidiOutput = nullptr;
	MidiShown = FStageMidiFeedbackMemory();
	Status.bMidiOutputOpen = false;
	Status.MidiOutputDevice.Reset();
}

// --- Input ---

void UStageRemoteControlSubsystem::HandleOscMessage(const FOSCMessage& Message, const FString& IPAddress, uint16 Port)
{
	TOptional<float> Value;
	const FString Address = Message.GetAddress().GetFullPath();
	if (!StageRemoteControl::ReadFirstNumber(Message, Value))
	{
		++Status.Received;
		++Status.Ignored;
		Status.LastInput = FString::Printf(TEXT("OSC %s (not a number)"), *Address.Left(StageRemote::MaxOscAddressLength));
		MarkStatusDirty();
		return;
	}
	ReceiveOsc(Address, Value, IPAddress);
}

void UStageRemoteControlSubsystem::HandleMidiEvent(UMIDIDeviceInputController* Controller, int32 Timestamp, int32 Type, int32 Channel, int32 Data1, int32 Data2)
{
	ReceiveMidi(Type, Channel, Data1, Data2);
}

void UStageRemoteControlSubsystem::ReceiveOsc(const FString& Address, TOptional<float> Value, const FString& Sender)
{
	++Status.Received;
	Status.LastInput = Value.IsSet()
		? FString::Printf(TEXT("OSC %s %.2f"), *Address.Left(StageRemote::MaxOscAddressLength), Value.GetValue())
		: FString::Printf(TEXT("OSC %s"), *Address.Left(StageRemote::MaxOscAddressLength));

	// A new sender gets the whole state once, then only changes.
	if (!Sender.IsEmpty() && Sender != FeedbackHost)
	{
		FeedbackHost = Sender;
		SentFeedback = FSentFeedback();
		SentFeedback.Levels.SetNum(StagePlaybackRules::Count);
		ScheduleFeedback();
	}

	FStageRemoteCommand Command;
	if (StageRemote::ParseOsc(Address, Value, Command))
	{
		Enqueue(Command);
	}
	else
	{
		++Status.Ignored;
	}
	MarkStatusDirty();
}

void UStageRemoteControlSubsystem::ReceiveMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2)
{
	// System messages (clock, active sensing) arrive many times a second and are never controls: not even counted.
	if (Type < StageRemote::MidiNoteOff || Type > 0xE)
	{
		return;
	}
	++Status.Received;
	Status.LastInput = StageRemote::DescribeMidi(Type, Channel, Data1, Data2);
	const double Now = MidiClock();
	if (MidiOutput && StageRemote::IsMidiEcho(Type, Channel, Data1, Data2, MidiShown, Now))
	{
		// Our own feedback coming back (a virtual port or a controller's MIDI thru): a lit GO light must not fire GO again.
		++Status.Ignored;
		Status.LastInput += TEXT(" (echo of feedback)");
		MarkStatusDirty();
		return;
	}
	if (StageRemote::NoteMidiInput(Type, Channel, Data1, Data2, StageRemote::EffectiveMidiBindings(GetSettings()), MidiShown, Now))
	{
		ArmMidiSettle(StageRemote::MidiHoldSeconds);
	}

	if (LearnTemplate.IsSet())
	{
		FStageMidiBinding Binding = LearnTemplate.GetValue();
		if (StageRemote::LearnFromMidi(Type, Channel, Data1, Data2, Binding))
		{
			// The learned press is the binding, not a command. The control now drives this target only, and the target only this control.
			FStageRemoteSettings Settings = GetSettings();
			StageRemote::AssignLearned(Settings, Binding);
			if (UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get())
			{
				if (UserSettings->SetRemoteSettings(Settings))
				{
					UserSettings->SaveSettings();
				}
			}
			UE_LOG(LogStageCraft, Log, TEXT("Remote: learned %s for %s."), *Status.LastInput, *StageRemote::DescribeAction(Binding.Action, Binding.Playback).ToString());
			LearnTemplate.Reset();
			Status.Learning = EStageRemoteAction::None;
			Status.LearningPlayback = 0;
			RestartMidiFeedback();
			BroadcastStatus();
			return;
		}
	}

	TArray<FStageRemoteCommand> Resolved;
	if (StageRemote::ResolveMidi(Type, Channel, Data1, Data2, StageRemote::EffectiveMidiBindings(GetSettings()), Resolved) == 0)
	{
		++Status.Ignored;
	}
	for (const FStageRemoteCommand& Command : Resolved)
	{
		Enqueue(Command);
	}
	MarkStatusDirty();
}

void UStageRemoteControlSubsystem::Enqueue(const FStageRemoteCommand& Command)
{
	if (!StageRemote::Enqueue(PendingCommands, Command))
	{
		++Status.Dropped;
		return;
	}
	UWorld* World = GetWorld();
	if (World && !FlushTimer.IsValid())
	{
		FlushTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::FlushCommands));
	}
}

void UStageRemoteControlSubsystem::FlushCommands()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlushTimer);
	}
	FlushTimer.Invalidate();

	// Taken first: a command's side effects may queue more (they wait for the next frame).
	const TArray<FStageRemoteCommand> Commands = MoveTemp(PendingCommands);
	PendingCommands.Reset();
	for (const FStageRemoteCommand& Command : Commands)
	{
		const FStageEconomyResultInfo Verdict = Dispatch(Command);
		if (Verdict.IsSuccess())
		{
			++Status.Applied;
		}
		else
		{
			++Status.Refused;
			Status.LastRefusal = Verdict.Message;
		}
	}
	if (!Commands.IsEmpty())
	{
		MarkStatusDirty();
	}
}

FStageEconomyResultInfo UStageRemoteControlSubsystem::Dispatch(const FStageRemoteCommand& Command)
{
	if (!CommandHandler.IsBound())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoHandler", "There is no stage to control yet."));
	}
	return CommandHandler.Execute(Command);
}

// --- OSC feedback ---

void UStageRemoteControlSubsystem::BindShow()
{
	UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	if (!Show || BoundShow.Get() == Show)
	{
		return;
	}
	Show->OnCueStarted.AddUniqueDynamic(this, &ThisClass::HandleShowCueStarted);
	Show->OnDeskStateChanged.AddUniqueDynamic(this, &ThisClass::HandleShowDeskChanged);
	BoundShow = Show;
}

void UStageRemoteControlSubsystem::UnbindShow()
{
	if (UShowControlSubsystem* Show = BoundShow.Get())
	{
		Show->OnCueStarted.RemoveDynamic(this, &ThisClass::HandleShowCueStarted);
		Show->OnDeskStateChanged.RemoveDynamic(this, &ThisClass::HandleShowDeskChanged);
	}
	BoundShow.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FeedbackTimer);
	}
}

void UStageRemoteControlSubsystem::HandleShowCueStarted(float CueNumber)
{
	ScheduleFeedback();
}

void UStageRemoteControlSubsystem::HandleShowDeskChanged()
{
	ScheduleFeedback();
}

void UStageRemoteControlSubsystem::ScheduleFeedback()
{
	UWorld* World = GetWorld();
	const bool bSomeoneListens = MidiOutput || (OscServer && !FeedbackHost.IsEmpty());
	if (!World || !BoundShow.IsValid() || !bSomeoneListens || FeedbackTimer.IsValid())
	{
		return;
	}
	// Once per frame at most, however many changes the frame brings.
	FeedbackTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::SendFeedback));
}

void UStageRemoteControlSubsystem::SendFeedback()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FeedbackTimer);
	}
	FeedbackTimer.Invalidate();
	if (const UShowControlSubsystem* Show = BoundShow.Get())
	{
		SendOscFeedback(*Show);
		SendMidiFeedback(*Show);
	}
}

void UStageRemoteControlSubsystem::SendMidiFeedback(const UShowControlSubsystem& Show)
{
	if (!MidiOutput || !CheckMidiDevices())
	{
		return;
	}
	StageRemote::FMidiShowState State;
	State.Levels.SetNum(StagePlaybackRules::Count);
	State.Flashing.SetNum(StagePlaybackRules::Count);
	State.Active.SetNum(StagePlaybackRules::Count);
	for (int32 Index = 0; Index < StagePlaybackRules::Count; ++Index)
	{
		State.Levels[Index] = Show.GetPlaybackLevel(Index);
		State.Flashing[Index] = Show.IsPlaybackFlashing(Index);
		State.Active[Index] = Show.IsPlaybackActive(Index);
	}
	State.GrandMaster = Show.GetGrandMaster();
	State.bBlackout = Show.IsBlackout();
	State.AudioMaster = Show.GetAudioMaster();

	TArray<StageRemote::FMidiOutMessage> Messages;
	double HoldUntil = 0.0;
	const double Now = MidiClock();
	StageRemote::CollectMidiFeedback(StageRemote::EffectiveMidiBindings(GetSettings()), State, MidiShown, Now, Messages, HoldUntil);
	if (HoldUntil > 0.0)
	{
		ArmMidiSettle(HoldUntil - Now);
	}
	if (const int32 Sent = StageRemoteControl::SendMidiMessages(MidiOutput, Messages))
	{
		Status.MidiSent += Sent;
		MarkStatusDirty();
	}
}

void UStageRemoteControlSubsystem::SendOscFeedback(const UShowControlSubsystem& Show)
{
	const int32 Port = GetSettings().OscFeedbackPort;
	if (!OscServer || FeedbackHost.IsEmpty() || Port <= 0)
	{
		return;
	}
	if (!OscClient)
	{
		OscClient = UOSCManager::CreateOSCClient(FeedbackHost, Port, FString(), this);
	}
	else
	{
		OscClient->SetSendIPAddress(FeedbackHost, Port);
	}
	if (!OscClient || !OscClient->IsActive())
	{
		return;
	}

	auto SendIfChanged = [this](TOptional<float>& Sent, float Value, const FString& Address)
	{
		if (!Sent.IsSet() || !FMath::IsNearlyEqual(Sent.GetValue(), Value, StageRemoteControl::FeedbackTolerance))
		{
			Sent = Value;
			SendFeedbackValue(Address, Value);
		}
	};
	SendIfChanged(SentFeedback.ActiveCue, Show.GetActiveCueNumber(), TEXT("/stagecraft/cue/active"));
	for (int32 Index = 0; Index < SentFeedback.Levels.Num(); ++Index)
	{
		SendIfChanged(SentFeedback.Levels[Index], Show.GetPlaybackLevel(Index), StageRemote::OscAddressFor(EStageRemoteAction::PlaybackLevel, Index));
	}
	SendIfChanged(SentFeedback.GrandMaster, Show.GetGrandMaster(), StageRemote::OscAddressFor(EStageRemoteAction::GrandMaster, 0));
	SendIfChanged(SentFeedback.Blackout, Show.IsBlackout() ? 1.f : 0.f, StageRemote::OscAddressFor(EStageRemoteAction::Blackout, 0));
	SendIfChanged(SentFeedback.AudioMaster, Show.GetAudioMaster(), StageRemote::OscAddressFor(EStageRemoteAction::AudioMaster, 0));
}

void UStageRemoteControlSubsystem::SendFeedbackValue(const FString& Address, float Value)
{
	FOSCMessage Message;
	Message.SetAddress(FOSCAddress(Address));
	UOSCManager::AddFloat(Message, Value);
	OscClient->SendOSCMessage(Message);
}

// --- Status ---

void UStageRemoteControlSubsystem::MarkStatusDirty()
{
	bStatusDirty = true;
	UWorld* World = GetWorld();
	if (!World || World->GetTimerManager().IsTimerActive(StatusTimer))
	{
		return;
	}
	World->GetTimerManager().SetTimer(StatusTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		if (bStatusDirty)
		{
			BroadcastStatus();
		}
	}), StatusInterval, /*bLoop*/ false);
}

void UStageRemoteControlSubsystem::BroadcastStatus()
{
	bStatusDirty = false;
	OnStatusChanged.Broadcast();
}

void UStageRemoteControlSubsystem::SetProblem(const FText& Problem)
{
	Status.LastProblem = Problem;
}

#undef LOCTEXT_NAMESPACE
