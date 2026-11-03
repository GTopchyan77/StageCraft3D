// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageShowMix.h"

namespace StageShowMix
{
	bool ResolveAttribute(const FGameplayTag& Attribute, const FFixtureLayers& Layers, float& OutValue)
	{
		for (const FAttributeMap* Layer : { Layers.Highlight, Layers.Programmer, Layers.Playback, Layers.Home })
		{
			if (const float* Value = Layer ? Layer->Find(Attribute) : nullptr)
			{
				OutValue = *Value;
				return true;
			}
		}
		return false;
	}

	bool IsHeldAbovePlayback(const FGameplayTag& Attribute, const FFixtureLayers& Layers)
	{
		return (Layers.Highlight && Layers.Highlight->Contains(Attribute)) || (Layers.Programmer && Layers.Programmer->Contains(Attribute));
	}

	void ResolveFixture(TConstArrayView<FGameplayTag> Attributes, const FFixtureLayers& Layers, FAttributeMap& OutValues)
	{
		OutValues.Reset();
		for (const FGameplayTag& Attribute : Attributes)
		{
			float Value = 0.f;
			if (ResolveAttribute(Attribute, Layers, Value))
			{
				OutValues.Add(Attribute, Value);
			}
		}
	}

	float FanFactor(EStageFanMode Mode, int32 Index, int32 Count)
	{
		if (Mode == EStageFanMode::Off || Count <= 1)
		{
			return 1.f;
		}
		// Position along the selection: -1 for the first fixture, +1 for the last.
		const float Position = 2.f * static_cast<float>(FMath::Clamp(Index, 0, Count - 1)) / static_cast<float>(Count - 1) - 1.f;
		switch (Mode)
		{
		case EStageFanMode::Linear:
			return Position;
		case EStageFanMode::V:
			return FMath::Abs(Position);
		case EStageFanMode::InvertedV:
			return 1.f - FMath::Abs(Position);
		default:
			return 1.f;
		}
	}
}
