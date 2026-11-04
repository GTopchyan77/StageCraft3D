// Copyright Epic Games, Inc. All Rights Reserved.

#include "Show/StageShowMix.h"

namespace StageShowMix
{
	/** The value from the cue list and the playbacks: HTP for intensity, LTP by activation order for the rest. */
	bool ResolvePlaybacks(const FGameplayTag& Attribute, bool bIntensity, const FFixtureLayers& Layers, float& OutValue)
	{
		const float* Main = Layers.Playback ? Layers.Playback->Find(Attribute) : nullptr;
		if (bIntensity)
		{
			bool bHeld = Main != nullptr;
			float Highest = Main ? *Main : 0.f;
			for (const FPlaybackLayer& Playback : Layers.Playbacks)
			{
				if (const float* Value = Playback.Values ? Playback.Values->Find(Attribute) : nullptr)
				{
					Highest = bHeld ? FMath::Max(Highest, *Value * Playback.Level) : *Value * Playback.Level;
					bHeld = true;
				}
			}
			OutValue = Highest;
			return bHeld;
		}
		const FPlaybackLayer* Latest = nullptr;
		const float* LatestValue = nullptr;
		for (const FPlaybackLayer& Playback : Layers.Playbacks)
		{
			const float* Value = Playback.Values ? Playback.Values->Find(Attribute) : nullptr;
			if (Value && (!Latest || Playback.Order > Latest->Order))
			{
				Latest = &Playback;
				LatestValue = Value;
			}
		}
		if (const float* Value = LatestValue ? LatestValue : Main)
		{
			OutValue = *Value;
			return true;
		}
		return false;
	}

	/**
	 * Puts Effect over the static value InOutValue (bHasStatic: some layer held it). A relative effect needs a static value to move
	 * around, so without one it does nothing; an absolute one fades in from the static value (or stands alone). True when it applied.
	 */
	bool ApplyEffect(const FEffectValue& Effect, bool bHasStatic, float& InOutValue)
	{
		const float Weight = FMath::Clamp(Effect.Weight, 0.f, 1.f);
		if (Effect.bRelative)
		{
			if (!bHasStatic)
			{
				return false;
			}
			InOutValue += Effect.Value * Weight;
			return true;
		}
		InOutValue = bHasStatic ? FMath::Lerp(InOutValue, Effect.Value, Weight) : Effect.Value;
		return true;
	}

	void ComposeEffect(FEffectValue& Into, const FEffectValue& Over)
	{
		const float Below = FMath::Clamp(Into.Weight, 0.f, 1.f);
		const float Above = FMath::Clamp(Over.Weight, 0.f, 1.f);
		if (Into.bRelative != Over.bRelative || Above >= 1.f)
		{
			Into = Over;
			return;
		}
		if (Over.bRelative)
		{
			// Offsets: Into's share that Over leaves, plus Over's own, both already weighted.
			Into.Value = Into.Value * Below * (1.f - Above) + Over.Value * Above;
			Into.Weight = 1.f;
			return;
		}
		// s -> lerp(s, a, wa) -> lerp(.., b, wb) is lerp(s, V, W) with W = 1 - (1 - wa)(1 - wb).
		const float Weight = 1.f - (1.f - Below) * (1.f - Above);
		Into.Value = Weight > UE_SMALL_NUMBER ? (Into.Value * Below * (1.f - Above) + Over.Value * Above) / Weight : Over.Value;
		Into.Weight = Weight;
	}

	bool ResolveAttribute(const FGameplayTag& Attribute, const FFixtureLayers& Layers, float& OutValue)
	{
		// The kind is only needed when something can change intensity on its way through: playbacks (HTP) or the masters.
		const bool bNeedsKind = !Layers.Playbacks.IsEmpty() || Layers.IntensityScale != 1.f;
		const bool bIntensity = bNeedsKind && StagePaletteRules::GetAttributeKind(Attribute) == EStagePaletteKind::Intensity;
		bool bFound = false;
		if (const float* Highlighted = Layers.Highlight ? Layers.Highlight->Find(Attribute) : nullptr)
		{
			OutValue = *Highlighted;
			bFound = true;
		}
		else
		{
			if (const float* Programmed = Layers.Programmer ? Layers.Programmer->Find(Attribute) : nullptr)
			{
				OutValue = *Programmed;
				bFound = true;
			}
			if (!bFound)
			{
				bFound = ResolvePlaybacks(Attribute, bIntensity, Layers, OutValue);
			}
			if (!bFound)
			{
				const float* Home = Layers.Home ? Layers.Home->Find(Attribute) : nullptr;
				OutValue = Home ? *Home : OutValue;
				bFound = Home != nullptr;
			}
			if (const FEffectValue* Effect = Layers.Effects ? Layers.Effects->Find(Attribute) : nullptr)
			{
				bFound = ApplyEffect(*Effect, bFound, OutValue) || bFound;
			}
		}
		if (bFound && bIntensity)
		{
			OutValue *= Layers.IntensityScale;
		}
		return bFound;
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
