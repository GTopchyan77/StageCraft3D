// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "Input/StageKeyBindings.h"
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

	/** Toolbar grid snap in cm, 0 = Off; always one of StageViewMath::GetGridSnapPresets (UStageSnappingComponent is the only writer). */
	float GetGridSnapCm() const { return GridSnapCm; }

	/** Toolbar angle snap in degrees, 0 = Off; always one of StageViewMath::GetAngleSnapPresets (UStageSnappingComponent is the only writer). */
	float GetAngleSnapDegrees() const { return AngleSnapDegrees; }

	/** A value that is not a preset is refused (returns false, nothing stored). Returns true if the stored value changed. Does not save. */
	bool SetGridSnapCm(float Cm);
	bool SetAngleSnapDegrees(float Degrees);

	/** Whether the viewport draws its grid (UStageViewportComponent is the only writer). */
	bool IsViewportGridVisible() const { return bViewportGridVisible; }

	/** Returns true if the stored value changed. Does not save. */
	bool SetViewportGridVisible(bool bVisible);

	/**
	 * Input preferences (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.2). UStageInputPreferencesSubsystem is
	 * the only writer. The key map is stored as overrides of the defaults and rebuilt through FStageKeyBindings::FromOverrides
	 * on every read, so a hand-edited ini can never load a conflict or an unbindable key. OutDropped (optional) counts the
	 * saved entries that were dropped. Call once the engine has registered its keys (any time after engine start-up).
	 */
	FStageKeyBindings GetKeyBindings(int32* OutDropped = nullptr) const;

	/** Returns true if the stored map changed. Does not save. */
	bool SetKeyBindings(const FStageKeyBindings& Bindings);

	/** Multiplier on the camera's look speed, StageInputPreferences::MinLookSensitivity..MaxLookSensitivity. */
	float GetLookSensitivity() const { return LookSensitivity; }

	/** Clamped; a non-finite value is refused (returns false). Returns true if the stored value changed. Does not save. */
	bool SetLookSensitivity(float Sensitivity);

	bool IsLookInverted() const { return bInvertLook; }

	/** Returns true if the stored value changed. Does not save. */
	bool SetLookInverted(bool bInverted);

	/** Camera speed (UE editor units: 1 = 20 m/s flight), clamped to StageCameraNavigation::MinCameraSpeed..MaxCameraSpeed. */
	float GetCameraSpeed() const { return CameraSpeed; }

	/** Clamped; a non-finite value is refused (returns false). Returns true if the stored value changed. Does not save. */
	bool SetCameraSpeed(float Speed);

	/** Look speed follows camera speed on a square-root curve (STATE.md #15) instead of the editor's fixed rate. */
	bool IsLookLinkedToCameraSpeed() const { return bLinkLookToCameraSpeed; }

	/** Returns true if the stored value changed. Does not save. */
	bool SetLookLinkedToCameraSpeed(bool bLinked);

	/** Whether the viewport toolbar is collapsed to its expand button (UStageViewportComponent is the only writer). */
	bool IsViewportToolbarCollapsed() const { return bViewportToolbarCollapsed; }

	/** Returns true if the stored value changed. Does not save. */
	bool SetViewportToolbarCollapsed(bool bCollapsed);

private:
	void ResetAudioToDefaults();
	void SanitizeAudio();
	void ResetInputToDefaults();
	void SanitizeInput();

	/** Bumped when the meaning of the audio fields changes; older saved values are then reset. */
	static constexpr int32 CurrentAudioSettingsVersion = 1;

	/** Bumped when the meaning of the input fields changes; older saved values are then reset. */
	static constexpr int32 CurrentInputSettingsVersion = 1;

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

	UPROPERTY(Config)
	float GridSnapCm = 50.f;

	UPROPERTY(Config)
	float AngleSnapDegrees = 15.f;

	UPROPERTY(Config)
	bool bViewportGridVisible = true;

	UPROPERTY(Config)
	bool bViewportToolbarCollapsed = false;

	/** Only the commands whose keys differ from the defaults (FStageKeyBindings::ToOverrides). */
	UPROPERTY(Config)
	TArray<FStageKeyBindingOverride> KeyBindingOverrides;

	UPROPERTY(Config)
	float LookSensitivity = 1.f;

	UPROPERTY(Config)
	bool bInvertLook = false;

	UPROPERTY(Config)
	float CameraSpeed = 1.f;

	UPROPERTY(Config)
	bool bLinkLookToCameraSpeed = false;

	UPROPERTY(Config)
	int32 InputSettingsVersion = 0;
};

namespace StageInputPreferences
{
	inline constexpr float MinLookSensitivity = 0.1f;
	inline constexpr float MaxLookSensitivity = 5.f;
	inline constexpr float DefaultLookSensitivity = 1.f;
}
