// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class IWebSocket;

/** High-level state used to drive the chat UI status bar. */
enum class ENGGChatState : uint8
{
	Disconnected,
	Connecting,
	Handshaking, // Socket open, waiting for {"type":"ready"}
	Ready,
	Thinking,
};

/** One entry from the live Anthropic model catalog (GET /v1/models), relayed
 * by the sidecar so the chat window can populate its model dropdown without a
 * hardcoded list. */
struct FNGGSidecarModel
{
	FString Id;          // e.g. "claude-opus-4-8" — passed to `claude --model`
	FString DisplayName; // e.g. "Claude Opus 4.8" — shown in the dropdown
};

/** One image pasted from the clipboard, ready to include in a prompt. */
struct FNGGPastedImage
{
	FString MediaType;  // e.g. "image/png"
	FString Base64Data; // raw base64, no data-URL prefix
	int32   Width  = 0;
	int32   Height = 0;
};

/**
 * FSidecarClient
 *
 * Owns the IWebSocket to the Node.js ngg-sidecar. Parses the JSON protocol
 * described in the CLAUDE Chat spec and fans it out to delegates the chat
 * window can bind to.
 *
 * Lifetime: owned by SNGGChatWindow. Shared pointer (the websocket module
 * needs a stable address for its lambdas).
 */
class FSidecarClient : public TSharedFromThis<FSidecarClient>
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnStateChanged, ENGGChatState);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnEvent, TSharedPtr<FJsonObject> /*stream json event*/);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnStatus, FString /*state hint*/);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnError, FString /*message*/);
	/** Permission prompt arrived from sidecar. Payload is the full protocol object
	 * (id, tool, input). The UI should respond via SendPermissionResponse. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPermissionRequest, TSharedPtr<FJsonObject> /*protocol object*/);
	/** Raw system notice from sidecar (e.g. permission_mode:prompt-tool). */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnSystem, FString /*subtype*/);
	/** Live model catalog arrived from the sidecar (response to SendListModels).
	 * Empty array means the fetch failed — the UI should keep its fallback list. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnModels, const TArray<FNGGSidecarModel>& /*models*/);

	FSidecarClient();
	~FSidecarClient();

	void Connect(const FString& URL, const FString& SessionId, bool bReplay, const FString& ProjectCwd = FString());
	void Disconnect();

	void SendPrompt(const FString& Text);
	void SendPromptWithImages(const FString& Text, const TArray<FNGGPastedImage>& Images);
	void SendCancel();
	void SendClear();
	/** Set the Claude model used on the next spawn. Empty string clears the
	 * override so the Claude CLI picks its default. Forces a restart of the
	 * underlying claude subprocess if one is already running. */
	void SendSetModel(const FString& Model);
	/** Toggle permission-gate bypass. When enabled, the sidecar respawns claude
	 * with --dangerously-skip-permissions on the next prompt — eliminating the
	 * per-tool permission roundtrip. Identical UX to running the same flag in
	 * the terminal. Must be paired with a clear UI affordance: the user is
	 * asserting they trust the prompt. */
	void SendSetPermissionBypass(bool bEnabled);
	/** Decision values: "approve", "approve_always", "deny". */
	void SendPermissionResponse(const FString& RequestId, const FString& Decision);
	/** Ask the sidecar for the live Anthropic model catalog. The reply arrives
	 * asynchronously via the OnModels delegate. */
	void SendListModels();

	bool IsConnected() const;
	ENGGChatState GetState() const { return State; }

	FOnStateChanged      OnStateChanged;
	FOnEvent             OnEvent;
	FOnStatus            OnStatus;
	FOnError             OnError;
	FOnPermissionRequest OnPermissionRequest;
	FOnSystem            OnSystem;
	FOnModels            OnModels;

private:
	void SetState(ENGGChatState NewState);
	void SendJson(const TSharedRef<FJsonObject>& Obj);
	void HandleTextMessage(const FString& Message);

	TSharedPtr<IWebSocket> Socket;
	FString PendingSessionId;
	FString PendingCwd;
	bool    bPendingReplay = false;
	ENGGChatState State = ENGGChatState::Disconnected;
};
