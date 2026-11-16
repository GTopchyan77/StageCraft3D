// Copyright Epic Games, Inc. All Rights Reserved.

#include "Remote/StageWebRemoteSubsystem.h"

#include "Algo/Count.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IO/StageAtomicFile.h"
#include "IO/StageCraftPaths.h"
#include "Misc/FileHelper.h"
#include "IPAddress.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Remote/StageRemoteMapping.h"
#include "Remote/StageWebRemoteProtocol.h"
#include "Remote/StageWebRemoteServer.h"
#include "Settings/StageCraftUserSettings.h"
#include "Show/ShowControlTypes.h"
#include "SocketSubsystem.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageWebRemoteSubsystem)

#define LOCTEXT_NAMESPACE "StageWebRemote"

UStageWebRemoteSubsystem::UStageWebRemoteSubsystem() = default;

UStageWebRemoteSubsystem::~UStageWebRemoteSubsystem() = default;

bool UStageWebRemoteSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UStageWebRemoteSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UShowControlSubsystem>();
}

void UStageWebRemoteSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlushTimer);
		World->GetTimerManager().ClearTimer(StateTimer);
		World->GetTimerManager().ClearTimer(StatusTimer);
	}
	OnStatusChanged.Clear();
	Close();
	PendingCommands.Reset();
	CommandHandler.Unbind();
	Super::Deinitialize();
}

void UStageWebRemoteSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FText Error;
	Open(Error);
	BroadcastStatus();
}

const FStageWebRemoteSettings& UStageWebRemoteSubsystem::GetSettings() const
{
	static const FStageWebRemoteSettings Defaults;
	const UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	return UserSettings ? UserSettings->GetWebRemoteSettings() : Defaults;
}

bool UStageWebRemoteSubsystem::ApplySettings(const FStageWebRemoteSettings& Settings, FText& OutError)
{
	UStageCraftUserSettings* UserSettings = UStageCraftUserSettings::Get();
	if (!UserSettings)
	{
		OutError = LOCTEXT("NoSettings", "The settings of this PC are not available.");
		return false;
	}
	if (UserSettings->SetWebRemoteSettings(Settings))
	{
		UserSettings->SaveSettings();
	}

	Close();
	OutError = FText::GetEmpty();
	const bool bOpened = Open(OutError);
	BroadcastStatus();
	return bOpened;
}

bool UStageWebRemoteSubsystem::RenewPin()
{
	if (!Server)
	{
		return false;
	}
	Server->RenewPin();
	RefreshPairingStatus();
	BroadcastStatus();
	return true;
}

bool UStageWebRemoteSubsystem::UnpairAll()
{
	if (!Server)
	{
		// Forgetting is always allowed, also while the server is off: the saved list would otherwise come back at the next start.
		if (!CanUseDevicesFile())
		{
			return true;
		}
		IFileManager::Get().Delete(*GetDevicesFile(), /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		SavedDevicesJson.Reset();
		UE_LOG(LogStageCraft, Log, TEXT("Web Remote: the remembered devices were forgotten."));
		return true;
	}
	Server->UnpairAll();
	RefreshPairingStatus();
	UE_LOG(LogStageCraft, Log, TEXT("Web Remote: every device was unpaired."));
	BroadcastStatus();
	return true;
}

bool UStageWebRemoteSubsystem::ForgetDevice(const FString& DeviceId)
{
	if (!Server || !Server->ForgetDevice(DeviceId))
	{
		return false;
	}
	RefreshPairingStatus();
	UE_LOG(LogStageCraft, Log, TEXT("Web Remote: a device was forgotten."));
	BroadcastStatus();
	return true;
}

FString UStageWebRemoteSubsystem::GetDevicesFile()
{
	return FPaths::Combine(StageCraftPaths::UserRoot(), TEXT("WebRemoteDevices.json"));
}

void UStageWebRemoteSubsystem::ClearCommandHandler(const UObject* Owner)
{
	if (Owner && CommandHandler.IsBoundToObject(Owner))
	{
		CommandHandler.Unbind();
	}
}

int32 UStageWebRemoteSubsystem::GetListeningPort() const
{
	return Server ? Server->GetPort() : 0;
}

FString UStageWebRemoteSubsystem::GetPageFile()
{
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("Source/ModularSceneBuilder/Resources/WebRemote/index.html"));
}

bool UStageWebRemoteSubsystem::Open(FText& OutError)
{
	Status.LastProblem = FText::GetEmpty();
	const FStageWebRemoteSettings& Settings = GetSettings();
	const FString DevicesFile = GetDevicesFile();
	SavedDevicesJson.Reset();
	if (!Settings.bRememberDevices && CanUseDevicesFile())
	{
		// Before the Off check: switching remembering off must remove the hashes even while the Web Remote itself is off.
		IFileManager::Get().Delete(*DevicesFile, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
	}
	if (Settings.Scope == EStageWebRemoteScope::Off)
	{
		return true;
	}

	TFunction<FString()> MakePin;
#if WITH_DEV_AUTOMATION_TESTS
	if (!PinForTesting.IsEmpty())
	{
		MakePin = [Pin = PinForTesting]() { return Pin; };
	}
#endif

	TArray<StageWebRemote::FRememberedDevice> Remembered;
	if (Settings.bRememberDevices && CanUseDevicesFile() && IFileManager::Get().FileSize(*DevicesFile) > 0)
	{
		FString Json;
		if (IFileManager::Get().FileSize(*DevicesFile) > StageWebRemote::MaxDevicesFileBytes || !FFileHelper::LoadFileToString(Json, *DevicesFile)
			|| !StageWebRemote::ReadDevicesJson(Json, Remembered))
		{
			// Pairing again is the worst case, so a damaged file is ignored, not repaired.
			UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: %s cannot be read; remembered devices must pair again."), *DevicesFile);
			Remembered.Reset();
		}
		else
		{
			SavedDevicesJson = Json;
		}
	}

	const bool bLoopbackOnly = Settings.Scope == EStageWebRemoteScope::ThisComputer;
	TUniquePtr<FStageWebRemoteServer> NewServer = MakeUnique<FStageWebRemoteServer>();
	TWeakObjectPtr<UStageWebRemoteSubsystem> WeakThis(this);
	if (!NewServer->Start(bLoopbackOnly, static_cast<uint16>(Settings.Port), GetPageFile(), [WeakThis]()
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis]()
			{
				if (UStageWebRemoteSubsystem* Self = WeakThis.Get())
				{
					Self->DrainEvents();
				}
			});
		}, OutError, MoveTemp(MakePin), Remembered))
	{
		Status.LastProblem = OutError;
		UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: could not listen on port %d: %s"), Settings.Port, *OutError.ToString());
		return false;
	}

	Server = MoveTemp(NewServer);
	Status.bListening = true;
	Status.Addresses = FindAddresses(Settings.Scope, Server->GetPort());
	Status.ConnectedPages = 0;
	OpenDeviceIds.Reset();
	LastSentRefusal.Reset();
	RefreshPairingStatus();
	BindShow();
	MarkStateDirty(true);
	SendState();
	UE_LOG(LogStageCraft, Log, TEXT("Web Remote: listening on port %d (%s)."), Server->GetPort(), bLoopbackOnly ? TEXT("this PC only") : TEXT("every network"));
	return true;
}

void UStageWebRemoteSubsystem::Close()
{
	UnbindShow();
	if (Server)
	{
		// The snapshot is taken after the worker has stopped, so a pairing or unpair still in flight is neither lost nor revived.
		TArray<StageWebRemote::FRememberedDevice> FinalDevices;
		Server->Stop(&FinalDevices);
		if (GetSettings().bRememberDevices)
		{
			SaveDevicesIfChanged(StageWebRemote::WriteDevicesJsonOrEmpty(FinalDevices));
		}
		Server.Reset();
		UE_LOG(LogStageCraft, Log, TEXT("Web Remote: closed."));
	}
	Status.bListening = false;
	Status.Addresses.Reset();
	Status.Pin.Reset();
	Status.PairedDevices = 0;
	Status.Devices.Reset();
	Status.ConnectedPages = 0;
	Status.PairingLockedUntil = 0.0;
	OpenDeviceIds.Reset();
}

void UStageWebRemoteSubsystem::DrainEvents()
{
	if (!Server)
	{
		return;
	}
	bool bPairingChanged = false;
	const int32 Dropped = Server->Drain([this, &bPairingChanged](const FStageWebRemoteEvent& Event)
	{
		switch (Event.Kind)
		{
		case FStageWebRemoteEvent::EKind::Command:
			ReceiveCommand(Event.Path, Event.Value);
			break;
		case FStageWebRemoteEvent::EKind::Paired:
		case FStageWebRemoteEvent::EKind::Unpaired:
		case FStageWebRemoteEvent::EKind::PairingLocked:
			bPairingChanged = true;
			break;
		case FStageWebRemoteEvent::EKind::PagesChanged:
			Status.ConnectedPages = Event.Pages;
			OpenDeviceIds = Event.OpenDeviceIds;
			bPairingChanged = true;
			break;
		}
	});
	if (Dropped > 0)
	{
		Status.Dropped += Dropped;
		MarkStatusDirty();
	}
	if (bPairingChanged)
	{
		RefreshPairingStatus();
		BroadcastStatus();
	}
}

void UStageWebRemoteSubsystem::ReceiveCommand(const FString& Path, TOptional<float> Value)
{
	++Status.Received;
	const FString ShownPath = Path.Left(StageRemote::MaxOscAddressLength);
	Status.LastInput = Value.IsSet() ? FString::Printf(TEXT("%s %.2f"), *ShownPath, Value.GetValue()) : ShownPath;

	FStageRemoteCommand Command;
	if (StageRemote::ParseOsc(Path, Value, Command))
	{
		Command.Source = EStageRemoteSource::Web;
		Enqueue(Command);
	}
	else
	{
		++Status.Ignored;
	}
	MarkStatusDirty();
}

void UStageWebRemoteSubsystem::Enqueue(const FStageRemoteCommand& Command)
{
	if (!StageRemote::Enqueue(PendingCommands, Command))
	{
		++Status.Dropped;
		return;
	}
	UWorld* World = GetWorld();
	if (World && !FlushTimer.IsValid())
	{
		FlushTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::FlushCommands));
	}
}

void UStageWebRemoteSubsystem::FlushCommands()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FlushTimer);
	}
	FlushTimer.Invalidate();

	const TArray<FStageRemoteCommand> Commands = MoveTemp(PendingCommands);
	PendingCommands.Reset();
	for (const FStageRemoteCommand& Command : Commands)
	{
		const FStageEconomyResultInfo Verdict = Dispatch(Command);
		if (Verdict.IsSuccess())
		{
			++Status.Applied;
			LastSentRefusal.Reset();
			continue;
		}
		++Status.Refused;
		Status.LastRefusal = Verdict.Message;
		const FString Reason = Verdict.Message.ToString();
		if (Server && Reason != LastSentRefusal)
		{
			LastSentRefusal = Reason;
			Server->Broadcast(StageWebRemote::MakeRefusedMessage(Reason));
		}
	}
	if (!Commands.IsEmpty())
	{
		MarkStatusDirty();
	}
}

FStageEconomyResultInfo UStageWebRemoteSubsystem::Dispatch(const FStageRemoteCommand& Command)
{
	if (!CommandHandler.IsBound())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoHandler", "There is no stage to control yet."));
	}
	return CommandHandler.Execute(Command);
}

void UStageWebRemoteSubsystem::BindShow()
{
	UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	if (!Show || BoundShow.Get() == Show)
	{
		return;
	}
	Show->OnCueStarted.AddUniqueDynamic(this, &ThisClass::HandleShowCueStarted);
	Show->OnDeskStateChanged.AddUniqueDynamic(this, &ThisClass::HandleShowChanged);
	Show->OnPlaybacksChanged.AddUniqueDynamic(this, &ThisClass::HandleShowChanged);
	Show->OnCueListChanged.AddUniqueDynamic(this, &ThisClass::HandleShowCueListChanged);
	BoundShow = Show;
}

void UStageWebRemoteSubsystem::UnbindShow()
{
	if (UShowControlSubsystem* Show = BoundShow.Get())
	{
		Show->OnCueStarted.RemoveDynamic(this, &ThisClass::HandleShowCueStarted);
		Show->OnDeskStateChanged.RemoveDynamic(this, &ThisClass::HandleShowChanged);
		Show->OnPlaybacksChanged.RemoveDynamic(this, &ThisClass::HandleShowChanged);
		Show->OnCueListChanged.RemoveDynamic(this, &ThisClass::HandleShowCueListChanged);
	}
	BoundShow.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StateTimer);
	}
	bCuesDirty = false;
}

void UStageWebRemoteSubsystem::HandleShowCueStarted(float CueNumber)
{
	MarkStateDirty(false);
}

void UStageWebRemoteSubsystem::HandleShowChanged()
{
	MarkStateDirty(false);
}

void UStageWebRemoteSubsystem::HandleShowCueListChanged()
{
	MarkStateDirty(true);
}

void UStageWebRemoteSubsystem::MarkStateDirty(bool bCues)
{
	bCuesDirty |= bCues;
	UWorld* World = GetWorld();
	if (!World || !Server || World->GetTimerManager().IsTimerActive(StateTimer))
	{
		return;
	}
	World->GetTimerManager().SetTimer(StateTimer, FTimerDelegate::CreateUObject(this, &ThisClass::SendState), StateInterval, false);
}

void UStageWebRemoteSubsystem::SendState()
{
	const UShowControlSubsystem* Show = BoundShow.Get();
	if (!Server || !Show)
	{
		return;
	}

	if (bCuesDirty)
	{
		bCuesDirty = false;
		TArray<StageWebRemote::FCueEntry> Cues;
		const TArray<FShowCue>& ShowCues = Show->GetCues();
		Cues.Reserve(FMath::Min(ShowCues.Num(), StageWebRemote::MaxCues));
		for (const FShowCue& Cue : ShowCues)
		{
			if (Cues.Num() >= StageWebRemote::MaxCues)
			{
				break;
			}
			StageWebRemote::FCueEntry& Entry = Cues.AddDefaulted_GetRef();
			Entry.Number = Cue.Number;
			Entry.Label = Cue.Label.ToString();
		}
		Server->SetCues(StageWebRemote::MakeCuesMessage(Cues));
	}

	StageWebRemote::FShowState State;
	State.ActiveCue = Show->GetActiveCueNumber();
	if (const FShowCue* Active = Show->FindCue(State.ActiveCue))
	{
		State.ActiveLabel = Active->Label.ToString();
	}
	const TArray<FStagePlayback>& Playbacks = Show->GetPlaybacks();
	State.Playbacks.Reserve(StagePlaybackRules::Count);
	for (int32 Index = 0; Index < StagePlaybackRules::Count; ++Index)
	{
		StageWebRemote::FShowState::FPlayback& Playback = State.Playbacks.AddDefaulted_GetRef();
		if (Playbacks.IsValidIndex(Index))
		{
			Playback.Name = Playbacks[Index].Name.ToString();
			Playback.bEmpty = Playbacks[Index].Cues.IsEmpty();
		}
		Playback.Level = Show->GetPlaybackLevel(Index);
		Playback.bFlash = Show->IsPlaybackFlashing(Index);
		Playback.bActive = Show->IsPlaybackActive(Index);
	}
	State.GrandMaster = Show->GetGrandMaster();
	State.bBlackout = Show->IsBlackout();
	State.AudioMaster = Show->GetAudioMaster();
	Server->SetState(StageWebRemote::MakeStateMessage(State));
}

void UStageWebRemoteSubsystem::RefreshPairingStatus()
{
	if (!Server)
	{
		return;
	}
	Status.Pin = Server->GetPin();
	const TArray<StageWebRemote::FRememberedDevice> Devices = Server->GetDevices();
	Status.PairedDevices = Devices.Num();
	Status.Devices.Reset(Devices.Num());
	for (const StageWebRemote::FRememberedDevice& Device : Devices)
	{
		FStageWebRemoteDeviceInfo& Info = Status.Devices.AddDefaulted_GetRef();
		Info.Id = Device.Id;
		Info.Name = Device.Name;
		Info.LastSeenUtc = FDateTime::FromUnixTimestamp(Device.LastSeenUnix);
		Info.OpenPages = Algo::Count(OpenDeviceIds, Device.Id);
	}
	if (GetSettings().bRememberDevices)
	{
		SaveDevicesIfChanged(StageWebRemote::WriteDevicesJsonOrEmpty(Devices));
	}
	const double LockedUntil = Server->GetPairingLockedUntil();
	Status.PairingLockedUntil = LockedUntil > FPlatformTime::Seconds() ? LockedUntil : 0.0;
}

bool UStageWebRemoteSubsystem::CanUseDevicesFile()
{
#if WITH_DEV_AUTOMATION_TESTS
	// Any test that makes a Game world starts this subsystem; only tests that moved UserRoot to a temporary folder may read or write
	// the devices file, so an unrelated test never rewrites (or expires entries in) the user's own list.
	if (GIsAutomationTesting && !StageCraftPaths::HasTestOverride())
	{
		return false;
	}
#endif
	return true;
}

void UStageWebRemoteSubsystem::SaveDevicesIfChanged(const FString& Json)
{
	if (Json == SavedDevicesJson || !CanUseDevicesFile())
	{
		return;
	}
	const FString File = GetDevicesFile();
	if (Json.IsEmpty())
	{
		// No device, no file: nothing is left in the user folder by a Web Remote that never paired anything.
		IFileManager::Get().Delete(*File, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		SavedDevicesJson.Reset();
		return;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree*/ true);
	if (StageAtomicFile::WriteText(File, Json))
	{
		SavedDevicesJson = Json;
	}
	else
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Web Remote: could not save the paired devices to %s; they must pair again after a restart."), *File);
	}
}

TArray<FString> UStageWebRemoteSubsystem::FindAddresses(EStageWebRemoteScope Scope, int32 Port)
{
	TArray<FString> Addresses;
	if (Scope == EStageWebRemoteScope::ThisComputer)
	{
		Addresses.Add(FString::Printf(TEXT("http://127.0.0.1:%d"), Port));
		return Addresses;
	}
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TArray<TSharedPtr<FInternetAddr>> Adapters;
	if (Sockets && Sockets->GetLocalAdapterAddresses(Adapters))
	{
		for (const TSharedPtr<FInternetAddr>& Adapter : Adapters)
		{
			if (!Adapter.IsValid() || Adapter->GetProtocolType() != FNetworkProtocolTypes::IPv4)
			{
				continue;
			}
			const FString Ip = Adapter->ToString(false);
			if (Ip.StartsWith(TEXT("127.")) || Ip.StartsWith(TEXT("169.254.")) || Ip == TEXT("0.0.0.0"))
			{
				continue;
			}
			Addresses.AddUnique(FString::Printf(TEXT("http://%s:%d"), *Ip, Port));
		}
		// The first address is the one the QR code shows; put the likely Wi-Fi/Ethernet one there, not a virtual switch's.
		Addresses.StableSort([](const FString& A, const FString& B)
		{
			return StageWebRemote::AddressPreference(A.Mid(7)) < StageWebRemote::AddressPreference(B.Mid(7));
		});
	}
	if (Addresses.IsEmpty())
	{
		Addresses.Add(FString::Printf(TEXT("http://127.0.0.1:%d"), Port));
	}
	return Addresses;
}

void UStageWebRemoteSubsystem::MarkStatusDirty()
{
	bStatusDirty = true;
	UWorld* World = GetWorld();
	if (!World || World->GetTimerManager().IsTimerActive(StatusTimer))
	{
		return;
	}
	World->GetTimerManager().SetTimer(StatusTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		if (bStatusDirty)
		{
			BroadcastStatus();
		}
	}), StatusInterval, false);
}

void UStageWebRemoteSubsystem::BroadcastStatus()
{
	bStatusDirty = false;
	OnStatusChanged.Broadcast();
}

#undef LOCTEXT_NAMESPACE
