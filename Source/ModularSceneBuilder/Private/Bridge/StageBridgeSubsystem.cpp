// Copyright Epic Games, Inc. All Rights Reserved.

#include "Bridge/StageBridgeSubsystem.h"

#include "Async/Async.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeServer.h"
#include "Bridge/StageSceneExporter.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IO/StageAtomicFile.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
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

	FString Sha1OfFile(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return FString();
		}
		FSHAHash Hash;
		FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
		return Hash.ToString().ToLower();
	}
}

UStageBridgeSubsystem::UStageBridgeSubsystem() = default;

// Out of line: FStageBridgeServer is only complete in the .cpp.
UStageBridgeSubsystem::~UStageBridgeSubsystem() = default;

void UStageBridgeSubsystem::Deinitialize()
{
	StopBridge();
	Super::Deinitialize();
}

FString UStageBridgeSubsystem::GetSessionFilePath()
{
	// UserSettingsDir is the user's local application data folder: other users of the computer cannot read the token.
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("StageCraft"), TEXT("Bridge"), TEXT("session.json"));
}

FString UStageBridgeSubsystem::GetOutboxDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("Bridge"), TEXT("Outbox")));
}

bool UStageBridgeSubsystem::IsRunning() const
{
	return Server.IsValid() && Server->IsRunning();
}

int32 UStageBridgeSubsystem::GetPort() const
{
	return IsRunning() ? Server->GetPort() : 0;
}

bool UStageBridgeSubsystem::StartBridge(FText& OutError)
{
	if (IsRunning())
	{
		return true;
	}

	ClearOutbox();
	Token = StageBridgeProtocol::MakeToken();
	Server = MakeUnique<FStageBridgeServer>();
	if (!Server->Start(StageBridgeSubsystemImpl::GetPort(), Token, [this]() { ScheduleDrain(); }, OutError))
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
			if (!Pending.IsSet() || Pending->ClientId != Event.ClientId || Pending->AssetId != Event.Ack.Id)
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

	if (bClientsChanged)
	{
		OnClientsChanged.Broadcast();
	}
}

const FStageBridgeClientInfo* UStageBridgeSubsystem::FindClient(int32 ClientId) const
{
	return Clients.FindByPredicate([ClientId](const FStageBridgeClientInfo& Client) { return Client.ClientId == ClientId; });
}

FStageBridgeResultInfo UStageBridgeSubsystem::ExportSceneToFile(const FString& SceneName, const FString& Path)
{
	UGameInstance* GameInstance = GetGameInstance();
	FStageBridgeResultInfo Result = GameInstance ? FStageSceneExporter::ExportSavedScene(*GameInstance, FStageSceneStore(FStageSceneStore::GetDefaultDirectory()), SceneName, Path)
		: FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("NoGameInstance", "Exporting is not available."));
	UE_LOG(LogStageCraft, Log, TEXT("Bridge: export of \"%s\" to %s: %s."), *SceneName, *Path, *UEnum::GetValueAsString(Result.Code));
	OnSendCompleted.Broadcast(Result);
	return Result;
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
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::Unavailable, LOCTEXT("NoGameInstance", "Exporting is not available."));
	}

	const FString AssetId = StageBridgeProtocol::MakeAssetId();
	const FString Path = FPaths::Combine(GetOutboxDirectory(), AssetId + FStageSceneExporter::FileExtension);
	FStageBridgeResultInfo Result = FStageSceneExporter::ExportSavedScene(*GameInstance, FStageSceneStore(FStageSceneStore::GetDefaultDirectory()), SceneName, Path);
	Result.ClientId = ClientId;
	if (!Result.IsSuccess())
	{
		return Result;
	}

	StageBridgeProtocol::FAsset Asset;
	Asset.Id = AssetId;
	Asset.Format = TEXT("glb");
	Asset.Name = SceneName;
	Asset.Path = Path;
	Asset.Sha1 = StageBridgeSubsystemImpl::Sha1OfFile(Path);
	Asset.Bytes = IFileManager::Get().FileSize(*Path);
	if (Asset.Sha1.IsEmpty())
	{
		IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true);
		return FStageBridgeResultInfo::Fail(EStageBridgeResult::WriteFailed, LOCTEXT("Unreadable", "The exported file could not be read back."));
	}
	Server->Send(ClientId, StageBridgeProtocol::MakeAsset(Asset));

	Pending.Emplace();
	Pending->AssetId = AssetId;
	Pending->ClientId = ClientId;
	Pending->Result = Result;
	GameInstance->GetTimerManager().SetTimer(SendTimeout, FTimerDelegate::CreateUObject(this, &ThisClass::HandleSendTimeout), SendTimeoutSeconds, /*bLoop*/ false);

	UE_LOG(LogStageCraft, Log, TEXT("Bridge: sent \"%s\" to client %d as %s (%lld bytes)."), *SceneName, ClientId, *AssetId, Asset.Bytes);
	Result.Message = FText::Format(LOCTEXT("Sending", "Sending \"{0}\" to {1}..."), FText::FromString(SceneName), StageBridgeProtocol::GetAppDisplayName(Client->App));
	return Result;
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
