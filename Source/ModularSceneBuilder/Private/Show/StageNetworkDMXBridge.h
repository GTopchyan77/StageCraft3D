// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Show/StageDMXBridge.h"
#include "Show/StageDMXNetworkReceiver.h"
#include "StageNetworkDMXBridge.generated.h"

/**
 * The DMX transport for Art-Net and sACN input (Docs/ADR/0019 §3.3): owns an FStageDMXNetworkReceiver and hands its frames to
 * the show control on the game thread. Input only: output stays off, so the show never ticks to render frames for it.
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
	//~ End UStageDMXBridge Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

	/** Opens the ports. False, with nothing open, when they cannot be. */
	bool Open(const FStageDMXReceiverConfig& Config, FText& OutError);

	bool IsListening() const { return Receiver.IsRunning(); }

	/** See FStageDMXNetworkReceiver::SetUniverses. */
	void SetUniverses(TConstArrayView<int32> Universes) { Receiver.SetUniverses(Universes); }

	FStageDMXInputStatus GetStatus() const { return Receiver.GetStatus(); }

	/** Called on the game thread after each batch of frames was handed to the show. */
	FSimpleDelegate OnFramesApplied;

private:
	/** Game thread, queued by the inbox: applies every universe that received a frame since the last drain. */
	void DrainInbox();

	FStageDMXNetworkReceiver Receiver;

	/** ReceiveUniverse takes a TArray; reused, so draining does not allocate per frame. */
	TArray<uint8> Scratch;
};
