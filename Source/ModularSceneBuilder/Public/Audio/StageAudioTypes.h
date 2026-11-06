// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Volume rules in one pure place, so the settings and the mixer override agree.
 * Covered by StageCraft.Audio.Math tests.
 */
namespace StageAudioMath
{
	/** Every user volume is a linear gain in [0, 1]. Non-finite input (corrupt ini) becomes the default, 1. */
	inline float SanitizeVolume(float Volume)
	{
		return FMath::IsFinite(Volume) ? FMath::Clamp(Volume, 0.f, 1.f) : 1.f;
	}

	/** Gain applied to the project's master sound class (affects every sound). Mute wins over any volume. */
	inline float ComputeMasterGain(float MasterVolume, bool bMuted)
	{
		return bMuted ? 0.f : SanitizeVolume(MasterVolume);
	}
}
