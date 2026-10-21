// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/StageCraftUserSettings.h"

#include "Audio/StageAudioTypes.h"
#include "Engine/Engine.h"
#include "Viewport/StageViewMath.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftUserSettings)

UStageCraftUserSettings::UStageCraftUserSettings()
{
	ResetAudioToDefaults();
	// Not ResetInputToDefaults: the key map's defaults need registered FKeys, which a class default object may predate.
	InputSettingsVersion = CurrentInputSettingsVersion;
}

UStageCraftUserSettings* UStageCraftUserSettings::Get()
{
	return GEngine ? Cast<UStageCraftUserSettings>(GEngine->GetGameUserSettings()) : nullptr;
}

void UStageCraftUserSettings::SetToDefaults()
{
	Super::SetToDefaults();
	ResetAudioToDefaults();
	bSnapToItems = true;
	RenderSettings = FStageRenderSettings();
	GridSnapCm = StageViewMath::DefaultGridSnapCm;
	AngleSnapDegrees = StageViewMath::DefaultAngleSnapDegrees;
	bViewportGridVisible = true;
	ResetInputToDefaults();
}

void UStageCraftUserSettings::LoadSettings(bool bForceReload)
{
	Super::LoadSettings(bForceReload);

	if (AudioSettingsVersion != CurrentAudioSettingsVersion)
	{
		ResetAudioToDefaults();
	}
	SanitizeAudio();
	RenderSettings = StageRender::Sanitize(RenderSettings);
	// A hand-edited ini can only select a toolbar preset; anything else falls back to the default.
	GridSnapCm = StageViewMath::SanitizeGridSnap(GridSnapCm);
	AngleSnapDegrees = StageViewMath::SanitizeAngleSnap(AngleSnapDegrees);

	if (InputSettingsVersion != CurrentInputSettingsVersion)
	{
		ResetInputToDefaults();
	}
	SanitizeInput();
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

bool UStageCraftUserSettings::SetSnapToItemsEnabled(bool bEnabled)
{
	const bool bChanged = bSnapToItems != bEnabled;
	bSnapToItems = bEnabled;
	return bChanged;
}

bool UStageCraftUserSettings::SetRenderSettings(const FStageRenderSettings& Settings)
{
	const FStageRenderSettings Sanitized = StageRender::Sanitize(Settings);
	const bool bChanged = !(RenderSettings == Sanitized);
	RenderSettings = Sanitized;
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

bool UStageCraftUserSettings::SetGridSnapCm(float Cm)
{
	if (!StageViewMath::IsGridSnapPreset(Cm))
	{
		return false;
	}
	const bool bChanged = !FMath::IsNearlyEqual(GridSnapCm, Cm);
	GridSnapCm = Cm;
	return bChanged;
}

bool UStageCraftUserSettings::SetAngleSnapDegrees(float Degrees)
{
	if (!StageViewMath::IsAngleSnapPreset(Degrees))
	{
		return false;
	}
	const bool bChanged = !FMath::IsNearlyEqual(AngleSnapDegrees, Degrees);
	AngleSnapDegrees = Degrees;
	return bChanged;
}

bool UStageCraftUserSettings::SetViewportGridVisible(bool bVisible)
{
	const bool bChanged = bViewportGridVisible != bVisible;
	bViewportGridVisible = bVisible;
	return bChanged;
}

FStageKeyBindings UStageCraftUserSettings::GetKeyBindings(int32* OutDropped) const
{
	return FStageKeyBindings::FromOverrides(KeyBindingOverrides, OutDropped);
}

bool UStageCraftUserSettings::SetKeyBindings(const FStageKeyBindings& Bindings)
{
	TArray<FStageKeyBindingOverride> Overrides = Bindings.ToOverrides();
	if (Overrides == KeyBindingOverrides)
	{
		return false;
	}
	KeyBindingOverrides = MoveTemp(Overrides);
	return true;
}

bool UStageCraftUserSettings::SetLookSensitivity(float Sensitivity)
{
	if (!FMath::IsFinite(Sensitivity))
	{
		return false;
	}
	const float Clamped = FMath::Clamp(Sensitivity, StageInputPreferences::MinLookSensitivity, StageInputPreferences::MaxLookSensitivity);
	const bool bChanged = !FMath::IsNearlyEqual(LookSensitivity, Clamped);
	LookSensitivity = Clamped;
	return bChanged;
}

bool UStageCraftUserSettings::SetLookInverted(bool bInverted)
{
	const bool bChanged = bInvertLook != bInverted;
	bInvertLook = bInverted;
	return bChanged;
}

void UStageCraftUserSettings::ResetInputToDefaults()
{
	KeyBindingOverrides.Reset();
	LookSensitivity = StageInputPreferences::DefaultLookSensitivity;
	bInvertLook = false;
	InputSettingsVersion = CurrentInputSettingsVersion;
}

void UStageCraftUserSettings::SanitizeInput()
{
	// The key map is not checked here: settings load before the engine registers its keys, when every key would read as
	// invalid. GetKeyBindings validates it on every read instead.
	LookSensitivity = FMath::IsFinite(LookSensitivity)
		? FMath::Clamp(LookSensitivity, StageInputPreferences::MinLookSensitivity, StageInputPreferences::MaxLookSensitivity)
		: StageInputPreferences::DefaultLookSensitivity;
}
