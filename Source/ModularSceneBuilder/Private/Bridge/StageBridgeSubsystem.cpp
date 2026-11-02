// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageBridgeSubsystem.h"

#include "Async/Async.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeServer.h"
#include "Bridge/StageSceneExporter.h"
#include "Bridge/StageSceneExportJob.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IO/StageAtomicFile.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Scene/StageSceneStore.h"
#include "ModularSceneBuilder.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageBridgeSubsystem)

#define LOCTEXT_NAMESPACE "StageBridgeSubsystem"

namespace StageBridgeSubsystemImpl
{
	uint16 GetPort()
	{
		int32 Port = StageBridgeProtocol::DefaultPort;
		// A second port for a second StageCraft, or for a machine where the default is taken.
		if (FParse::Value(FCommandLine::Get(), TEXT("StageBridgePort="), Port) && (Port < 1024 || Port > 65535))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Bridge: -StageBridgePort=%d is outside 1024-65535; using %d."), Port, StageBridgeProtocol::DefaultPort);
			Port = StageBridgeProtocol::DefaultPort;
		}
		return static_cast<uint16>(Port);
	}

}

UStageBridgeSubsystem::UStageBridgeSubsystem() = default;

// Out of line: FStageBridgeServer is only complete in the .cpp.
UStageBridgeSubsystem::~UStageBridgeSubsystem() = default;

void UStageBridgeSubsystem::Deinitialize()
{
	// Nobody is told about a shutdown: listeners may already be gone, and StopBridge would broadcast to them.
	OnStateChanged.Clear();
	OnClientsChanged.Clear();
	OnSendCompleted.Clear();
	StopBridge();
	// An export to a file still running is abandoned with its world; its requester is not called (see BeginExportSceneToFile).
	ExportJob.Reset();
	Super::Deinitialize();
}

FString UStageBridgeSubsystem::GetSessionFilePath()
{
	// UserSettingsDir is the user's local application data folder: other users of the computer cannot read the token.
	return FPaths::Combine(StageCraftPaths::UserRoot(), TEXT("Bridge"), TEXT("session.json"));
}

FString UStageBridgeSubsystem::GetOutboxDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Bridge"), TEXT("Outbox")));
}

bool UStageBridgeSubsystem::IsRunning() const
{
	return Server.IsValid() && Server->IsRunning();
}

int32 UStageBridgeSubsystem::GetPort() const
{
	return IsRunning() ? Server->GetPort() : 0;
}

float UStageBridgeSubsystem::GetSendTimeoutSeconds() const
{
	constexpr float DefaultTimeoutSeconds = 120.f;
	return SendTimeoutSeconds > 0.f ? SendTimeoutSeconds : DefaultTimeoutSeconds;
}

bool UStageBridgeSubsystem::StartBridge(FText& OutError)
{
	return StartBridgeOn(StageBridgeSubsystemImpl::GetPort(), OutError);
}

#if WITH_DEV_AUTOMATION_TESTS
bool UStageBridgeSubsystem::StartBridgeOnAnyPortForTests(FText& OutError)
{
	return StartBridgeOn(/*Port*/ 0, OutError);
}
#endif

bool UStageBridgeSubsystem::StartBridgeOn(uint16 Port, FText& OutError)
{
	if (IsRunning())
	{
		return true;
	}

	ClearOutbox();
	Token = StageBridgeProtocol::MakeToken();
	Server = MakeUnique<FStageBridgeServer>();
	if (!Server->Start(Port, Token, [this]() { ScheduleDrain(); }, OutError))
	{
		Server.Reset();
		Token.Reset();
		UE_LOG(LogStageCraft, Warning, TEXT("Bridge: could not start: %s"), *OutError.ToString());
		return false;
	}
	if (!WriteSessionFile(Server->GetPort()))
	{
		OutError = FText::Format(LOCTEXT("SessionFile", "Could not write {0}, which the 3D app add-ons read to connect."), FText::FromString(GetSessionFilePath()));
		StopBridge();
		return false;
	}

	UE_LOG(LogStageCraft, Log, TEXT("Bridge: listening on 127.0.0.1:%d (session file %s)."), Server->GetPort(), *GetSessionFilePath());
	OnStateChanged.Broadcast(true);
	return true;
}

void UStageBridgeSubsystem::StopBridge()
{
	const bool bWasRunning = Server.IsValid();
	if (Pending.IsSet())
	{
		FinishSend(EStageBridgeResult::NotConnected, LOCTEXT("Stopped", "The bridge was switched off before the app answered."));
	}
	// The server joins its thread here; the lambda that captured this cannot run afterwards.
	Server.Reset();
	Token.Reset();
	IFileManager::Get().Delete(*GetSessionFilePath(), /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
	ClearOutbox();

	const bool bHadClients = !Clients.IsEmpty();
	Clients.Reset();
	if (bWasRunning)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Bridge: stopped."));
		if (bHadClients)
		{
			OnClientsChanged.Broadcast();
		}
		OnStateChanged.Broadcast(false);
	}
}

bool UStageBridgeSubsystem::WriteSessionFile(uint16 Port) const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("protocol"), StageBridgeProtocol::Version);
	Root->SetNumberField(TEXT("port"), Port);
	Root->SetStringField(TEXT("token"), Token);
	Root->SetStringField(TEXT("outbox"), GetOutboxDirectory());
	Root->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());

	FString Contents;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Contents);
	FJsonSerializer::Serialize(Root, Writer);

	const FString Path = GetSessionFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	return StageAtomicFile::WriteText(Path, Contents);
}

void UStageBridgeSubsystem::ClearOutbox()
{
	const FString Outbox = GetOutboxDirectory();
	if (IFileManager::Get().DirectoryExists(*Outbox))
	{
		IFileManager::Get().DeleteDirectory(*Outbox, /*RequireExists*/ false, /*Tree*/ true);
	}
}

void UStageBridgeSubsystem::ScheduleDrain()
{
	// Worker thread: one game-thread task per batch of events (the server only asks once until the next drain).
	AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UStageBridgeSubsystem>(this)]()
	{
		if (UStageBridgeSubsystem* Self = WeakThis.Get())
		{
			Self->DrainEvents();
		}
	});
}

void UStageBridgeSubsystem::DrainEvents()
{
	if (!Server.IsValid())
	{
		return;
	}

	bool bClientsChanged = false;
	Server->Drain([this, &bClientsChanged](const FStageBridgeEvent& Event)
	{
		// A listener of FinishSend may switch the bridge off in the middle of a batch; the rest of it belongs to the old session
		// (a Connected event would otherwise add a client to a bridge that is off).
		if (!Server.IsValid())
		{
			return;
		}
		switch (Event.Kind)
		{
		case FStageBridgeEvent::EKind::Connected:
			Clients.Add(Event.Client);
			bClientsChanged = true;
			UE_LOG(LogStageCraft, Log, TEXT("Bridge: %s %s connected (client %d)."), *StageBridgeProtocol::GetAppDisplayName(Event.Client.App).ToString(),
				*Event.Client.AppVersion, Event.ClientId);
			break;

		case FStageBridgeEvent::EKind::Disconnected:
			bClientsChanged |= Clients.RemoveAll([&Event](const FStageBridgeClientInfo& Client) { return Client.ClientId == Event.ClientId; }) > 0;
			UE_LOG(LogStageCraft, Log, TEXT("Bridge: client %d disconnected: %s."), Event.ClientId, *Event.Reason);
			if (Pending.IsSet() && Pending->ClientId == Event.ClientId)
			{
				FinishSend(EStageBridgeResult::AppFailed, LOCTEXT("Disconnected", "The app disconnected before it finished importing."));
			}
			break;

		case FStageBridgeEvent::EKind::Ack:
			// An ack for anything but the pending send (late, after a timeout, or invented) changes nothing.
			if (!Pending.IsSet() || !Pending->bSent || Pending->ClientId != Event.ClientId || Pending->AssetId != Event.Ack.Id)
			{
				UE_LOG(LogStageCraft, Log, TEXT("Bridge: ignored an ack from client %d for an asset that is not pending."), Event.ClientId);
				break;
			}
			if (Event.Ack.bOk)
			{
				const FStageBridgeClientInfo* Client = FindClient(Event.ClientId);
				FinishSend(EStageBridgeResult::Success, FText::Format(LOCTEXT("Imported", "{0} imported \"{1}\" ({2} items)."),
					StageBridgeProtocol::GetAppDisplayName(Client ? Client->App : EStageBridgeApp::Unknown), FText::FromString(Pending->Result.SceneName),
					FText::AsNumber(Pending->Result.ExportedItems)));
			}
			else
			{
				FinishSend(EStageBridgeResult::AppFailed, FText::Format(LOCTEXT("AppFailed", "The app could not import \"{0}\": {1}"),
					FText::FromString(Pending->Result.SceneName), FText::FromString(Event.Ack.Error.IsEmpty() ? TEXT("no reason given") : Event.Ack.Error)));
			}
			break;
		}
	});

	// StopBridge has already announced its own change if it ran during the batch.
	if (bClientsChanged && Server.IsValid())
	{
		OnClientsChanged.Broadcast();
	}
}

const FStageBridgeClientInfo* UStageBridgeSubsystem::FindClient(int32 ClientId) const
{
	return Clients.FindByPredicate([ClientId](const FStageBridgeClientInfo& Client) { return Client.ClientId == ClientId; });
}

bool UStageBridgeSubsystem::IsExporting() const
{
	return ExportJob.IsValid() && ExportJob->IsRunning();
}

FStageBridgeResultInfo UStageBridgeSubsystem::StartExportJob(const FString& SceneName, const FString& Path, bool bComputeSha1, TFunction<void(const FStageSceneExportOutcome&)> OnDone)
{
	if (IsExporting())
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Busy, LOCTEXT("ExportBusy", "Another export is running; try again when it has finished."));
	}
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("NoGameInstance", "Exporting is not available."));
	}
	FStageSceneExportJobParams Params;
	Params.SceneName = SceneName;
	Params.Path = Path;
	Params.bComputeSha1 = bComputeSha1;
	FStageBridgeResultInfo Refusal;
	// The job ends before this subsystem (Deinitialize drops it, which cancels it), so the weak lambda never outlives its owner.
	ExportJob = FStageSceneExportJob::Start(*GameInstance, MoveTemp(Params), FOnStageSceneExportFinished::CreateWeakLambda(this,
		[this, OnDone = MoveTemp(OnDone)](const FStageSceneExportOutcome& Outcome)
		{
			// The job has ended; it is released here, after its callback (the ticker keeps it alive until then).
			ExportJob.Reset();
			OnDone(Outcome);
		}), Refusal);
	return ExportJob.IsValid() ? FStageBridgeResultInfo() : Refusal;
}

FStageBridgeResultInfo UStageBridgeSubsystem::BeginExportSceneToFile(const FString& SceneName, const FString& Path, FOnStageBridgeExportFinished OnFinished)
{
	if (Pending.IsSet())
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Busy, LOCTEXT("ExportBusy", "Another export is running; try again when it has finished."));
	}
	FStageBridgeResultInfo Started = StartExportJob(SceneName, Path, /*bComputeSha1*/ false, [SceneName, Path, OnFinished = MoveTemp(OnFinished)](const FStageSceneExportOutcome& Outcome)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Bridge: export of \"%s\" to %s: %s."), *SceneName, *Path, *UEnum::GetValueAsString(Outcome.Result.Code));
		OnFinished.ExecuteIfBound(Outcome.Result);
	});
	Started.SceneName = SceneName;
	Started.FilePath = Path;
	return Started;
}

FStageBridgeResultInfo UStageBridgeSubsystem::SendSceneToApp(const FString& SceneName, int32 ClientId)
{
	if (!IsRunning())
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::NotConnected, LOCTEXT("Off", "Switch on \"Allow 3D apps to connect\" first."));
	}
	const FStageBridgeClientInfo* Client = FindClient(ClientId);
	if (!Client)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::NotConnected, LOCTEXT("NoClient", "That app is no longer connected."));
	}
	if (!Client->bImportsGlb)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::InvalidRequest,
			FText::Format(LOCTEXT("NoGlb", "{0} cannot import glTF yet; save a .glb file and import it with a glTF plug-in instead."), StageBridgeProtocol::GetAppDisplayName(Client->App)));
	}
	if (Pending.IsSet())
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Busy, LOCTEXT("Busy", "Another send is waiting for its app; try again when it has finished."));
	}

	const FString AssetId = StageBridgeProtocol::MakeAssetId();
	const FString Path = FPaths::Combine(GetOutboxDirectory(), AssetId + FStageSceneExporter::FileExtension);
	// The export runs over several frames; the SHA-1 is computed on a worker as it finishes (HandleSendExported).
	FStageBridgeResultInfo Started = StartExportJob(SceneName, Path, /*bComputeSha1*/ true, [this](const FStageSceneExportOutcome& Outcome)
	{
		HandleSendExported(Outcome);
	});
	Started.SceneName = SceneName;
	Started.ClientId = ClientId;
	if (!Started.IsSuccess())
	{
		return Started;
	}

	Pending.Emplace();
	Pending->AssetId = AssetId;
	Pending->ClientId = ClientId;
	Pending->Result = Started;
	Pending->Result.FilePath = Path;
	UE_LOG(LogStageCraft, Log, TEXT("Bridge: exporting \"%s\" for client %d as %s."), *SceneName, ClientId, *AssetId);
	Started.Message = FText::Format(LOCTEXT("Sending", "Sending \"{0}\" to {1}..."), FText::FromString(SceneName), StageBridgeProtocol::GetAppDisplayName(Client->App));
	return Started;
}

void UStageBridgeSubsystem::HandleSendExported(const FStageSceneExportOutcome& Outcome)
{
	if (!Pending.IsSet() || Pending->bSent)
	{
		return;
	}
	if (!Outcome.Result.IsSuccess())
	{
		FinishSend(Outcome.Result.Code, Outcome.Result.Message);
		return;
	}
	// The app may have left, or the bridge stopped, while the stage was exported.
	const FStageBridgeClientInfo* Client = FindClient(Pending->ClientId);
	UGameInstance* GameInstance = GetGameInstance();
	if (!IsRunning() || !Client || !GameInstance)
	{
		FinishSend(EStageBridgeResult::NotConnected, LOCTEXT("GoneWhileExporting", "The app disconnected while the stage was being exported."));
		return;
	}

	StageBridgeProtocol::FAsset Asset;
	Asset.Id = Pending->AssetId;
	Asset.Format = TEXT("glb");
	Asset.Name = Pending->Result.SceneName;
	Asset.Path = Pending->Result.FilePath;
	Asset.Sha1 = Outcome.Sha1;
	Asset.Bytes = IFileManager::Get().FileSize(*Asset.Path);
	Server->Send(Pending->ClientId, StageBridgeProtocol::MakeAsset(Asset));

	Pending->bSent = true;
	Pending->Result.ExportedItems = Outcome.Result.ExportedItems;
	Pending->Result.SkippedItems = Outcome.Result.SkippedItems;
	GameInstance->GetTimerManager().SetTimer(SendTimeout, FTimerDelegate::CreateUObject(this, &ThisClass::HandleSendTimeout), GetSendTimeoutSeconds(), /*bLoop*/ false);
	UE_LOG(LogStageCraft, Log, TEXT("Bridge: sent \"%s\" to client %d as %s (%lld bytes)."), *Asset.Name, Pending->ClientId, *Asset.Id, Asset.Bytes);
}

void UStageBridgeSubsystem::HandleSendTimeout()
{
	if (Pending.IsSet())
	{
		FinishSend(EStageBridgeResult::AppFailed, LOCTEXT("Timeout", "The app did not answer in time; check its console for errors."));
	}
}

void UStageBridgeSubsystem::FinishSend(EStageBridgeResult Code, const FText& Message)
{
	if (!Pending.IsSet())
	{
		return;
	}
	// Ended while its export still ran (the app left, the bridge stopped): that export is of no use now.
	if (!Pending->bSent)
	{
		ExportJob.Reset();
	}
	FStageBridgeResultInfo Result = MoveTemp(Pending->Result);
	Pending.Reset();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().ClearTimer(SendTimeout);
	}

	// The app has read the file (or never will): the outbox copy has done its job.
	IFileManager::Get().Delete(*Result.FilePath, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
	Result.Code = Code;
	Result.Message = Message;
	UE_LOG(LogStageCraft, Log, TEXT("Bridge: send of \"%s\" to client %d finished: %s (%s)."), *Result.SceneName, Result.ClientId,
		*UEnum::GetValueAsString(Code), *Message.ToString());
	OnSendCompleted.Broadcast(Result);
}

#undef LOCTEXT_NAMESPACE
