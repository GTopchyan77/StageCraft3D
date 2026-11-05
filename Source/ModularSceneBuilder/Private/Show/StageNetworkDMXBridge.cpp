// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageNetworkDMXBridge.h"

#include "Async/Async.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageNetworkDMXBridge)

UStageNetworkDMXBridge::UStageNetworkDMXBridge()
{
	// Follows the sender (OpenOutput / CloseOutput): a bridge that only listens must not make the show render universes.
	bOutputEnabled = false;
}

bool UStageNetworkDMXBridge::OpenInput(const FStageDMXReceiverConfig& Config, FText& OutError)
{
	// Called on a socket thread when a batch starts; the weak pointer is only resolved on the game thread.
	const TWeakObjectPtr<UStageNetworkDMXBridge> WeakThis(this);
	return Receiver.Start(Config, [WeakThis]()
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (UStageNetworkDMXBridge* Bridge = WeakThis.Get())
			{
				Bridge->DrainInbox();
			}
		});
	}, OutError);
}

void UStageNetworkDMXBridge::CloseInput()
{
	Receiver.Stop();
}

bool UStageNetworkDMXBridge::OpenOutput(const FStageDMXSenderConfig& Config, FText& OutError)
{
	const bool bOpened = Sender.Open(Config, OutError);
	SetOutputEnabled(bOpened);
	return bOpened;
}

void UStageNetworkDMXBridge::CloseOutput()
{
	SetOutputEnabled(false);
	Sender.Close();
}

void UStageNetworkDMXBridge::SendUniverse_Implementation(int32 Universe, const TArray<uint8>& Channels)
{
	Sender.Post(Universe, Channels);
}

void UStageNetworkDMXBridge::Stop()
{
	CloseOutput();
	CloseInput();
	Super::Stop();
}

void UStageNetworkDMXBridge::BeginDestroy()
{
	CloseOutput();
	CloseInput();
	Super::BeginDestroy();
}

void UStageNetworkDMXBridge::DrainInbox()
{
	Receiver.Drain([this](int32 Universe, TConstArrayView<uint8> Slots)
	{
		Scratch.Reset();
		Scratch.Append(Slots.GetData(), Slots.Num());
		ReceiveUniverse(Universe, Scratch);
	});
	OnFramesApplied.ExecuteIfBound();
}
