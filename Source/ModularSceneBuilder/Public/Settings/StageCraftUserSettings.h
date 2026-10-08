// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "Render/StageRenderTypes.h"
#include "StageCraftUserSettings.generated.h"

/**
 * Per-machine StageCraft preferences, saved in GameUserSettings.ini (ADR 0001 §Preferences). Registered as
 * GameUserSettingsClassName in DefaultEngine.ini; graphics settings are inherited from UGameUserSettings.
 *
 * This class only stores and validates. Applying a preference is the job of the system that owns it
 * (audio: UStageAudioSubsystem, which is also the only caller of the audio setters), so the stored
 * values are the single source of truth and there is exactly one write path.
 *
 * Every setter clamps; values read from disk are sanitised in LoadSettings, so a hand-edited ini can
 * never put NaN or out-of-range gains into the mixer.
 */
UCLASS(Config = GameUserSettings, ConfigDoNotCheckDefaults)
class MODULARSCENEBUILDER_API UStageCraftUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	UStageCraftUserSettings();

	/** The engine's settings object as StageCraft settings, or null if GameUserSettingsClassName is not this class. */
	static UStageCraftUserSettings* Get();

	//~ Begin UGameUserSettings Interface
	virtual void SetToDefaults() override;
	virtual void LoadSettings(bool bForceReload = false) override;
	//~ End UGameUserSettings Interface

	float GetMasterVolume() const { return MasterVolume; }
	float GetEffectsVolume() const { return EffectsVolume; }
	bool IsAudioMuted() const { return bAudioMuted; }

	/** Clamped to [0, 1]. Returns true if the stored value changed. Does not save. */
	bool SetMasterVolume(float Volume);
	bool SetEffectsVolume(float Volume);
	bool SetAudioMuted(bool bMuted);

	/** Object snapping while placing and moving items (UStageSnappingComponent is the only writer). */
	bool IsSnapToItemsEnabled() const { return bSnapToItems; }

	/** Returns true if the stored value changed. Does not save. */
	bool SetSnapToItemsEnabled(bool bEnabled);

	/** Render panel choices (UStageRenderSubsystem is the only writer). */
	const FStageRenderSettings& GetRenderSettings() const { return RenderSettings; }

	/** Sanitized (out-of-range enums fall back to defaults). Returns true if the stored value changed. Does not save. */
	bool SetRenderSettings(const FStageRenderSettings& Settings);

private:
	void ResetAudioToDefaults();
	void SanitizeAudio();

	/** Bumped when the meaning of the audio fields changes; older saved values are then reset. */
	static constexpr int32 CurrentAudioSettingsVersion = 1;

	UPROPERTY(Config)
	float MasterVolume = 1.f;

	UPROPERTY(Config)
	float EffectsVolume = 1.f;

	UPROPERTY(Config)
	bool bAudioMuted = false;

	UPROPERTY(Config)
	int32 AudioSettingsVersion = 0;

	UPROPERTY(Config)
	bool bSnapToItems = true;

	UPROPERTY(Config)
	FStageRenderSettings RenderSettings;
};
