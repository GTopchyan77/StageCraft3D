// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StageDMXBridge.generated.h"

/**
 * Boundary between StageCraft's show control and a DMX transport. StageCraft itself only deals
 * in 512-byte universe buffers; a bridge moves them on and off the wire.
 *
 * The intended production implementation wraps Unreal's DMX Engine plugin (DMXProtocol: Art-Net
 * and sACN input/output ports) in a separate module, so the core module never depends on the
 * plugin. Blueprint subclasses work too (e.g. for a quick test bridge).
 *
 *   Output: UShowControlSubsystem renders universes and calls SendUniverse at DMXOutputRateHz.
 *   Input:  the bridge calls ReceiveUniverse when a frame arrives; in External control mode the
 *           subsystem decodes it into fixture attributes.
 */
UCLASS(Abstract, Blueprintable, EditInlineNew)
class MODULARSCENEBUILDER_API UStageDMXBridge : public UObject
{
	GENERATED_BODY()

public:
	/** Called by the subsystem when the bridge becomes active. Open ports here. */
	virtual void Start(class UShowControlSubsystem& InOwner);

	/** Called when the bridge is replaced or the world ends. Close ports here. */
	virtual void Stop();

	/** Outbound frame. Channels has exactly 512 entries; index 0 is DMX address 1. */
	UFUNCTION(BlueprintNativeEvent, Category = "StageCraft|DMX")
	void SendUniverse(int32 Universe, const TArray<uint8>& Channels);

	UFUNCTION(BlueprintPure, Category = "StageCraft|DMX")
	bool IsOutputEnabled() const { return bOutputEnabled; }

	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void SetOutputEnabled(bool bEnabled) { bOutputEnabled = bEnabled; }

protected:
	/** Inbound frame from the wire. Forwarded to the owning subsystem. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|DMX")
	void ReceiveUniverse(int32 Universe, const TArray<uint8>& Channels);

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|DMX", meta = (DisplayName = "On Started"))
	void BP_OnStarted();

	UFUNCTION(BlueprintImplementableEvent, Category = "StageCraft|DMX", meta = (DisplayName = "On Stopped"))
	void BP_OnStopped();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StageCraft|DMX")
	bool bOutputEnabled = true;

private:
	TWeakObjectPtr<class UShowControlSubsystem> Owner;
};
