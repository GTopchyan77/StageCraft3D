// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "GameplayTagContainer.h"
#include "StageAudioSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageAudioSettingsChanged);

/**
 * Central audio manager: plays feedback cues and owns the user's audio preferences.
 *
 * - Cues: PlayCue(StageCraft.Sound.*) looks the tag up in UStageAudioDeveloperSettings. Sounds stream in
 *   asynchronously at startup; a cue whose sound is not loaded yet is skipped (logged once).
 * - Master volume and Mute apply to every sound: a sound-mix class override on the project's default
 *   (master) sound class, applied to its children. Effects volume is a per-play gain on cues, which is
 *   why every interface/interaction sound must go through PlayCue.
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

	/** Plays a feedback cue in 2D. Silent while muted, for unknown tags, unloaded sounds or a too-soon retrigger. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void PlayCue(FGameplayTag CueTag);

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	float GetMasterVolume() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	float GetEffectsVolume() const;

	UFUNCTION(BlueprintPure, Category = "StageCraft|Audio")
	bool IsMuted() const;

	/** Linear gain, clamped to [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void SetMasterVolume(float Volume);

	/** Linear gain for feedback cues, clamped to [0, 1]. */
	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void SetEffectsVolume(float Volume);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void SetMuted(bool bMuted);

	UFUNCTION(BlueprintCallable, Category = "StageCraft|Audio")
	void ToggleMute();

	/** Any audio preference changed (from UI, console or code). Views read the getters back. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Audio")
	FOnStageAudioSettingsChanged OnAudioSettingsChanged;

private:
	struct FLoadedCue
	{
		TSoftObjectPtr<class USoundBase> Sound;
		float VolumeScale = 1.f;
		float MinRetriggerInterval = 0.f;
		double LastPlayedTime = -UE_BIG_NUMBER;
		bool bReportedMissing = false;
	};

	void BuildCueTable();
	void RequestCueSounds();
	void HandlePostLoadMap(class UWorld* LoadedWorld);
	void ApplyMasterGain();
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

	TMap<FGameplayTag, FLoadedCue> Cues;
	TSharedPtr<struct FStreamableHandle> CueLoadHandle;

	/** Audio devices the mix has been pushed to, so re-applying after travel never stacks pushes. */
	TSet<uint32> MixPushedDeviceIds;

	FTSTicker::FDelegateHandle PendingSaveHandle;
	FDelegateHandle PostLoadMapHandle;
};
