// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioSubsystem.h"

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "Audio/StageAudioDeveloperSettings.h"
#include "Audio/StageAudioTypes.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "ModularSceneBuilder.h"
#include "Settings/StageCraftUserSettings.h"
#include "Sound/AudioSettings.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAudioSubsystem)

namespace StageAudioSubsystem
{
	// Long enough that a slider drag writes the ini once, short enough that a crash loses little.
	constexpr float SaveDebounceSeconds = 1.f;
}

void UStageAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Settings = UStageCraftUserSettings::Get();
	if (!Settings)
	{
		UE_LOG(LogStageCraft, Error, TEXT("StageAudio: GameUserSettingsClassName is not StageCraftUserSettings; audio preferences work for this run but are not saved."));
		Settings = NewObject<UStageCraftUserSettings>(this);
	}

	MasterSoundClass = GetDefault<UAudioSettings>()->GetDefaultSoundClass();
	if (!MasterSoundClass)
	{
		UE_LOG(LogStageCraft, Error, TEXT("StageAudio: the project has no default sound class; Master volume and Mute cannot be applied."));
	}

	// Transient: the override is computed from the user settings, not authored content.
	UserVolumeMix = NewObject<USoundMix>(this, TEXT("StageUserVolumeMix"), RF_Transient);
	UserVolumeMix->FadeInTime = 0.f;
	UserVolumeMix->FadeOutTime = 0.f;
	UserVolumeMix->Duration = -1.f;

	BuildCueTable();
	RequestCueSounds();
	ApplyMasterGain();

	// PIE worlds can own their own audio device, and travel may bring a new one; the override must follow.
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);

	UE_LOG(LogStageCraft, Log, TEXT("StageAudio: master %.2f, effects %.2f, muted %s, %d cue(s) configured."),
		GetMasterVolume(), GetEffectsVolume(), IsMuted() ? TEXT("yes") : TEXT("no"), Cues.Num());
}

void UStageAudioSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	PostLoadMapHandle.Reset();

	FlushPendingSave();
	ReleaseMixer();

	if (CueLoadHandle.IsValid())
	{
		CueLoadHandle->CancelHandle();
		CueLoadHandle.Reset();
	}
	Cues.Reset();

	Super::Deinitialize();
}

// --- Cues ---

void UStageAudioSubsystem::BuildCueTable()
{
	Cues.Reset();
	for (const FStageAudioCue& Cue : GetDefault<UStageAudioDeveloperSettings>()->Cues)
	{
		if (!Cue.CueTag.IsValid())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageAudio: a cue in Project Settings > StageCraft Audio has no tag; it is ignored."));
			continue;
		}
		if (Cues.Contains(Cue.CueTag))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageAudio: cue %s is configured twice; the first entry wins."), *Cue.CueTag.ToString());
			continue;
		}

		FLoadedCue& Loaded = Cues.Add(Cue.CueTag);
		Loaded.Sound = Cue.Sound;
		Loaded.VolumeScale = Cue.VolumeScale;
		Loaded.MinRetriggerInterval = FMath::Max(Cue.MinRetriggerInterval, 0.f);
	}
}

void UStageAudioSubsystem::RequestCueSounds()
{
	TArray<FSoftObjectPath> Paths;
	for (const TPair<FGameplayTag, FLoadedCue>& Pair : Cues)
	{
		if (!Pair.Value.Sound.IsNull())
		{
			Paths.AddUnique(Pair.Value.Sound.ToSoftObjectPath());
		}
	}
	if (Paths.IsEmpty())
	{
		return;
	}

	// The handle keeps the sounds resident for the whole run; they are small one-shot waves.
	CueLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths, FStreamableDelegate::CreateWeakLambda(this, [this]()
	{
		UE_LOG(LogStageCraft, Log, TEXT("StageAudio: feedback sounds loaded."));
	}));
}

void UStageAudioSubsystem::PlayCue(FGameplayTag CueTag)
{
	FLoadedCue* Cue = Cues.Find(CueTag);
	if (!Cue || IsMuted())
	{
		return;
	}

	USoundBase* Sound = Cue->Sound.Get();
	if (!Sound)
	{
		if (!Cue->bReportedMissing)
		{
			Cue->bReportedMissing = true;
			UE_LOG(LogStageCraft, Warning, TEXT("StageAudio: cue %s has no loaded sound (%s); it stays silent."), *CueTag.ToString(), *Cue->Sound.ToString());
		}
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - Cue->LastPlayedTime < Cue->MinRetriggerInterval)
	{
		return;
	}

	const float Gain = StageAudioMath::ComputeCueGain(GetEffectsVolume(), Cue->VolumeScale);
	UWorld* World = GetPlaybackWorld();
	if (Gain <= 0.f || !World)
	{
		return;
	}

	Cue->LastPlayedTime = Now;
	UE_LOG(LogStageCraft, Verbose, TEXT("StageAudio: cue %s at gain %.2f."), *CueTag.ToString(), Gain);
	// UI sound: keeps playing while the game is paused, never spatialised.
	UGameplayStatics::PlaySound2D(World, Sound, Gain, 1.f, 0.f, nullptr, nullptr, /*bIsUISound*/ true);
}

// --- Preferences ---

float UStageAudioSubsystem::GetMasterVolume() const
{
	return Settings ? Settings->GetMasterVolume() : 1.f;
}

float UStageAudioSubsystem::GetEffectsVolume() const
{
	return Settings ? Settings->GetEffectsVolume() : 1.f;
}

bool UStageAudioSubsystem::IsMuted() const
{
	return Settings && Settings->IsAudioMuted();
}

void UStageAudioSubsystem::SetMasterVolume(float Volume)
{
	if (Settings && Settings->SetMasterVolume(Volume))
	{
		HandleSettingsChanged();
	}
}

void UStageAudioSubsystem::SetEffectsVolume(float Volume)
{
	if (Settings && Settings->SetEffectsVolume(Volume))
	{
		HandleSettingsChanged();
	}
}

void UStageAudioSubsystem::SetMuted(bool bMuted)
{
	if (Settings && Settings->SetAudioMuted(bMuted))
	{
		HandleSettingsChanged();
	}
}

void UStageAudioSubsystem::ToggleMute()
{
	SetMuted(!IsMuted());
}

void UStageAudioSubsystem::HandleSettingsChanged()
{
	ApplyMasterGain();
	ScheduleSave();
	OnAudioSettingsChanged.Broadcast();
}

// --- Mixer ---

UWorld* UStageAudioSubsystem::GetPlaybackWorld() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetWorld() : nullptr;
}

void UStageAudioSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (LoadedWorld && LoadedWorld == GetPlaybackWorld())
	{
		ApplyMasterGain();
	}
}

void UStageAudioSubsystem::ApplyMasterGain()
{
	if (!UserVolumeMix || !MasterSoundClass)
	{
		return;
	}

	const UWorld* World = GetPlaybackWorld();
	FAudioDeviceHandle Device = World ? World->GetAudioDevice() : FAudioDeviceHandle();
	if (!Device.IsValid() && GEngine)
	{
		Device = GEngine->GetMainAudioDevice();
	}
	if (!Device.IsValid())
	{
		return; // No audio (e.g. -nosound); nothing to apply.
	}

	const uint32 DeviceId = Device.GetDeviceID();
	if (!MixPushedDeviceIds.Contains(DeviceId))
	{
		Device->PushSoundMixModifier(UserVolumeMix);
		MixPushedDeviceIds.Add(DeviceId);
	}

	const float Gain = StageAudioMath::ComputeMasterGain(GetMasterVolume(), IsMuted());
	Device->SetSoundMixClassOverride(UserVolumeMix, MasterSoundClass, Gain, 1.f, 0.f, /*bApplyToChildren*/ true);
}

void UStageAudioSubsystem::ReleaseMixer()
{
	// PIE shares devices with the editor: leaving the override behind would keep the editor muted.
	FAudioDeviceManager* DeviceManager = GEngine ? GEngine->GetAudioDeviceManager() : nullptr;
	for (const uint32 DeviceId : MixPushedDeviceIds)
	{
		FAudioDevice* Device = DeviceManager ? DeviceManager->GetAudioDeviceRaw(DeviceId) : nullptr;
		if (Device && UserVolumeMix)
		{
			if (MasterSoundClass)
			{
				Device->ClearSoundMixClassOverride(UserVolumeMix, MasterSoundClass, 0.f);
			}
			Device->PopSoundMixModifier(UserVolumeMix);
		}
	}
	MixPushedDeviceIds.Reset();
}

// --- Persistence ---

void UStageAudioSubsystem::ScheduleSave()
{
	if (PendingSaveHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PendingSaveHandle);
	}
	PendingSaveHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &ThisClass::HandleSaveTimer), StageAudioSubsystem::SaveDebounceSeconds);
}

bool UStageAudioSubsystem::HandleSaveTimer(float DeltaTime)
{
	PendingSaveHandle.Reset();
	if (Settings && Settings == UStageCraftUserSettings::Get())
	{
		Settings->SaveSettings();
		UE_LOG(LogStageCraft, Log, TEXT("StageAudio: preferences saved (master %.2f, effects %.2f, muted %s)."),
			GetMasterVolume(), GetEffectsVolume(), IsMuted() ? TEXT("yes") : TEXT("no"));
	}
	return false; // One-shot.
}

void UStageAudioSubsystem::FlushPendingSave()
{
	if (!PendingSaveHandle.IsValid())
	{
		return;
	}
	FTSTicker::GetCoreTicker().RemoveTicker(PendingSaveHandle);
	HandleSaveTimer(0.f);
}
