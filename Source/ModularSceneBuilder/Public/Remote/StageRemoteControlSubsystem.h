// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Economy/StageEconomyTypes.h"
#include "Remote/StageRemoteControlTypes.h"
#include "StageRemoteControlSubsystem.generated.h"

/**
 * MIDI and OSC remote control of the stage's desk (Docs/ADR/0035). Owns this PC's OSC server, its feedback client and the MIDI input
 * and output devices for one stage world, like UStageDMXNetworkSubsystem: what the saved settings ask for opens once the local controller
 * has pushed the GameMode's verdict (SetCapabilityAllowed), closes while it is refused, and Deinitialize closes everything. No tick.
 *
 * Flow: the engine plugins deliver on the game thread (MIDIDevice from its module tick, OSC from its packet pump) → ReceiveOsc /
 * ReceiveMidi resolve the input with the pure StageRemote layer → the frame's queue (continuous values coalesced, at most
 * StageRemote::MaxCommandsPerFrame) → once per frame each command goes to the command handler, which the local
 * AModularPlayerController binds (RequestRemoteCommand: GameMode, then the desk's own request). The subsystem never changes the show.
 *
 * Writes the remote settings in UStageCraftUserSettings (only writer); reads the show (UShowControlSubsystem) for OSC and MIDI feedback
 * (Docs/ADR/0035 §6, §12): after a change in the show, once per frame, only the values that changed go out.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageRemoteControlSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Carries out one command; returns the verdict. */
	using FCommandHandler = TDelegate<FStageEconomyResultInfo(const FStageRemoteCommand&)>;

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

public:

	/** The saved settings of this PC, sanitized. */
	const FStageRemoteSettings& GetSettings() const;

	/**
	 * Stores Settings (sanitized and saved) and reopens the inputs with them. Returns false with OutError when OSC or MIDI was asked for
	 * and could not open (port in use, device missing); the settings are kept either way, and the status shows the problem.
	 * A map edit (StageRemote::HasSameInputs) reopens nothing: the devices keep running, a learn keeps waiting, and the MIDI output is
	 * sent the new map's state. Game thread. Callers in play go through AModularPlayerController::RequestSetRemoteControl.
	 */
	bool ApplySettings(const FStageRemoteSettings& Settings, FText& OutError);

	/**
	 * Whether this stage may open MIDI and OSC at all: the GameMode's verdict (EvaluateRemoteControl: the session's rules, then the
	 * StageCraft Pro licence), pushed by the local AModularPlayerController when it starts and whenever the licence changes
	 * (Docs/ADR/0035 §13). While refused, the OSC server and the MIDI devices stay closed, a learn is cancelled and the status shows
	 * Reason; the saved settings are kept, so the inputs open again by themselves once allowed (a renewed licence). The saved settings
	 * open only after the first push, so a Free licence never opens a port or a device, not even before the controller's BeginPlay.
	 * Returns true when the verdict changed (or the first push opened the saved inputs). Game thread.
	 */
	bool SetCapabilityAllowed(bool bAllowed, const FText& Reason);
	bool IsCapabilityAllowed() const { return bCapabilityAllowed; }

	const FStageRemoteStatus& GetStatus() const { return Status; }

	/** Fires after the status changes: at once when an input opens or closes, at most every StatusInterval for traffic counters. */
	FOnStageRemoteStatusChanged OnStatusChanged;

	/** The names of the MIDI input devices on this PC, in the order the system lists them. */
	static TArray<FString> FindMidiInputs();

	/**
	 * The MIDI inputs and outputs on this PC, in the order the system lists them. One call, because each listing restarts the engine's
	 * MIDI layer (UMIDIDeviceManager::FindAllMIDIDeviceInfo re-initializes PortMidi and reconnects open devices).
	 */
	static void FindMidiDevices(TArray<FString>& OutInputs, TArray<FString>& OutOutputs);

	/**
	 * MIDI learn: the next Note On or CC (with a value) on the open MIDI input is bound to Action and saved, replacing any binding of
	 * the same control. PlaybackNumber is 1–10 for playback actions; CueNumber is for CueGoTo. False with OutError when MIDI is not
	 * open or the target is invalid. None cancels.
	 */
	bool StartMidiLearn(EStageRemoteAction Action, int32 PlaybackNumber, float CueNumber, FText& OutError);

	/**
	 * The inputs. The device callbacks call these; tests and the dev console feed them directly. Game thread. Value is the OSC
	 * message's first argument as a number (unset without one); Sender is the address feedback answers to (empty: no feedback).
	 */
	void ReceiveOsc(const FString& Address, TOptional<float> Value, const FString& Sender);
	void ReceiveMidi(int32 Type, int32 Channel, int32 Data1, int32 Data2);

	/** Carries out the queued commands now (normally on the next tick after the first one arrives). */
	void FlushCommands();

	/** Sends the show's changed values to the OSC and MIDI feedback now (normally on the next tick after a change). */
	void SendFeedback();

	/**
	 * Closes the MIDI devices the engine has dropped (a controller unplugged: each device listing marks a controller it cannot restart
	 * as garbage) and says so in the status. Returns false when one was. Called before MIDI is used, and by Preferences after it lists
	 * the devices.
	 */
	bool CheckMidiDevices();

	/**
	 * Who carries out commands: the local AModularPlayerController binds RequestRemoteCommand in its BeginPlay (tests bind their own).
	 * Without one, commands are refused and counted.
	 */
	void SetCommandHandler(FCommandHandler Handler) { CommandHandler = MoveTemp(Handler); }

	/** Unbinds the command handler if Owner bound it (a controller leaving must not drop a newer controller's binding). */
	void ClearCommandHandler(const UObject* Owner);

	/** Counters are reported at most this often (seconds). */
	static constexpr float StatusInterval = 0.25f;

private:
	void OpenInputs(FText& OutError);
	void CloseInputs();
	bool OpenOsc(FText& OutError);
	void CloseOsc();
	/** Opens the MIDI input and output the settings name, from one device listing. */
	bool OpenMidi(FText& OutError);
	void CloseMidi();
	/** The show is bound while some feedback can go out: OSC with a feedback port, or a MIDI output. */
	void UpdateShowBinding();
	/** The MIDI output shows the map's controls from scratch: after it opens and after the map changes (controls no longer bound go dark). */
	void RestartMidiFeedback();
	/** Feedback runs again after Delay seconds: a fader the hand let go of settles on the desk's value (refused moves snap back). */
	void ArmMidiSettle(double Delay);
	/** The clock of the hand's holds and of echo checks: the world's real time, so ticks and timers agree with it. */
	double MidiClock() const;

	void HandleOscMessage(const struct FOSCMessage& Message, const FString& IPAddress, uint16 Port);
	void HandleMidiEvent(class UMIDIDeviceInputController* Controller, int32 Timestamp, int32 Type, int32 Channel, int32 Data1, int32 Data2);

	void Enqueue(const FStageRemoteCommand& Command);
	FStageEconomyResultInfo Dispatch(const FStageRemoteCommand& Command);

	/** OSC and MIDI feedback (Docs/ADR/0035 §6, §12): the show's values that changed since the last send. */
	UFUNCTION()
	void HandleShowCueStarted(float CueNumber);
	UFUNCTION()
	void HandleShowDeskChanged();
	void ScheduleFeedback();
	void SendOscFeedback(const class UShowControlSubsystem& Show);
	void SendMidiFeedback(const class UShowControlSubsystem& Show);
	void SendFeedbackValue(const FString& Address, float Value);
	void BindShow();
	void UnbindShow();

	/** Counters changed: the status goes out with the next StatusInterval. Structural changes call BroadcastStatus at once. */
	void MarkStatusDirty();
	void BroadcastStatus();
	void SetProblem(const FText& Problem);

	UPROPERTY(Transient)
	TObjectPtr<class UOSCServer> OscServer = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UOSCClient> OscClient = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UMIDIDeviceInputController> MidiInput = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UMIDIDeviceOutputController> MidiOutput = nullptr;

	/** What the MIDI controller shows, so MIDI feedback sends only changes (and never a fader's own move back to it). */
	FStageMidiFeedbackMemory MidiShown;

	/** The show this world's feedback reads (non-owning; the world owns it). */
	TWeakObjectPtr<class UShowControlSubsystem> BoundShow;

	FDelegateHandle OscMessageHandle;
	FDelegateHandle MidiEventHandle;

	/** This frame's commands (StageRemote::Enqueue keeps it coalesced and capped). */
	TArray<FStageRemoteCommand> PendingCommands;

	FCommandHandler CommandHandler;

	/** Where feedback goes: the last OSC sender's address (port is the setting). Empty until someone sends. */
	FString FeedbackHost;

	/** The values last sent as feedback, so only changes go out. Unset entries are sent on the next feedback. */
	struct FSentFeedback
	{
		TOptional<float> ActiveCue;
		TArray<TOptional<float>> Levels;
		TOptional<float> GrandMaster;
		TOptional<float> Blackout;
		TOptional<float> AudioMaster;
	};
	FSentFeedback SentFeedback;

	/** MIDI learn waits for a control: the binding to complete (its action and target set). */
	TOptional<FStageMidiBinding> LearnTemplate;

	FStageRemoteStatus Status;

	FTimerHandle FlushTimer;
	FTimerHandle FeedbackTimer;
	FTimerHandle MidiSettleTimer;
	FTimerHandle StatusTimer;
	bool bStatusDirty = false;

	/**
	 * The pushed verdict (SetCapabilityAllowed). True until a push, so tests and tools that call ApplySettings directly work unbound;
	 * the saved settings themselves wait for the push (bRestorePending), and play reaches ApplySettings only through the GameMode.
	 */
	bool bCapabilityAllowed = true;
	/** Why the inputs stay closed while not allowed (the GameMode's message). */
	FText CapabilityReason;
	/** OnWorldBeginPlay ran and the saved settings wait for the controller's first SetCapabilityAllowed. */
	bool bRestorePending = false;
};
