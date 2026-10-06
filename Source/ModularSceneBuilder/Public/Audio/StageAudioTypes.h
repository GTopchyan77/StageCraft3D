// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"

/** Feedback cue identities. Which sound plays for each is data (UStageAudioDeveloperSettings), never code. */
namespace StageCraftTags
{
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Sound);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Sound_Select);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Sound_Place);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Sound_Snap);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Sound_Error);
}

/**
 * Volume rules in one pure place, so the settings, the mixer override and the per-cue gain agree.
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

	/** Per-play gain of one feedback cue, on top of the master gain. CueScale is the cue's authored level. */
	inline float ComputeCueGain(float EffectsVolume, float CueScale)
	{
		return SanitizeVolume(EffectsVolume) * FMath::Max(FMath::IsFinite(CueScale) ? CueScale : 0.f, 0.f);
	}
}
