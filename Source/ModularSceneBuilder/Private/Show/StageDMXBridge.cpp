// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageDMXBridge.h"

#include "Subsystems/ShowControlSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageDMXBridge)

void UStageDMXBridge::Start(UShowControlSubsystem& InOwner)
{
	Owner = &InOwner;
	BP_OnStarted();
}

void UStageDMXBridge::Stop()
{
	BP_OnStopped();
	Owner.Reset();
}

void UStageDMXBridge::SendUniverse_Implementation(int32 Universe, const TArray<uint8>& Channels)
{
	// Base bridge has no transport. Subclasses send over Art-Net/sACN.
}

void UStageDMXBridge::ReceiveUniverse(int32 Universe, const TArray<uint8>& Channels)
{
	if (UShowControlSubsystem* ShowControl = Owner.Get())
	{
		ShowControl->ReceiveDMXUniverse(Universe, Channels);
	}
}
