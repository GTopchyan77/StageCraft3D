// Copyright Epic Games, Inc. All Rights Reserved.

#include "Report/StageCablingSettings.h"

#include "Report/StageShoppingList.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCablingSettings)

namespace
{
	EStageCableAnchor ToAnchor(EStageCableAnchorSetting Setting)
	{
		return Setting == EStageCableAnchorSetting::Fixed ? EStageCableAnchor::Fixed : EStageCableAnchor::Automatic;
	}

	TArray<double> ToDoubles(const TArray<float>& Values)
	{
		TArray<double> Out;
		Out.Reserve(Values.Num());
		for (const float Value : Values)
		{
			Out.Add(Value);
		}
		return Out;
	}
}

FStageCablingRules UStageCablingSettings::ToRules() const
{
	FStageCablingRules Rules;
	Rules.SafetyMargin = SafetyMarginPercent / 100.0;
	Rules.StockLengthsMeters = ToDoubles(StockLengthsMeters);
	Rules.PowerCableName = PowerCableName;
	Rules.DmxCableName = DmxCableName;
	Rules.DistroAnchor = ToAnchor(DistroPosition);
	Rules.DistroLocationMeters = DistroLocationMeters;
	Rules.ControlAnchor = ToAnchor(ControlNodePosition);
	Rules.ControlLocationMeters = ControlNodeLocationMeters;
	Rules.DistroOutputs = DistroOutputs;
	Rules.DistroFeedAmps = DistroFeedAmps;
	Rules.RiggedAboveMeters = RiggedAboveMeters;
	Rules.WeightPerClampKg = WeightPerClampKg;
	Rules.SafetyBondRatingsKg = ToDoubles(SafetyBondRatingsKg);
	return StageShoppingList::Sanitize(Rules);
}
