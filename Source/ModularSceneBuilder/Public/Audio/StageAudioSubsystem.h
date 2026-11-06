// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "StageAudioSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageAudioSettingsChanged);

/**
 * Central audio manager: owns the user's audio preferences.
 *
 * - The editor makes no interface sounds: clicks, selection, placement and refusals are silent (STATE.md #76; the feedback cues
 *   of #23 were removed on Gevor's request). Sound in StageCraft is the music the show follows (UStageAudioReactiveSubsystem).
 * - Master volume and Mute apply to every sound: a sound-mix class override on the project's default
 *   (master) sound class, applied to its children.
 * - Preferences are stored in UStageCraftUserSettings (single source of truth). Setters apply immediately,
 *   broadcast OnAudioSettingsChanged and save to disk debounced (~1 s; flushed on shutdown).
 *
 * Lives on the GameInstance: preferences and the mixer state span level travel. No tick; game thread only.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageAudioSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	float GetMasterVolume() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	bool IsMuted() const;

	/** Linear gain, clamped to [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void SetMasterVolume(float Volume);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void SetMuted(bool bMuted);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void ToggleMute();

	/**
	 * Whether StageCraft keeps sounding while another window has focus (UStageCraftUserSettings::IsAudioPlayedInBackground,
	 * Docs/ADR/0029 §2 A7). On by default: music and its analysis must not stop when the operator clicks into a browser.
	 */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	bool IsPlayedInBackground() const;

	/** Applies (FApp::SetUnfocusedVolumeMultiplier) and saves. Gameplay code changes it through AModularPlayerController. */
	void SetPlayedInBackground(bool bPlay);

	/** Any audio preference changed (from UI, console or code). Views read the getters back. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Audio")
	FOnStageAudioSettingsChanged OnAudioSettingsChanged;

private:
	void HandlePostLoadMap(class UWorld* LoadedWorld);
	void ApplyMasterGain();
	void ApplyBackgroundPlayback();
	void ReleaseMixer();
	void HandleSettingsChanged();
	void ScheduleSave();
	bool HandleSaveTimer(float DeltaTime);
	void FlushPendingSave();
	class UWorld* GetPlaybackWorld() const;

	/** Engine settings object when configured, else a transient fallback (values then do not persist). */
	UPROPERTY(Transient)
	TObjectPtr<class UStageCraftUserSettings> Settings = nullptr;

	/** Transient mix carrying the master override. Pushed once per audio device. */
	UPROPERTY(Transient)
	TObjectPtr<class USoundMix> UserVolumeMix = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USoundClass> MasterSoundClass = nullptr;

	/** Audio devices the mix has been pushed to, so re-applying after travel never stacks pushes. */
	TSet<uint32> MixPushedDeviceIds;

	FTSTicker::FDelegateHandle PendingSaveHandle;
	FDelegateHandle PostLoadMapHandle;

	/** The process-wide unfocused volume before this subsystem changed it, restored on Deinitialize (PIE shares it with the editor). */
	float UnfocusedVolumeBefore = 1.f;
};
