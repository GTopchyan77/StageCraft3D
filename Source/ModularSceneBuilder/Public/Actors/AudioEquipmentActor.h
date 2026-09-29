// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Actors/ModularBaseActor.h"
#include "AudioEquipmentActor.generated.h"

/**
 * Placed audio gear. Loudspeakers expose system-tuning parameters (gain, delay, mute, polarity,
 * splay); consoles/amps/processors expose specs only. The values are design data for now:
 * nothing is rendered acoustically. They are the inputs a later coverage/SPL visualisation reads.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API AAudioEquipmentActor : public AModularBaseActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	class UAudioEquipmentData* GetAudioData() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	float GetGainDb() const { return GainDb; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	bool IsMuted() const { return bMuted; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	float GetDelayMs() const { return DelayMs; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	bool IsPolarityInverted() const { return bPolarityInverted; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	float GetSplayAngle() const { return SplayAngle; }

protected:
	//~ Begin AModularBaseActor Interface
	virtual void ApplyItemData(const class UBaseItemData& Data) override;
	virtual void GatherParameterSections(TArray<FStageParameterSection>& OutSections) const override;
	virtual bool ReadParameter(const FGameplayTag& ParameterId, FStageParameterValue& OutValue) const override;
	virtual bool WriteParameter(const FGameplayTag& ParameterId, const FStageParameterValue& Value) override;
	//~ End AModularBaseActor Interface

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Audio", meta = (ClampMin = "-60.0", ClampMax = "12.0"))
	float GainDb = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Audio")
	bool bMuted = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Audio", meta = (ClampMin = "0.0", ClampMax = "1000.0", Units = "ms"))
	float DelayMs = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Audio")
	bool bPolarityInverted = false;

	/** Angle to the element above in a line array hang. Clamped to the catalog's MaxSplayAngle. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, SaveGame, Category = "StageCraft|Audio", meta = (ClampMin = "0.0", ClampMax = "20.0", Units = "deg"))
	float SplayAngle = 0.f;
};
