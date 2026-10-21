// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StagePowerDistributionSettings.h"

#include "Report/StageCableSchedule.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePowerDistributionSettings)

FStagePowerDistribution UStagePowerDistributionSettings::ToDistribution() const
{
	FStagePowerDistribution Supply;
	Supply.PhaseVoltage = PhaseVoltage;
	Supply.PhaseCount = Phases == EStagePowerPhases::Single ? 1 : 3;
	Supply.CircuitRatingAmps = CircuitRatingAmps;
	Supply.UsableShare = UsablePercent / 100.0;
	Supply.PowerFactor = PowerFactor;
	return StageCableSchedule::Sanitize(Supply);
}
