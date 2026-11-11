// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageRemoteControlSubsystem.h"

#include "Engine/World.h"
#include "MIDIDeviceInputController.h"
#include "MIDIDeviceManager.h"
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

	// The user left OSC or MIDI on last time: this stage opens them again. A port or device that is gone shows in the status.
	FText Error;
	OpenInputs(Error);
	BroadcastStatus();
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
	if (UserSettings->SetRemoteSettings(Settings))
	{
		UserSettings->SaveSettings();
	}

	CloseInputs();
	OutError = FText::GetEmpty();
	OpenInputs(OutError);
	BroadcastStatus();
	return OutError.IsEmpty();
}

TArray<FString> UStageRemoteControlSubsystem::FindMidiInputs()
{
	TArray<FMIDIDeviceInfo> Inputs;
	TArray<FMIDIDeviceInfo> Outputs;
	UMIDIDeviceManager::FindAllMIDIDeviceInfo(Inputs, Outputs);

	TArray<FString> Names;
	Names.Reserve(Inputs.Num());
	for (const FMIDIDeviceInfo& Input : Inputs)
	{
		Names.Add(Input.DeviceName);
	}
	return Names;
}

bool UStageRemoteControlSubsystem::StartMidiLearn(EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber, FText& OutError)
{
	if (Action == EStageRemoteAction::None)
	{
		LearnTemplate.Reset();
		Status.Learning = EStageRemoteAction::None;
		BroadcastStatus();
		return true;
	}
	if (!MidiInput)
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
}

void UStageRemoteControlSubsystem::CloseInputs()
{
	CloseOsc();
	CloseMidi();
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
	if (Settings.OscFeedbackPort > 0)
	{
		BindShow();
	}
	UE_LOG(LogStageCraft, Log, TEXT("Remote: OSC listening on %s (feedback port %d)."), *Status.OscEndpoint, Settings.OscFeedbackPort);
	return true;
}

void UStageRemoteControlSubsystem::CloseOsc()
{
	UnbindShow();
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
	if (Settings.MidiInputName.IsEmpty())
	{
		return true;
	}

	// By name, every time: a controller plugged into another USB port gets another ID but keeps its name.
	TArray<FMIDIDeviceInfo> Inputs;
	TArray<FMIDIDeviceInfo> Outputs;
	UMIDIDeviceManager::FindAllMIDIDeviceInfo(Inputs, Outputs);
	const FMIDIDeviceInfo* Found = Inputs.FindByPredicate([&Settings](const FMIDIDeviceInfo& Info) { return Info.DeviceName == Settings.MidiInputName; });
	if (!Found)
	{
		OutError = FText::Format(LOCTEXT("MidiMissing", "The MIDI input \"{0}\" is not connected."), FText::FromString(Settings.MidiInputName));
		return false;
	}
	if (Found->bIsAlreadyInUse)
	{
		OutError = FText::Format(LOCTEXT("MidiInUse", "The MIDI input \"{0}\" is in use by another program."), FText::FromString(Settings.MidiInputName));
		return false;
	}

	UMIDIDeviceInputController* Input = UMIDIDeviceManager::CreateMIDIDeviceInputController(Found->DeviceID, StageRemoteControl::MidiBufferSize);
	if (!Input)
	{
		OutError = FText::Format(LOCTEXT("MidiOpenFailed", "The MIDI input \"{0}\" could not be opened."), FText::FromString(Settings.MidiInputName));
		return false;
	}
	MidiInput = Input;
	MidiEventHandle = MidiInput->OnMIDIRawEvent.AddUObject(this, &ThisClass::HandleMidiEvent);
	Status.bMidiOpen = true;
	Status.MidiDevice = Settings.MidiInputName;
	UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI input \"%s\" open."), *Settings.MidiInputName);
	return true;
}

void UStageRemoteControlSubsystem::CloseMidi()
{
	if (MidiInput)
	{
		MidiInput->OnMIDIRawEvent.Remove(MidiEventHandle);
		MidiInput->ShutdownDevice();
		UE_LOG(LogStageCraft, Log, TEXT("Remote: MIDI input \"%s\" closed."), *Status.MidiDevice);
	}
	MidiEventHandle.Reset();
	MidiInput = nullptr;
	LearnTemplate.Reset();
	Status.Learning = EStageRemoteAction::None;
	Status.bMidiOpen = false;
	Status.MidiDevice.Reset();
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

	if (LearnTemplate.IsSet())
	{
		FStageMidiBinding Binding = LearnTemplate.GetValue();
		if (StageRemote::LearnFromMidi(Type, Channel, Data1, Data2, Binding))
		{
			// The learned press is the binding, not a command. The map becomes the user's from the one in force.
			FStageRemoteSettings Settings = GetSettings();
			if (!Settings.bCustomMidiMap)
			{
				Settings.MidiBindings = StageRemote::DefaultMidiBindings();
				Settings.bCustomMidiMap = true;
			}
			Settings.MidiBindings.Add(Binding);
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
	if (!World || !BoundShow.IsValid() || FeedbackHost.IsEmpty() || FeedbackTimer.IsValid())
	{
		return;
	}
	// Once per frame at most, however many changes the frame brings.
	FeedbackTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::SendFeedback));
}

void UStageRemoteControlSubsystem::SendFeedback()
{
	FeedbackTimer.Invalidate();
	const UShowControlSubsystem* Show = BoundShow.Get();
	const int32 Port = GetSettings().OscFeedbackPort;
	if (!Show || FeedbackHost.IsEmpty() || Port <= 0)
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
	SendIfChanged(SentFeedback.ActiveCue, Show->GetActiveCueNumber(), TEXT("/stagecraft/cue/active"));
	for (int32 Index = 0; Index < SentFeedback.Levels.Num(); ++Index)
	{
		SendIfChanged(SentFeedback.Levels[Index], Show->GetPlaybackLevel(Index), StageRemote::OscAddressFor(EStageRemoteAction::PlaybackLevel, Index));
	}
	SendIfChanged(SentFeedback.GrandMaster, Show->GetGrandMaster(), StageRemote::OscAddressFor(EStageRemoteAction::GrandMaster, 0));
	SendIfChanged(SentFeedback.Blackout, Show->IsBlackout() ? 1.f : 0.f, StageRemote::OscAddressFor(EStageRemoteAction::Blackout, 0));
	SendIfChanged(SentFeedback.AudioMaster, Show->GetAudioMaster(), StageRemote::OscAddressFor(EStageRemoteAction::AudioMaster, 0));
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
