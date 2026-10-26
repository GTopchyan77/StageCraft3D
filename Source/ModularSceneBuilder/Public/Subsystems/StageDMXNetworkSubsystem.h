// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Show/StageDMXNetworkTypes.h"
#include "StageDMXNetworkSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnStageDMXInputChanged);

/**
 * Art-Net / sACN input for one world (Docs/ADR/0019-dmx-patch-and-network-input.md §3): switches the listener on and off,
 * keeps its sACN multicast groups and ArtPollReply in step with the patch, and reports its status.
 *
 * States: Off -> Listening (ports open, the network bridge installed in the show control) or Failed (ports could not be opened;
 * the error is kept for the Patch panel). Switching on remembers the choice in the user settings, so the next stage opens
 * listening; the controller's RequestSetDMXInput pairs it with the show's External control source.
 *
 * Event-driven, no tick: frames reach the show through the bridge's game-thread hand-off; status listeners are told at most
 * twice a second while frames arrive (one timer armed by the traffic, none while the wire is quiet), and at once on every state
 * change. Patch changes are folded into one update on the next tick.
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

	/**
	 * Opens (true) or closes the ports and remembers the choice on this PC. False, with OutError, when they cannot be opened;
	 * the state is then Failed and the remembered choice stays off. Does not change the show's control source.
	 */
	bool SetInputEnabled(bool bEnabled, FText& OutError);

	/** Replaces the settings (sanitized) and saves them; a running listener restarts with them. False with OutError if that fails. */
	bool ApplySettings(const FStageDMXInputSettings& Settings, FText& OutError);

	FStageDMXInputSettings GetSettings() const;

	EStageDMXInputState GetState() const { return State; }

	FStageDMXInputStatus GetStatus() const;

	/** State changes at once; traffic counters at most every StatusIntervalSeconds while frames arrive. */
	FOnStageDMXInputChanged OnInputChanged;

	static constexpr float StatusIntervalSeconds = 0.5f;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	bool StartListening(FText& OutError);
	void StopListening();
	void SaveEnabled(bool bEnabled);
	void SetState(EStageDMXInputState NewState, const FText& Error);
	void UpdateUniverses();

	UFUNCTION()
	void HandlePatchChanged();

	void HandleFramesApplied();

	UPROPERTY(Transient)
	TObjectPtr<class UStageNetworkDMXBridge> Bridge = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UShowControlSubsystem> ShowControl = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageDMXPatchSubsystem> Patch = nullptr;

	/** Used when there are no user settings (commandlets, tests): the input then works but is not remembered. */
	FStageDMXInputSettings FallbackSettings;

	EStageDMXInputState State = EStageDMXInputState::Off;
	FText LastError;

	FTimerHandle StatusTimer;
	FTimerHandle UniverseTimer;
};
