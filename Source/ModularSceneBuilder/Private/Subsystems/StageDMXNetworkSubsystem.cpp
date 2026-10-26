// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageDMXNetworkSubsystem.h"

#include "Engine/World.h"
#include "ModularSceneBuilder.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/StageNetworkDMXBridge.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageDMXNetworkSubsystem)

#define LOCTEXT_NAMESPACE "StageDMXNetwork"

bool UStageDMXNetworkSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Like the show it feeds: play sessions only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageDMXNetworkSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ShowControl = Collection.InitializeDependency<UShowControlSubsystem>();
	Patch = Collection.InitializeDependency<UStageDMXPatchSubsystem>();
	if (Patch)
	{
		Patch->OnPatchChanged.AddUniqueDynamic(this, &ThisClass::HandlePatchChanged);
	}
}

void UStageDMXNetworkSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StatusTimer);
		World->GetTimerManager().ClearTimer(UniverseTimer);
	}
	if (Patch)
	{
		Patch->OnPatchChanged.RemoveDynamic(this, &ThisClass::HandlePatchChanged);
	}
	StopListening();
	OnInputChanged.Clear();
	ShowControl = nullptr;
	Patch = nullptr;
	Super::Deinitialize();
}

void UStageDMXNetworkSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// The user left input on last time: open it again with this stage, and give the show to the console as before.
	if (GetSettings().bEnabled)
	{
		FText Error;
		if (StartListening(Error) && ShowControl)
		{
			ShowControl->SetControlSource(EStageControlSource::External);
		}
	}
}

FStageDMXInputSettings UStageDMXNetworkSubsystem::GetSettings() const
{
	const UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	return UserSettings ? UserSettings->GetDMXInputSettings() : FallbackSettings;
}

bool UStageDMXNetworkSubsystem::SetInputEnabled(bool bEnabled, FText& OutError)
{
	if (!bEnabled)
	{
		StopListening();
		SaveEnabled(false);
		SetState(EStageDMXInputState::Off, FText::GetEmpty());
		return true;
	}

	if (Bridge && Bridge->IsListening())
	{
		return true;
	}
	if (!StartListening(OutError))
	{
		return false;
	}
	SaveEnabled(true);
	return true;
}

bool UStageDMXNetworkSubsystem::ApplySettings(const FStageDMXInputSettings& Settings, FText& OutError)
{
	const bool bWasListening = Bridge && Bridge->IsListening();
	FStageDMXInputSettings Sanitized = StageDMXInput::Sanitize(Settings);
	Sanitized.bEnabled = bWasListening;
	if (UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get())
	{
		UserSettings->SetDMXInputSettings(Sanitized);
		UserSettings->SaveSettings();
	}
	else
	{
		FallbackSettings = Sanitized;
	}

	if (!bWasListening)
	{
		return true;
	}
	StopListening();
	if (!StartListening(OutError))
	{
		SaveEnabled(false);
		return false;
	}
	return true;
}

FStageDMXInputStatus UStageDMXNetworkSubsystem::GetStatus() const
{
	FStageDMXInputStatus Status = Bridge ? Bridge->GetStatus() : FStageDMXInputStatus();
	// The show may have replaced and stopped our bridge since; then nothing is listening, whatever was asked.
	Status.State = State == EStageDMXInputState::Listening && !(Bridge && Bridge->IsListening()) ? EStageDMXInputState::Off : State;
	Status.Error = LastError;
	return Status;
}

bool UStageDMXNetworkSubsystem::StartListening(FText& OutError)
{
	if (!ShowControl)
	{
		OutError = LOCTEXT("NoShow", "DMX input needs a stage with show control.");
		SetState(EStageDMXInputState::Failed, OutError);
		return false;
	}

	StopListening(); // A bridge the show stopped on its own (replaced) is let go first.
	FStageDMXReceiverConfig Config;
	Config.Settings = GetSettings();
	UStageNetworkDMXBridge* NewBridge = NewObject<UStageNetworkDMXBridge>(this);
	if (!NewBridge->Open(Config, OutError))
	{
		SetState(EStageDMXInputState::Failed, OutError);
		return false;
	}

	Bridge = NewBridge;
	Bridge->OnFramesApplied.BindUObject(this, &ThisClass::HandleFramesApplied);
	ShowControl->SetDMXBridge(Bridge);
	UpdateUniverses();
	SetState(EStageDMXInputState::Listening, FText::GetEmpty());
	return true;
}

void UStageDMXNetworkSubsystem::StopListening()
{
	if (!Bridge)
	{
		return;
	}
	Bridge->OnFramesApplied.Unbind();
	// Only remove the bridge from the show if it is still ours; someone may have installed another transport since.
	if (ShowControl && ShowControl->GetDMXBridge() == Bridge)
	{
		ShowControl->SetDMXBridge(nullptr); // Stops it, which closes the ports.
	}
	else
	{
		Bridge->Stop();
	}
	Bridge = nullptr;
}

void UStageDMXNetworkSubsystem::SaveEnabled(bool bEnabled)
{
	UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	FStageDMXInputSettings Settings = GetSettings();
	Settings.bEnabled = bEnabled;
	if (UserSettings)
	{
		if (UserSettings->SetDMXInputSettings(Settings))
		{
			UserSettings->SaveSettings();
		}
	}
	else
	{
		FallbackSettings = Settings;
	}
}

void UStageDMXNetworkSubsystem::SetState(EStageDMXInputState NewState, const FText& Error)
{
	State = NewState;
	LastError = Error;
	if (NewState != EStageDMXInputState::Listening)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(StatusTimer);
		}
	}
	OnInputChanged.Broadcast();
}

void UStageDMXNetworkSubsystem::HandlePatchChanged()
{
	// A re-patch of 200 fixtures reports 400 changes; the multicast groups and ArtPollReply follow once, on the next tick.
	UWorld* World = GetWorld();
	if (Bridge && World && !World->GetTimerManager().TimerExists(UniverseTimer))
	{
		UniverseTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::UpdateUniverses));
	}
}

void UStageDMXNetworkSubsystem::UpdateUniverses()
{
	UniverseTimer.Invalidate();
	if (Bridge && Patch)
	{
		Bridge->SetUniverses(Patch->GetPatchedUniverses());
	}
}

void UStageDMXNetworkSubsystem::HandleFramesApplied()
{
	UWorld* World = GetWorld();
	if (World && !World->GetTimerManager().IsTimerActive(StatusTimer))
	{
		// One status update per interval while frames arrive; the timer is not re-armed when the wire goes quiet.
		World->GetTimerManager().SetTimer(StatusTimer, FTimerDelegate::CreateWeakLambda(this, [this]() { OnInputChanged.Broadcast(); }),
			StatusIntervalSeconds, /*bLoop*/ false);
	}
}

#undef LOCTEXT_NAMESPACE
