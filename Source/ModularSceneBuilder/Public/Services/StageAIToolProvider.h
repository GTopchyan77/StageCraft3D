// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/StageAIToolTypes.h"
#include "UObject/Object.h"
#include "StageAIToolProvider.generated.h"

/** A tool's answer to one call. */
DECLARE_DELEGATE_OneParam(FOnStageAIToolAnswer, const FStageAIToolResultInfo& /*Result*/);

/**
 * One external AI 3D tool (Meshy, Tripo, a self-hosted pipeline...): the seam between StageCraft and a third-party service
 * (Docs/ADR/0023-ai-tool-providers-and-credential-vault.md).
 *
 * Contract for implementations:
 *  - Every call that takes OnAnswer must execute it exactly once, now or later, on the game thread. Cancel must make a pending
 *    call answer Cancelled (once) or not at all; UStageAIToolSubsystem ignores answers it no longer waits for.
 *  - ApiKey is passed per call by UStageAIToolSubsystem, which reads it from the credential vault. A provider uses it for the
 *    request it is making and never stores, logs or echoes it, and never puts it in a result message.
 *  - Files handed to SubmitModel belong to the caller; a provider copies or uploads them and does not delete them.
 *
 * Providers are created by UStageAIToolSubsystem from its ProviderClasses config. They are not used directly by UI.
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageAIToolProvider : public UObject
{
	GENERATED_BODY()

public:
	/** Stable id, used to name the saved key: letters, digits, '_' and '-', at most 32 characters. */
	virtual FName GetProviderId() const PURE_VIRTUAL(UStageAIToolProvider::GetProviderId, return NAME_None;);

	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageAIToolProvider::GetDisplayName, return FText::GetEmpty(););

	virtual EStageAIToolCapability GetCapabilities() const { return EStageAIToolCapability::None; }

	virtual bool RequiresApiKey() const { return true; }

	/** Asks the service whether ApiKey is accepted. */
	virtual void VerifyKey(const FString& ApiKey, FOnStageAIToolAnswer OnAnswer)
		PURE_VIRTUAL(UStageAIToolProvider::VerifyKey, );

	/** Hands a model to the tool (AcceptModel). */
	virtual void SubmitModel(const FStageAIToolJob& Job, const FString& ApiKey, FOnStageAIToolAnswer OnAnswer)
		PURE_VIRTUAL(UStageAIToolProvider::SubmitModel, );

	/** Abandons any pending call. Idempotent. */
	virtual void Cancel() {}
};
