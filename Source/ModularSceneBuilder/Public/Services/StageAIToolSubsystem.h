// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Services/StageAIToolTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/UniquePtr.h"
#include "StageAIToolSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageAIToolsChanged);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStageAIToolAnswered, const FStageAIToolResultInfo&, Result);

/**
 * External AI 3D tools and their API keys (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md).
 *
 * Owns the configured providers (ProviderClasses; none by default), the per-user credential vault, and the one operation that may
 * be in flight (a key check or a model hand-off). Keys go into the vault and come out only here, for the single call that needs
 * them, and are wiped afterwards; views see only an EStageAIToolKeyState.
 *
 * Callers in play go through AModularPlayerController (RequestStoreProviderKey, RequestRemoveProviderKey,
 * RequestVerifyProviderKey); the acquire dispatcher calls SubmitModel after its own checks.
 */
UCLASS(Config = Game)
class MODULARSCENEBUILDER_API UStageAIToolSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UStageAIToolSubsystem();
	virtual ~UStageAIToolSubsystem() override;

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Every configured tool with its key state. No disk access: key presence is known from Initialize, StoreKey and RemoveKey. */
	TArray<FStageAIToolInfo> GetTools() const;

	/** False when no tool has that id. */
	bool FindTool(FName ProviderId, FStageAIToolInfo& OutInfo) const;

	/** False where keys cannot be stored protected (non-Windows). */
	bool IsKeyStorageAvailable() const;

	/** Saves ApiKey for the tool (trimmed; 1..4096 characters). Replaces a saved key and forgets any earlier check. */
	FStageAIToolResultInfo StoreKey(FName ProviderId, const FString& ApiKey);

	/** Deletes the tool's saved key. Success also when none was saved. */
	FStageAIToolResultInfo RemoveKey(FName ProviderId);

	/** Asks the tool's service to check the saved key. Accepted means started; the answer arrives on OnKeyVerified. */
	FStageAIToolResultInfo VerifyKey(FName ProviderId);

	/** Hands a model to a tool that AcceptModel. Accepted means started; the answer arrives on OnJobCompleted. */
	FStageAIToolResultInfo SubmitModel(FName ProviderId, const FStageAIToolJob& Job);

	/** True while a key check or a model hand-off is waiting for its tool. */
	bool IsBusy() const { return Operation != EOperation::None; }

	/** Abandons the operation in flight; its listener gets Cancelled. Idempotent. */
	void CancelOperation();

	/** The time a tool has to answer before its operation is cancelled with TimedOut (OperationTimeoutSeconds, validated). */
	float GetOperationTimeoutSeconds() const;

	/**
	 * Development builds and tests only (no effect in Shipping): adds a provider of Class that is not in the config, e.g. the
	 * offline mock. Ignored if a provider with the same id exists.
	 */
	void RegisterProviderForDevelopment(TSubclassOf<class UStageAIToolProvider> Class);

	/** Tests only (no effect in Shipping): use VaultDirectory instead of the user's folder. Call before anything else. */
	void InitializeForTests(const FString& VaultDirectory);

	/** Tools or key states changed. */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|AI Tools")
	FOnStageAIToolsChanged OnToolsChanged;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|AI Tools")
	FOnStageAIToolAnswered OnKeyVerified;

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|AI Tools")
	FOnStageAIToolAnswered OnJobCompleted;

protected:
	/** The tools offered. Each must be a concrete UStageAIToolProvider with a valid, unique id. Empty by default. */
	UPROPERTY(Config)
	TArray<TSoftClassPtr<class UStageAIToolProvider>> ProviderClasses;

	/**
	 * Seconds a tool has to answer a key check or a hand-off. Without it a tool that never answers would keep the subsystem (and
	 * the acquire dispatcher behind it) busy for the rest of the run. Values <= 0 fall back to the default.
	 */
	UPROPERTY(Config)
	float OperationTimeoutSeconds = 120.f;

private:
	enum class EOperation : uint8
	{
		None,
		Verify,
		Submit,
	};

	class UStageAIToolProvider* FindProvider(FName ProviderId) const;
	EStageAIToolKeyState GetKeyState(const class UStageAIToolProvider& Provider) const;
	FStageAIToolInfo DescribeProvider(const class UStageAIToolProvider& Provider) const;
	bool AddProvider(TSubclassOf<class UStageAIToolProvider> Class);
	/** Re-reads which providers have a key in the vault (one file lookup each). On Initialize and when the vault changes. */
	void RefreshSavedKeys();
	void HandleOperationTimeout(uint32 ForSerial);
	void ClearOperationTimer();

	/** Reads the key for Provider (or none, when it needs none). Fails with NeedsKey / Unavailable / Failed. */
	FStageAIToolResultInfo ReadKey(const class UStageAIToolProvider& Provider, FString& OutKey) const;

	/** Starts Operation and returns the callback that ends it; answers for an older operation are ignored. */
	TDelegate<void(const FStageAIToolResultInfo&)> BeginOperation(EOperation InOperation, FName ProviderId);
	void HandleAnswer(const FStageAIToolResultInfo& Result, uint32 ForSerial);

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageAIToolProvider>> Providers;

	/** Outcomes of key checks in this run. Saved keys without an entry are EStageAIToolKeyState::Saved. */
	TMap<FName, EStageAIToolKeyState> CheckedKeys;

	/** Providers with a key in the vault. Mirrors the vault folder so views never touch the disk (Rules §29). */
	TSet<FName> SavedKeys;

	FTimerHandle OperationTimer;

	TUniquePtr<class FStageCredentialVault> Vault;

	EOperation Operation = EOperation::None;
	FName OperationProvider;
	uint32 OperationSerial = 0;
};
