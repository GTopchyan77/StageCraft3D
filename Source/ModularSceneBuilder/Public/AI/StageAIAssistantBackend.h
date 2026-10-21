// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAICommandSchema.h"
#include "UObject/Object.h"
#include "StageAIAssistantBackend.generated.h"

/** One prompt for an online interpreter. */
struct FStageAIBackendRequest
{
	FString Prompt;
	FString SceneSummary;
	/** Earlier conversation lines, oldest first, without Prompt. */
	TArray<FStageAIMessage> History;
};

DECLARE_DELEGATE_OneParam(FOnStageAIBackendFinished, const FStageAIModelResponse& /*Response*/);

/**
 * An online interpreter of assistant prompts (Docs/ADR/0016-ai-assistant.md §2.1). It only translates: its answer is a validated
 * command, a text reply, a refusal or a failure, and UStageAICommandSubsystem decides what happens with it. One request in flight
 * at a time; owned by the subsystem, which cancels it in Deinitialize. Game thread only.
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageAIAssistantBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Whether a request can be made now; OutReason says why not ("no API key"). */
	virtual bool IsAvailable(FText& OutReason) const PURE_VIRTUAL(UStageAIAssistantBackend::IsAvailable, return false;);

	/** User-facing name of the interpreter ("Claude (claude-opus-5-5)"). */
	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageAIAssistantBackend::GetDisplayName, return FText::GetEmpty(););

	/**
	 * Starts interpreting Request. OnFinished runs exactly once, on the game thread, unless the request is cancelled first.
	 * False, and no callback, when it cannot start (unavailable, or a request is already in flight).
	 */
	virtual bool Interpret(const FStageAIBackendRequest& Request, FOnStageAIBackendFinished OnFinished)
		PURE_VIRTUAL(UStageAIAssistantBackend::Interpret, return false;);

	/** Abandons the request in flight without calling back. Safe to call when there is none. */
	virtual void Cancel() PURE_VIRTUAL(UStageAIAssistantBackend::Cancel, );
};

/**
 * The Claude Messages API (POST https://api.anthropic.com/v1/messages) over raw HTTP, with one strict stage_command tool
 * (StageAICommandSchema builds the body and parses the answer). The key comes from ANTHROPIC_API_KEY at request time and is
 * never stored or logged. A safety decline is retried server-side on a fallback model (fallbacks "default").
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageClaudeAssistantBackend : public UStageAIAssistantBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageAIAssistantBackend Interface
	virtual bool IsAvailable(FText& OutReason) const override;
	virtual FText GetDisplayName() const override;
	virtual bool Interpret(const FStageAIBackendRequest& Request, FOnStageAIBackendFinished OnFinished) override;
	virtual void Cancel() override;
	//~ End UStageAIAssistantBackend Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

private:
	static FString ReadApiKey();

	/** The request in flight; its completion delegate is unbound on Cancel so it can never call back afterwards. */
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> PendingRequest;
};
