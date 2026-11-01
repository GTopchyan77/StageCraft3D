// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Bridge/StageBridgeTypes.h"
#include "Engine/TimerHandle.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/UniquePtr.h"
#include "StageBridgeSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageBridgeStateChanged, bool, bRunning);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageBridgeClientsChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageBridgeSendCompleted, const FStageBridgeResultInfo&, Result);

/**
 * The DCC bridge (Docs/ADR/0022-dcc-bridge.md): sends the user's saved stages to Blender, 3ds Max and Cinema 4D running on this
 * computer, and exports them to glTF binary files.
 *
 * Hub and spoke: StageCraft listens on 127.0.0.1 (FStageBridgeServer); each app's add-on connects and authenticates with the token
 * of this run, which the bridge writes to a per-user session file (GetSessionFilePath) and deletes when it stops. A send exports
 * the saved scene into the outbox (GetOutboxDirectory) and tells the app its path and SHA-1; the app imports it and answers.
 *
 * Trust and lifetime:
 *  - Off by default: the bridge listens only between StartBridge and StopBridge (the user's switch); nothing is remembered.
 *  - Only the user's own stages leave (FStageSceneExporter); a client never sends anything StageCraft acts on beyond its ack.
 *  - One send at a time; it ends on the app's ack, its disconnect, StopBridge, or after SendTimeoutSeconds.
 *  - Push, never poll: socket events wake the game thread once per batch; everything is released in Deinitialize.
 *
 * Callers in play go through AModularPlayerController (RequestSetBridgeEnabled, RequestExportScene, RequestSendScene).
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageBridgeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Generous: a large stage takes a while to import into an app that is busy. */
	static constexpr float SendTimeoutSeconds = 120.f;

	UStageBridgeSubsystem();
	virtual ~UStageBridgeSubsystem() override;

	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Starts listening (idempotent). False, with OutError, when the port cannot be opened or the session file cannot be written. */
	bool StartBridge(FText& OutError);

	/** Closes every connection, ends a pending send as failed, removes the session file and the outbox. Idempotent. */
	void StopBridge();

	UFUNCTION(BlueprintPure, Category = "StageCraft|Bridge")
	bool IsRunning() const;

	/** The listening port while running; 0 otherwise. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Bridge")
	int32 GetPort() const;

	/** Connected (authenticated) apps, in connection order. */
	UFUNCTION(BlueprintPure, Category = "StageCraft|Bridge")
	const TArray<FStageBridgeClientInfo>& GetClients() const { return Clients; }

	UFUNCTION(BlueprintPure, Category = "StageCraft|Bridge")
	bool IsSending() const { return Pending.IsSet(); }

	/** Exports the saved scene SceneName to Path (absolute, .glb). Synchronous; also broadcast on OnSendCompleted. */
	FStageBridgeResultInfo ExportSceneToFile(const FString& SceneName, const FString& Path);

	/**
	 * Exports the saved scene into the outbox and sends it to the connected app ClientId. Success means sent: the final outcome
	 * (imported, refused by the app, timed out) arrives on OnSendCompleted. A refusal is returned and not broadcast.
	 */
	FStageBridgeResultInfo SendSceneToApp(const FString& SceneName, int32 ClientId);

	/** %LOCALAPPDATA%/StageCraft/Bridge/session.json on Windows: port, token and outbox for the add-ons of this user. */
	static FString GetSessionFilePath();

	/** Saved/StageCraft/Bridge/Outbox, absolute: files waiting for an app. Emptied when the bridge starts and stops. */
	static FString GetOutboxDirectory();

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Bridge")
	FOnStageBridgeStateChanged OnStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Bridge")
	FOnStageBridgeClientsChanged OnClientsChanged;

	/** Every finished export to a file, and every send once the app answered (or did not). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Bridge")
	FOnStageBridgeSendCompleted OnSendCompleted;

private:
	struct FPendingSend
	{
		FString AssetId;
		int32 ClientId = 0;
		FStageBridgeResultInfo Result;
	};

	void ScheduleDrain();
	void DrainEvents();
	void FinishSend(EStageBridgeResult Code, const FText& Message);
	void HandleSendTimeout();
	const FStageBridgeClientInfo* FindClient(int32 ClientId) const;
	bool WriteSessionFile(uint16 Port) const;
	static void ClearOutbox();

	TUniquePtr<class FStageBridgeServer> Server;
	FString Token;
	TArray<FStageBridgeClientInfo> Clients;
	TOptional<FPendingSend> Pending;
	FTimerHandle SendTimeout;
};
