// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageNetworkDMXBridge.h"

#include "Async/Async.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageNetworkDMXBridge)

UStageNetworkDMXBridge::UStageNetworkDMXBridge()
{
	// Input only (Gevor's choice, ADR 0019); with output on, the show would render universes 44 times a second for nobody.
	bOutputEnabled = false;
}

bool UStageNetworkDMXBridge::Open(const FStageDMXReceiverConfig& Config, FText& OutError)
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

void UStageNetworkDMXBridge::Stop()
{
	Receiver.Stop();
	Super::Stop();
}

void UStageNetworkDMXBridge::BeginDestroy()
{
	Receiver.Stop();
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
