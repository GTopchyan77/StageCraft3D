// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Economy/StageEconomyTypes.h"
#include "Remote/StageRemoteControlTypes.h"
#include "Remote/StageWebRemoteTypes.h"
#include "StageWebRemoteSubsystem.generated.h"

/**
 * The Web Remote of the stage's desk (Docs/ADR/0036): a page served to phones and tablets on the LAN that runs the desk's live
 * controls. Owns this PC's FStageWebRemoteServer for one stage world, like UStageRemoteControlSubsystem owns OSC and MIDI:
 * OnWorldBeginPlay reopens what the saved settings ask for, Deinitialize stops the server. No tick.
 *
 * Flow: the server's worker thread answers HTTP, pairs devices and reads WebSockets → paired commands come back as events, drained
 * on the game thread → StageRemote::ParseOsc resolves each path (the OSC address space, ADR 0035 §3) → the frame's queue (continuous
 * values coalesced, at most StageRemote::MaxCommandsPerFrame) → once per frame each command, marked EStageRemoteSource::Web, goes
 * to the command handler the local AModularPlayerController binds (RequestRemoteCommand: GameMode EvaluateWebRemote, then the
 * desk's own request). The subsystem never changes the show.
 *
 * Pushes the show's state (UShowControlSubsystem's desk, cues and playbacks) to the pages at most StateInterval apart, and each
 * refusal reason once. Writes the Web Remote settings in UStageCraftUserSettings (only writer).
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageWebRemoteSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Carries out one command; returns the verdict. */
	using FCommandHandler = TDelegate<FStageEconomyResultInfo(const FStageRemoteCommand&)>;

	UStageWebRemoteSubsystem();
	virtual ~UStageWebRemoteSubsystem() override;

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
	const FStageWebRemoteSettings& GetSettings() const;

	/**
	 * Stores Settings (sanitized and saved) and restarts the server with them: a new PIN, every device unpaired. Returns false with
	 * OutError when the server was asked for and could not open (port in use); the settings are kept either way, and the status shows
	 * the problem. Game thread. Callers in play go through AModularPlayerController::RequestSetWebRemote.
	 */
	bool ApplySettings(const FStageWebRemoteSettings& Settings, FText& OutError);

	const FStageWebRemoteStatus& GetStatus() const { return Status; }

	/** Fires after the status changes: at once for pairing and the server opening or closing, at most every StatusInterval for counters. */
	FOnStageWebRemoteStatusChanged OnStatusChanged;

	/** A new PIN; paired devices stay. False while the server is off. */
	bool RenewPin();

	/** Forgets every paired device (their pages close) and makes a new PIN. False while the server is off. */
	bool UnpairAll();

	/**
	 * One command from a paired page, as received. The server's events call this; tests feed it directly. Game thread. An unknown
	 * path or a bad value is counted as ignored, like an OSC message.
	 */
	void ReceiveCommand(const FString& Path, TOptional<float> Value);

	/** Carries out the queued commands now (normally on the next tick after the first one arrives). */
	void FlushCommands();

	/** Who carries out commands: the local AModularPlayerController binds RequestRemoteCommand (tests bind their own). */
	void SetCommandHandler(FCommandHandler Handler) { CommandHandler = MoveTemp(Handler); }

	/** Unbinds the command handler if Owner bound it. */
	void ClearCommandHandler(const UObject* Owner);

	/** The port the server listens on; 0 while off. */
	int32 GetListeningPort() const;

	/** Where the page file is read from (shipped with the module, Source/ModularSceneBuilder/Resources/WebRemote). */
	static FString GetPageFile();

	/** Counters are reported at most this often (seconds). */
	static constexpr float StatusInterval = 0.25f;

	/** The show's state goes to the pages at most this often (seconds). */
	static constexpr float StateInterval = 1.f / 30.f;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests only: the PIN the next server start uses, so a test can pair without reading the status. Empty restores random PINs. */
	void SetPinForTesting(const FString& Pin) { PinForTesting = Pin; }
#endif

private:
	bool Open(FText& OutError);
	void Close();

	void DrainEvents();

	void Enqueue(const FStageRemoteCommand& Command);
	FStageEconomyResultInfo Dispatch(const FStageRemoteCommand& Command);

	UFUNCTION()
	void HandleShowCueStarted(float CueNumber);
	UFUNCTION()
	void HandleShowChanged();
	UFUNCTION()
	void HandleShowCueListChanged();
	void BindShow();
	void UnbindShow();

	/** The state (and the cue list when bCues) goes out within StateInterval. */
	void MarkStateDirty(bool bCues);
	void SendState();

	/** Reads the PIN, devices and lock from the server into Status. */
	void RefreshPairingStatus();

	/** The pages to open: the loopback address for ThisComputer, every IPv4 card address for Network. */
	static TArray<FString> FindAddresses(EStageWebRemoteScope Scope, int32 Port);

	void MarkStatusDirty();
	void BroadcastStatus();

	TUniquePtr<class FStageWebRemoteServer> Server;

	/** The show the pages follow (non-owning; the world owns it). */
	TWeakObjectPtr<class UShowControlSubsystem> BoundShow;

	TArray<FStageRemoteCommand> PendingCommands;
	FCommandHandler CommandHandler;

	/** The refusal last sent to the pages: the same reason is not sent again until a command succeeds. */
	FString LastSentRefusal;

	bool bCuesDirty = false;

	FStageWebRemoteStatus Status;

	FTimerHandle FlushTimer;
	FTimerHandle StateTimer;
	FTimerHandle StatusTimer;
	bool bStatusDirty = false;

#if WITH_DEV_AUTOMATION_TESTS
	FString PinForTesting;
#endif
};
