// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/StageCraftUserSettings.h"

#include "Audio/StageAudioTypes.h"
#include "Engine/Engine.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftUserSettings)

UStageCraftUserSettings::UStageCraftUserSettings()
{
	ResetAudioToDefaults();
}

UStageCraftUserSettings* UStageCraftUserSettings::Get()
{
	return GEngine ? Cast<UStageCraftUserSettings>(GEngine->GetGameUserSettings()) : nullptr;
}

void UStageCraftUserSettings::SetToDefaults()
{
	Super::SetToDefaults();
	ResetAudioToDefaults();
}

void UStageCraftUserSettings::LoadSettings(bool bForceReload)
{
	Super::LoadSettings(bForceReload);

	if (AudioSettingsVersion != CurrentAudioSettingsVersion)
	{
		ResetAudioToDefaults();
	}
	SanitizeAudio();
}

bool UStageCraftUserSettings::SetMasterVolume(float Volume)
{
	const float Sanitized = StageAudioMath::SanitizeVolume(Volume);
	const bool bChanged = !FMath::IsNearlyEqual(MasterVolume, Sanitized);
	MasterVolume = Sanitized;
	return bChanged;
}

bool UStageCraftUserSettings::SetEffectsVolume(float Volume)
{
	const float Sanitized = StageAudioMath::SanitizeVolume(Volume);
	const bool bChanged = !FMath::IsNearlyEqual(EffectsVolume, Sanitized);
	EffectsVolume = Sanitized;
	return bChanged;
}

bool UStageCraftUserSettings::SetAudioMuted(bool bMuted)
{
	const bool bChanged = bAudioMuted != bMuted;
	bAudioMuted = bMuted;
	return bChanged;
}

void UStageCraftUserSettings::ResetAudioToDefaults()
{
	MasterVolume = 1.f;
	EffectsVolume = 1.f;
	bAudioMuted = false;
	AudioSettingsVersion = CurrentAudioSettingsVersion;
}

void UStageCraftUserSettings::SanitizeAudio()
{
	MasterVolume = StageAudioMath::SanitizeVolume(MasterVolume);
	EffectsVolume = StageAudioMath::SanitizeVolume(EffectsVolume);
}
