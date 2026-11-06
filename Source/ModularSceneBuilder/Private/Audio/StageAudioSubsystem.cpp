// Copyright Epic Games, Inc. All Rights Reserved.

#include "Audio/StageAudioSubsystem.h"

#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "Audio/StageAudioTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "ModularSceneBuilder.h"
#include "Settings/StageCraftUserSettings.h"
#include "Sound/AudioSettings.h"
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

	ApplyMasterGain();
	UnfocusedVolumeBefore = FApp::GetUnfocusedVolumeMultiplier();
	ApplyBackgroundPlayback();

	// PIE worlds can own their own audio device, and travel may bring a new one; the override must follow.
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);

	UE_LOG(LogStageCraft, Log, TEXT("StageAudio: master %.2f, muted %s."), GetMasterVolume(), IsMuted() ? TEXT("yes") : TEXT("no"));
}

void UStageAudioSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	PostLoadMapHandle.Reset();

	FlushPendingSave();
	ReleaseMixer();
	FApp::SetUnfocusedVolumeMultiplier(UnfocusedVolumeBefore);

	Super::Deinitialize();
}

// --- Preferences ---

float UStageAudioSubsystem::GetMasterVolume() const
{
	return Settings ? Settings->GetMasterVolume() : 1.f;
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

bool UStageAudioSubsystem::IsPlayedInBackground() const
{
	return !Settings || Settings->IsAudioPlayedInBackground();
}

void UStageAudioSubsystem::SetPlayedInBackground(bool bPlay)
{
	if (Settings && Settings->SetAudioPlayedInBackground(bPlay))
	{
		HandleSettingsChanged();
	}
}

void UStageAudioSubsystem::ApplyBackgroundPlayback()
{
	// The engine's own default (BaseEngine [Audio] UnfocusedVolumeMultiplier=0) silences the app, and every tap on its output,
	// the moment another window has focus (Phase G0, ADR 0029 §2 A7).
	FApp::SetUnfocusedVolumeMultiplier(IsPlayedInBackground() ? 1.f : 0.f);
}

void UStageAudioSubsystem::HandleSettingsChanged()
{
	ApplyMasterGain();
	ApplyBackgroundPlayback();
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
		UE_LOG(LogStageCraft, Log, TEXT("StageAudio: preferences saved (master %.2f, muted %s)."),
			GetMasterVolume(), IsMuted() ? TEXT("yes") : TEXT("no"));
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
