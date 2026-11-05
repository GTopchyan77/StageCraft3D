// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Show/ShowControlTypes.h"
#include "Show/StageDMXNetworkTypes.h"
#include "StageDMXNetworkSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnStageDMXInputChanged);
DECLARE_MULTICAST_DELEGATE(FOnStageDMXOutputChanged);

/**
 * Art-Net / sACN input and output for one world (Docs/ADR/0019-dmx-patch-and-network-input.md §3 and §9): switches each on and
 * off, keeps them in step with the patch (sACN multicast groups, ArtPollReply, the universes sent) and reports their status.
 *
 * Input and output are independent states, each Off -> Listening / Sending, or Failed (the error is kept for the Patch panel).
 * Both run in one UStageNetworkDMXBridge, installed in the show control while either is on and removed when both are off.
 * Switching one on remembers the choice in the user settings, so the next stage opens the same way. The controller's
 * RequestSetDMXInput pairs input with the show's External control source; output sends only while the show is Internal.
 *
 * Event-driven, no tick: received frames reach the show through the bridge's game-thread hand-off; output frames are rendered
 * by the show when the look changes. Input status listeners are told at most twice a second while frames arrive (one timer armed
 * by the traffic); output status listeners twice a second while sending (one looping timer that exists only then); both at once
 * on every state change. Patch changes are folded into one update on the next tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageDMXNetworkSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem / UWorldSubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End USubsystem / UWorldSubsystem Interface

	// --- Input ---

	/**
	 * Opens (true) or closes the input ports and remembers the choice on this PC. False, with OutError, when they cannot be
	 * opened; the state is then Failed and the remembered choice stays off. Does not change the show's control source.
	 */
	bool SetInputEnabled(bool bEnabled, FText& OutError);

	/**
	 * Replaces the input settings (sanitized) and saves them; a running listener restarts with them, and so does a running output
	 * (the network card and universe numbering are shared). False with OutError if a restart fails.
	 */
	bool ApplySettings(const FStageDMXInputSettings& Settings, FText& OutError);

	FStageDMXInputSettings GetSettings() const;

	EStageDMXInputState GetState() const { return State; }

	FStageDMXInputStatus GetStatus() const;

	/** State changes at once; traffic counters at most every StatusIntervalSeconds while frames arrive. */
	FOnStageDMXInputChanged OnInputChanged;

	// --- Output ---

	/**
	 * Opens (true) or closes the output and remembers the choice on this PC. False, with OutError, when it cannot be opened (no
	 * protocol on, unicast without an address, no socket); the state is then Failed and the remembered choice stays off.
	 */
	bool SetOutputEnabled(bool bEnabled, FText& OutError);

	/**
	 * Replaces the output settings (sanitized; this PC's Cid is kept) and saves them. A running output restarts when the target
	 * changed and only updates its universes when only the muted ones changed. False with OutError if a restart fails.
	 */
	bool ApplyOutputSettings(const FStageDMXOutputSettings& Settings, FText& OutError);

	FStageDMXOutputSettings GetOutputSettings() const;

	EStageDMXOutputState GetOutputState() const { return OutputState; }

	FStageDMXOutputStatus GetOutputStatus() const;

	/** State changes at once; packet counters every StatusIntervalSeconds while sending. */
	FOnStageDMXOutputChanged OnOutputChanged;

	static constexpr float StatusIntervalSeconds = 0.5f;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	/** The bridge, created and installed in the show on first use. Null without a show control. */
	class UStageNetworkDMXBridge* EnsureBridge();

	/** Removes the bridge from the show once neither direction uses it. */
	void ReleaseBridgeIfIdle();

	/** Lets go of a bridge the show stopped on its own (another transport was installed). */
	void ForgetStoppedBridge();

	bool StartListening(FText& OutError);
	void StopListening();
	void SaveEnabled(bool bEnabled);
	void SetState(EStageDMXInputState NewState, const FText& Error);

	bool StartSending(FText& OutError);
	void StopSending();
	void SaveOutputEnabled(bool bEnabled);
	void SetOutputState(EStageDMXOutputState NewState, const FText& Error);
	void SaveOutputSettings(const FStageDMXOutputSettings& Settings);

	/** Input groups / ArtPollReply and the universes sent (patched minus muted) follow the patch. */
	void UpdateUniverses();

	UFUNCTION()
	void HandlePatchChanged();

	/** Under External the show follows DMX IN and nothing is sent (and the sACN streams are terminated): StageCraft must not
	 * become a second source on the wire; back to Internal, every universe is sent again. */
	UFUNCTION()
	void HandleControlSourceChanged(EStageControlSource NewSource);

	void HandleFramesApplied();

	UPROPERTY(Transient)
	TObjectPtr<class UStageNetworkDMXBridge> Bridge = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UShowControlSubsystem> ShowControl = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXPatchSubsystem> Patch = nullptr;

	/** Used when there are no user settings (commandlets, tests): the input and output then work but are not remembered. */
	FStageDMXInputSettings FallbackSettings;
	FStageDMXOutputSettings FallbackOutputSettings;

	EStageDMXInputState State = EStageDMXInputState::Off;
	FText LastError;

	EStageDMXOutputState OutputState = EStageDMXOutputState::Off;
	FText LastOutputError;

	FTimerHandle StatusTimer;
	FTimerHandle OutputStatusTimer;
	FTimerHandle UniverseTimer;
};
