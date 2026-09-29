// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Data/BaseItemData.h"
#include "AudioEquipmentData.generated.h"

UENUM(BlueprintType)
enum class EStageAudioKind : uint8
{
	LineArrayElement	UMETA(DisplayName = "Line Array Element"),
	PointSource			UMETA(DisplayName = "Point Source"),
	Subwoofer			UMETA(DisplayName = "Subwoofer"),
	Monitor				UMETA(DisplayName = "Monitor / Fill"),
	MixingConsole		UMETA(DisplayName = "Mixing Console"),
	Amplifier			UMETA(DisplayName = "Amplifier"),
	Processor			UMETA(DisplayName = "System Processor"),
};

/**
 * Catalog entry for audio gear. Loudspeakers use the acoustic fields; consoles, amps and
 * processors use ChannelCount and the shared Specs (weight, power). Placed instances
 * (AAudioEquipmentActor) hold gain, delay, mute, polarity and splay.
 */
UCLASS(BlueprintType, Const)
class MODULARSCENEBUILDER_API UAudioEquipmentData : public UBaseItemData
{
	GENERATED_BODY()

public:
	UAudioEquipmentData();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
	//~ End UObject Interface

	UFUNCTION(BlueprintPure, Category = "Audio")
	bool IsLoudspeaker() const;

	UFUNCTION(BlueprintPure, Category = "Audio")
	bool SupportsSplay() const { return MaxSplayAngle > 0.f; }

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio")
	EStageAudioKind AudioKind = EStageAudioKind::LineArrayElement;

	// --- Acoustics (loudspeakers) ---

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "360.0", Units = "deg"))
	float HorizontalCoverage = 110.f;

	/** For line array elements this is the single-box vertical dispersion, not the array's. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "360.0", Units = "deg"))
	float VerticalCoverage = 10.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "160.0"))
	float MaxSPL = 140.f;

	/** Usable frequency response in Hz (X = low, Y = high). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acoustics")
	FVector2D FrequencyResponse = FVector2D(55.0, 18000.0);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acoustics", meta = (ClampMin = "0.0"))
	float NominalImpedance = 8.f;

	/** Largest inter-cabinet angle the rigging allows. Zero means the item cannot be splayed. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "20.0", Units = "deg"))
	float MaxSplayAngle = 10.f;

	// --- Electronics (consoles, amps, processors) ---

	/** Input channels for consoles, output channels for amplifiers/processors. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Electronics", meta = (ClampMin = "0"))
	int32 ChannelCount = 0;
};
