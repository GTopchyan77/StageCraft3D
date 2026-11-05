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

namespace StageDMXNetworkSubsystem
{
	/** Whether only the muted universes differ: then a running output keeps its socket and sequence numbers. */
	bool DiffersOnlyInMutes(const FStageDMXOutputSettings& A, const FStageDMXOutputSettings& B)
	{
		FStageDMXOutputSettings Left = A;
		FStageDMXOutputSettings Right = B;
		Left.MutedUniverses.Reset();
		Right.MutedUniverses.Reset();
		Left.bEnabled = Right.bEnabled;
		return Left == Right;
	}
}

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
	if (ShowControl)
	{
		ShowControl->OnControlSourceChanged.AddUniqueDynamic(this, &ThisClass::HandleControlSourceChanged);
	}
}

void UStageDMXNetworkSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StatusTimer);
		World->GetTimerManager().ClearTimer(OutputStatusTimer);
		World->GetTimerManager().ClearTimer(UniverseTimer);
	}
	if (Patch)
	{
		Patch->OnPatchChanged.RemoveDynamic(this, &ThisClass::HandlePatchChanged);
	}
	if (ShowControl)
	{
		ShowControl->OnControlSourceChanged.RemoveDynamic(this, &ThisClass::HandleControlSourceChanged);
	}
	StopSending();
	StopListening();
	OnInputChanged.Clear();
	OnOutputChanged.Clear();
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
	// And output: the show sends again (while it is Internal).
	if (GetOutputSettings().bEnabled)
	{
		FText Error;
		StartSending(Error);
	}
}

// --- Bridge ---

UStageNetworkDMXBridge* UStageDMXNetworkSubsystem::EnsureBridge()
{
	ForgetStoppedBridge();
	if (!ShowControl)
	{
		return nullptr;
	}
	if (!Bridge)
	{
		Bridge = NewObject<UStageNetworkDMXBridge>(this);
		Bridge->OnFramesApplied.BindUObject(this, &ThisClass::HandleFramesApplied);
		ShowControl->SetDMXBridge(Bridge);
	}
	return Bridge;
}

void UStageDMXNetworkSubsystem::ReleaseBridgeIfIdle()
{
	if (!Bridge || !Bridge->IsIdle())
	{
		return;
	}
	Bridge->OnFramesApplied.Unbind();
	// Only remove the bridge from the show if it is still ours; someone may have installed another transport since.
	if (ShowControl && ShowControl->GetDMXBridge() == Bridge)
	{
		ShowControl->SetDMXBridge(nullptr);
	}
	Bridge = nullptr;
}

void UStageDMXNetworkSubsystem::ForgetStoppedBridge()
{
	// The show replaced our bridge (and stopped it, closing both directions): start from a fresh one.
	if (Bridge && !(ShowControl && ShowControl->GetDMXBridge() == Bridge))
	{
		Bridge->OnFramesApplied.Unbind();
		Bridge->Stop();
		Bridge = nullptr;
	}
}

// --- Input ---

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

	if (Bridge && Bridge->IsListening() && ShowControl && ShowControl->GetDMXBridge() == Bridge)
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
	const bool bWasSending = Bridge && Bridge->IsSending();
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

	bool bOk = true;
	if (bWasListening)
	{
		StopListening();
		if (!StartListening(OutError))
		{
			SaveEnabled(false);
			bOk = false;
		}
	}
	// The output sends from the same card with the same universe numbering.
	if (bWasSending)
	{
		StopSending();
		FText OutputError;
		if (!StartSending(OutputError))
		{
			SaveOutputEnabled(false);
			OutError = OutError.IsEmpty() ? OutputError : OutError;
			bOk = false;
		}
	}
	return bOk;
}

FStageDMXInputStatus UStageDMXNetworkSubsystem::GetStatus() const
{
	const bool bListening = Bridge && Bridge->IsListening() && ShowControl && ShowControl->GetDMXBridge() == Bridge;
	FStageDMXInputStatus Status = bListening ? Bridge->GetInputStatus() : FStageDMXInputStatus();
	// The show may have replaced and stopped our bridge since; then nothing is listening, whatever was asked.
	Status.State = State == EStageDMXInputState::Listening && !bListening ? EStageDMXInputState::Off : State;
	Status.Error = LastError;
	return Status;
}

bool UStageDMXNetworkSubsystem::StartListening(FText& OutError)
{
	UStageNetworkDMXBridge* Target = EnsureBridge();
	if (!Target)
	{
		OutError = LOCTEXT("NoShow", "DMX input needs a stage with show control.");
		SetState(EStageDMXInputState::Failed, OutError);
		return false;
	}

	Target->CloseInput();
	FStageDMXReceiverConfig Config;
	Config.Settings = GetSettings();
	Config.IgnoredSource = GetOutputSettings().Cid; // Our own sACN output, looped back to this PC, is not input.
	if (!Target->OpenInput(Config, OutError))
	{
		ReleaseBridgeIfIdle();
		SetState(EStageDMXInputState::Failed, OutError);
		return false;
	}

	UpdateUniverses();
	SetState(EStageDMXInputState::Listening, FText::GetEmpty());
	return true;
}

void UStageDMXNetworkSubsystem::StopListening()
{
	ForgetStoppedBridge();
	if (Bridge)
	{
		Bridge->CloseInput();
		ReleaseBridgeIfIdle();
	}
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

// --- Output ---

FStageDMXOutputSettings UStageDMXNetworkSubsystem::GetOutputSettings() const
{
	const UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	return UserSettings ? UserSettings->GetDMXOutputSettings() : FallbackOutputSettings;
}

bool UStageDMXNetworkSubsystem::SetOutputEnabled(bool bEnabled, FText& OutError)
{
	if (!bEnabled)
	{
		StopSending();
		SaveOutputEnabled(false);
		SetOutputState(EStageDMXOutputState::Off, FText::GetEmpty());
		return true;
	}

	if (Bridge && Bridge->IsSending() && ShowControl && ShowControl->GetDMXBridge() == Bridge)
	{
		return true;
	}
	if (!StartSending(OutError))
	{
		return false;
	}
	SaveOutputEnabled(true);
	return true;
}

bool UStageDMXNetworkSubsystem::ApplyOutputSettings(const FStageDMXOutputSettings& Settings, FText& OutError)
{
	const FStageDMXOutputSettings Before = GetOutputSettings();
	const bool bWasSending = Bridge && Bridge->IsSending();
	FStageDMXOutputSettings Sanitized = StageDMXOutput::Sanitize(Settings);
	Sanitized.bEnabled = bWasSending;
	Sanitized.Cid = Before.Cid;
	SaveOutputSettings(Sanitized);

	if (!bWasSending)
	{
		OnOutputChanged.Broadcast(); // The panel shows the new targets even while off.
		return true;
	}
	if (StageDMXNetworkSubsystem::DiffersOnlyInMutes(Before, Sanitized))
	{
		UpdateUniverses();
		OnOutputChanged.Broadcast();
		return true;
	}
	StopSending();
	if (!StartSending(OutError))
	{
		SaveOutputEnabled(false);
		return false;
	}
	return true;
}

FStageDMXOutputStatus UStageDMXNetworkSubsystem::GetOutputStatus() const
{
	const bool bSending = Bridge && Bridge->IsSending() && ShowControl && ShowControl->GetDMXBridge() == Bridge;
	FStageDMXOutputStatus Status = bSending ? Bridge->GetOutputStatus() : FStageDMXOutputStatus();
	Status.State = OutputState == EStageDMXOutputState::Sending && !bSending ? EStageDMXOutputState::Off : OutputState;
	Status.Error = LastOutputError;
	return Status;
}

bool UStageDMXNetworkSubsystem::StartSending(FText& OutError)
{
	UStageNetworkDMXBridge* Target = EnsureBridge();
	if (!Target)
	{
		OutError = LOCTEXT("NoShowForOutput", "DMX output needs a stage with show control.");
		SetOutputState(EStageDMXOutputState::Failed, OutError);
		return false;
	}

	Target->CloseOutput();
	FStageDMXSenderConfig Config;
	Config.Settings = GetOutputSettings();
	Config.Numbering = GetSettings();
	if (!Target->OpenOutput(Config, OutError))
	{
		ReleaseBridgeIfIdle();
		SetOutputState(EStageDMXOutputState::Failed, OutError);
		return false;
	}

	ShowControl->DMXOutputRateHz = static_cast<float>(Config.Settings.RateHz);
	UpdateUniverses();
	SetOutputState(EStageDMXOutputState::Sending, FText::GetEmpty());
	if (UWorld* World = GetWorld())
	{
		// Packet counters move all the time while sending; listeners hear about them twice a second, and only while sending.
		World->GetTimerManager().SetTimer(OutputStatusTimer, FTimerDelegate::CreateWeakLambda(this, [this]() { OnOutputChanged.Broadcast(); }),
			StatusIntervalSeconds, /*bLoop*/ true);
	}
	return true;
}

void UStageDMXNetworkSubsystem::StopSending()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(OutputStatusTimer);
	}
	ForgetStoppedBridge();
	if (Bridge)
	{
		Bridge->CloseOutput();
		ReleaseBridgeIfIdle();
	}
}

void UStageDMXNetworkSubsystem::SaveOutputEnabled(bool bEnabled)
{
	FStageDMXOutputSettings Settings = GetOutputSettings();
	Settings.bEnabled = bEnabled;
	SaveOutputSettings(Settings);
}

void UStageDMXNetworkSubsystem::SaveOutputSettings(const FStageDMXOutputSettings& Settings)
{
	if (UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get())
	{
		// Saved even when nothing changed but a fresh Cid that was never written: receivers keep seeing the same source.
		UserSettings->SetDMXOutputSettings(Settings);
		UserSettings->SaveSettings();
	}
	else
	{
		FallbackOutputSettings = StageDMXOutput::Sanitize(Settings);
		FallbackOutputSettings.Cid = Settings.Cid;
	}
}

void UStageDMXNetworkSubsystem::SetOutputState(EStageDMXOutputState NewState, const FText& Error)
{
	OutputState = NewState;
	LastOutputError = Error;
	if (NewState != EStageDMXOutputState::Sending)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(OutputStatusTimer);
		}
	}
	OnOutputChanged.Broadcast();
}

// --- Patch ---

void UStageDMXNetworkSubsystem::HandlePatchChanged()
{
	// A re-patch of 200 fixtures reports 400 changes; the multicast groups, ArtPollReply and sent universes follow once, on the next tick.
	UWorld* World = GetWorld();
	if (Bridge && World && !World->GetTimerManager().TimerExists(UniverseTimer))
	{
		UniverseTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::UpdateUniverses));
	}
}

void UStageDMXNetworkSubsystem::UpdateUniverses()
{
	UniverseTimer.Invalidate();
	if (!Bridge || !Patch)
	{
		return;
	}
	const TArray<int32> Patched = Patch->GetPatchedUniverses();
	if (Bridge->IsListening())
	{
		Bridge->SetInputUniverses(Patched);
	}
	if (Bridge->IsSending())
	{
		const FStageDMXOutputSettings Settings = GetOutputSettings();
		const bool bPaused = ShowControl && ShowControl->GetControlSource() == EStageControlSource::External;
		TArray<int32> Sent;
		Sent.Reserve(Patched.Num());
		for (const int32 Universe : Patched)
		{
			if (!bPaused && !StageDMXOutput::IsMuted(Settings, Universe))
			{
				Sent.Add(Universe);
			}
		}
		Bridge->SetOutputUniverses(Sent);
		// A universe that just joined (or every universe, when output just opened) is sent at once, not at its next change.
		if (ShowControl)
		{
			ShowControl->RefreshDMXOutput();
		}
	}
}

void UStageDMXNetworkSubsystem::HandleControlSourceChanged(EStageControlSource NewSource)
{
	if (Bridge && Bridge->IsSending())
	{
		UpdateUniverses();
		OnOutputChanged.Broadcast();
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
