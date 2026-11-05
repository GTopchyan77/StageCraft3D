// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageDMXBridge.h"
#include "Show/StageDMXNetworkReceiver.h"
#include "Show/StageDMXNetworkSender.h"
#include "StageNetworkDMXBridge.generated.h"

/**
 * The DMX transport for Art-Net and sACN (Docs/ADR/0019 §3.3 and §9): owns an FStageDMXNetworkReceiver (input) and an
 * FStageDMXNetworkSender (output), each opened and closed on its own, and is the show control's one bridge while either runs.
 *  - Input: frames are handed to the show on the game thread (ReceiveUniverse); the show applies them only under External.
 *  - Output: the show renders a universe when its look changed and calls SendUniverse (Internal only); the frame is posted to
 *    the sender's mailbox and leaves on the sender's thread. bOutputEnabled follows whether the sender is open, so the show
 *    renders nothing for a bridge that only listens.
 * Created, opened and installed by UStageDMXNetworkSubsystem; stopped by the show control when replaced or at world end.
 */
UCLASS(NotBlueprintable, Transient)
class UStageNetworkDMXBridge : public UStageDMXBridge
{
	GENERATED_BODY()

public:
	UStageNetworkDMXBridge();

	//~ Begin UStageDMXBridge Interface
	virtual void Stop() override;
	virtual void SendUniverse_Implementation(int32 Universe, const TArray<uint8>& Channels) override;
	//~ End UStageDMXBridge Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

	/** Opens the input ports. False, with nothing open, when they cannot be. */
	bool OpenInput(const FStageDMXReceiverConfig& Config, FText& OutError);

	void CloseInput();

	bool IsListening() const { return Receiver.IsRunning(); }

	/** See FStageDMXNetworkReceiver::SetUniverses. */
	void SetInputUniverses(TConstArrayView<int32> Universes) { Receiver.SetUniverses(Universes); }

	FStageDMXInputStatus GetInputStatus() const { return Receiver.GetStatus(); }

	/** Opens the output socket and thread. False, with nothing open, when they cannot be. */
	bool OpenOutput(const FStageDMXSenderConfig& Config, FText& OutError);

	void CloseOutput();

	bool IsSending() const { return Sender.IsOpen(); }

	/** See FStageDMXNetworkSender::SetUniverses: the patched universes without the muted ones. */
	void SetOutputUniverses(TConstArrayView<int32> Universes) { Sender.SetUniverses(Universes); }

	FStageDMXOutputStatus GetOutputStatus() const { return Sender.GetStatus(); }

	/** Whether the bridge carries nothing any more: the subsystem then removes it from the show. */
	bool IsIdle() const { return !IsListening() && !IsSending(); }

	/** Called on the game thread after each batch of received frames was handed to the show. */
	FSimpleDelegate OnFramesApplied;

private:
	/** Game thread, queued by the inbox: applies every universe that received a frame since the last drain. */
	void DrainInbox();

	FStageDMXNetworkReceiver Receiver;
	FStageDMXNetworkSender Sender;

	/** ReceiveUniverse takes a TArray; reused, so draining does not allocate per frame. */
	TArray<uint8> Scratch;
};
