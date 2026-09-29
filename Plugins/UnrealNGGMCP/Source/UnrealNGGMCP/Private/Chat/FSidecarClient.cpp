// Copyright 2025-2026 NGG. All Rights Reserved.

#include "FSidecarClient.h"
#include "SidecarLauncher.h"

#include "WebSocketsModule.h"
#include "IWebSocket.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FSidecarClient::FSidecarClient() = default;

FSidecarClient::~FSidecarClient()
{
	Disconnect();
}

void FSidecarClient::SetState(ENGGChatState NewState)
{
	if (State != NewState)
	{
		State = NewState;
		OnStateChanged.Broadcast(State);
	}
}

bool FSidecarClient::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void FSidecarClient::Connect(const FString& URL, const FString& SessionId, bool bReplay, const FString& ProjectCwd)
{
	Disconnect();

	PendingSessionId = SessionId;
	PendingCwd       = ProjectCwd;
	bPendingReplay   = bReplay;

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		FModuleManager::Get().LoadModule(TEXT("WebSockets"));
	}

	UE_LOG(LogNGGChat, Log, TEXT("FSidecarClient: connecting to %s (session=%s, replay=%s)"),
		*URL, *SessionId, bReplay ? TEXT("true") : TEXT("false"));

	// Normalize URL: UE5's IWebSocket on Windows fails silently if the path is empty
	// and sometimes with the string "localhost" — force 127.0.0.1 and a trailing "/".
	FString NormalizedURL = URL;
	NormalizedURL.ReplaceInline(TEXT("//localhost"), TEXT("//127.0.0.1"));
	if (!NormalizedURL.Contains(TEXT("://")) || NormalizedURL.EndsWith(TEXT(":")))
	{
		// malformed — fall back to original
		NormalizedURL = URL;
	}
	else
	{
		int32 SchemeEnd = NormalizedURL.Find(TEXT("://")) + 3;
		int32 PathStart = NormalizedURL.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SchemeEnd);
		if (PathStart == INDEX_NONE)
		{
			NormalizedURL.Append(TEXT("/"));
		}
	}

	// Empty protocol list is required — passing arbitrary protocols causes the server to reject the upgrade.
	const TArray<FString> Protocols;
	Socket = FWebSocketsModule::Get().CreateWebSocket(NormalizedURL, Protocols);
	UE_LOG(LogNGGChat, Log, TEXT("FSidecarClient: normalized URL -> %s"), *NormalizedURL);
	SetState(ENGGChatState::Connecting);

	TWeakPtr<FSidecarClient> WeakSelf = AsShared();

	Socket->OnConnected().AddLambda([WeakSelf]()
	{
		if (auto Pinned = WeakSelf.Pin())
		{
			UE_LOG(LogNGGChat, Log, TEXT("FSidecarClient: WS open — sending subscribe"));
			Pinned->SetState(ENGGChatState::Handshaking);

			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("type"), TEXT("subscribe"));
			Obj->SetStringField(TEXT("sessionId"), Pinned->PendingSessionId);
			Obj->SetBoolField(TEXT("replay"), Pinned->bPendingReplay);
			if (!Pinned->PendingCwd.IsEmpty())
			{
				Obj->SetStringField(TEXT("cwd"), Pinned->PendingCwd);
			}
			Pinned->SendJson(Obj);
		}
	});

	Socket->OnConnectionError().AddLambda([WeakSelf](const FString& Err)
	{
		if (auto Pinned = WeakSelf.Pin())
		{
			UE_LOG(LogNGGChat, Warning, TEXT("FSidecarClient: connection error: %s"), *Err);
			Pinned->SetState(ENGGChatState::Disconnected);
			// IWebSocket sometimes reports empty strings on common cases
			// like "port not accepting yet" — surface something actionable.
			const FString Details = Err.IsEmpty()
				? FString(TEXT("sidecar not reachable (still starting up or port blocked)"))
				: Err;
			Pinned->OnError.Broadcast(FString::Printf(TEXT("Connection error: %s"), *Details));
		}
	});

	Socket->OnClosed().AddLambda([WeakSelf](int32 StatusCode, const FString& Reason, bool /*bClean*/)
	{
		if (auto Pinned = WeakSelf.Pin())
		{
			UE_LOG(LogNGGChat, Log, TEXT("FSidecarClient: WS closed (%d): %s"), StatusCode, *Reason);
			Pinned->SetState(ENGGChatState::Disconnected);
		}
	});

	Socket->OnMessage().AddLambda([WeakSelf](const FString& Msg)
	{
		if (auto Pinned = WeakSelf.Pin())
		{
			Pinned->HandleTextMessage(Msg);
		}
	});

	Socket->Connect();
}

void FSidecarClient::Disconnect()
{
	if (Socket.IsValid())
	{
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
		Socket.Reset();
	}
	SetState(ENGGChatState::Disconnected);
}

void FSidecarClient::SendJson(const TSharedRef<FJsonObject>& Obj)
{
	if (!Socket.IsValid() || !Socket->IsConnected())
	{
		UE_LOG(LogNGGChat, Warning, TEXT("SendJson: socket not connected"));
		return;
	}

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	Socket->Send(Out);
}

void FSidecarClient::SendPrompt(const FString& Text)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("prompt"));
	Obj->SetStringField(TEXT("text"), Text);
	UE_LOG(LogNGGChat, Log, TEXT("SendPrompt: %d chars"), Text.Len());
	SendJson(Obj);
	SetState(ENGGChatState::Thinking);
}

void FSidecarClient::SendPromptWithImages(const FString& Text, const TArray<FNGGPastedImage>& Images)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("prompt"));
	Obj->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> ImgArray;
	for (const FNGGPastedImage& Img : Images)
	{
		TSharedRef<FJsonObject> ImgObj = MakeShared<FJsonObject>();
		ImgObj->SetStringField(TEXT("mediaType"), Img.MediaType);
		ImgObj->SetStringField(TEXT("data"), Img.Base64Data);
		ImgArray.Add(MakeShared<FJsonValueObject>(ImgObj));
	}
	Obj->SetArrayField(TEXT("images"), ImgArray);

	UE_LOG(LogNGGChat, Log, TEXT("SendPromptWithImages: %d chars, %d image(s)"), Text.Len(), Images.Num());
	SendJson(Obj);
	SetState(ENGGChatState::Thinking);
}

void FSidecarClient::SendCancel()
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("cancel"));
	UE_LOG(LogNGGChat, Log, TEXT("SendCancel"));
	SendJson(Obj);
}

void FSidecarClient::SendClear()
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("clear"));
	UE_LOG(LogNGGChat, Log, TEXT("SendClear"));
	SendJson(Obj);
}

void FSidecarClient::SendSetModel(const FString& Model)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("set_model"));
	Obj->SetStringField(TEXT("model"), Model);
	UE_LOG(LogNGGChat, Log, TEXT("SendSetModel: %s"), Model.IsEmpty() ? TEXT("<default>") : *Model);
	SendJson(Obj);
}

void FSidecarClient::SendSetPermissionBypass(bool bEnabled)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("set_permission_bypass"));
	Obj->SetBoolField(TEXT("enabled"), bEnabled);
	UE_LOG(LogNGGChat, Log, TEXT("SendSetPermissionBypass: %s"),
		bEnabled ? TEXT("on") : TEXT("off"));
	SendJson(Obj);
}

void FSidecarClient::SendPermissionResponse(const FString& RequestId, const FString& Decision)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("permission_response"));
	Obj->SetStringField(TEXT("id"), RequestId);
	Obj->SetStringField(TEXT("decision"), Decision);
	UE_LOG(LogNGGChat, Log, TEXT("SendPermissionResponse id=%s decision=%s"),
		*RequestId, *Decision);
	SendJson(Obj);
}

void FSidecarClient::SendListModels()
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("list_models"));
	UE_LOG(LogNGGChat, Log, TEXT("SendListModels"));
	SendJson(Obj);
}


void FSidecarClient::HandleTextMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogNGGChat, Warning, TEXT("Could not parse message: %s"),
			*Message.Left(200));
		return;
	}

	const FString Type = Root->GetStringField(TEXT("type"));

	if (Type == TEXT("ready"))
	{
		FString Ver, SessId;
		double PidDouble = 0.0;
		Root->TryGetStringField(TEXT("version"), Ver);
		Root->TryGetStringField(TEXT("sessionId"), SessId);
		Root->TryGetNumberField(TEXT("pid"), PidDouble);
		const int32 Pid = static_cast<int32>(PidDouble);
		UE_LOG(LogNGGChat, Log, TEXT("Sidecar ready — v%s pid=%d session=%s"), *Ver, Pid, *SessId);
		OnSystem.Broadcast(FString::Printf(TEXT("sidecar_ready:%s|%d|%s"), *Ver, Pid, *SessId));
		SetState(ENGGChatState::Ready);
	}
	else if (Type == TEXT("event"))
	{
		const TSharedPtr<FJsonObject>* EventObj = nullptr;
		if (Root->TryGetObjectField(TEXT("event"), EventObj) && EventObj && EventObj->IsValid())
		{
			OnEvent.Broadcast(*EventObj);
		}
	}
	else if (Type == TEXT("status"))
	{
		const FString S = Root->GetStringField(TEXT("state"));
		if (S == TEXT("idle"))           SetState(ENGGChatState::Ready);
		else if (S == TEXT("thinking"))  SetState(ENGGChatState::Thinking);
		else if (S == TEXT("cancelled")) SetState(ENGGChatState::Ready);
		OnStatus.Broadcast(S);
	}
	else if (Type == TEXT("error"))
	{
		const FString M = Root->GetStringField(TEXT("message"));
		UE_LOG(LogNGGChat, Warning, TEXT("Sidecar error: %s"), *M);
		OnError.Broadcast(M);
	}
	else if (Type == TEXT("permission_request"))
	{
		UE_LOG(LogNGGChat, Log, TEXT("permission_request id=%s tool=%s"),
			*Root->GetStringField(TEXT("id")),
			*Root->GetStringField(TEXT("tool")));
		OnPermissionRequest.Broadcast(Root);
	}
	else if (Type == TEXT("models"))
	{
		TArray<FNGGSidecarModel> Models;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Root->TryGetArrayField(TEXT("models"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
				if (!O.IsValid()) continue;
				FNGGSidecarModel M;
				O->TryGetStringField(TEXT("id"), M.Id);
				if (!O->TryGetStringField(TEXT("display_name"), M.DisplayName) || M.DisplayName.IsEmpty())
				{
					M.DisplayName = M.Id;
				}
				if (!M.Id.IsEmpty()) Models.Add(M);
			}
		}
		FString Source;
		Root->TryGetStringField(TEXT("source"), Source);
		UE_LOG(LogNGGChat, Log, TEXT("models: %d entries (source=%s)"), Models.Num(), *Source);
		OnModels.Broadcast(Models);
	}
	else
	{
		UE_LOG(LogNGGChat, Verbose, TEXT("Unhandled message type: %s"), *Type);
	}
}
