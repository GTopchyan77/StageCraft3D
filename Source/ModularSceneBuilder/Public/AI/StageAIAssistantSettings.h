// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAICommandSchema.h"
#include "Engine/DeveloperSettings.h"
#include "StageAIAssistantSettings.generated.h"

/**
 * The AI assistant's interpreter (Project Settings > Game > StageCraft AI Assistant, saved to DefaultGame.ini;
 * Docs/ADR/0016-ai-assistant.md §2.5). The local parser is always there; the online model is opt-in because it sends the prompt,
 * a scene summary and the recent conversation to Anthropic. The API key is never stored here: it is read from the
 * ANTHROPIC_API_KEY environment variable when a request is made. Every value is clamped again by the getters, so a hand-edited
 * ini cannot produce an invalid request.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "StageCraft AI Assistant"))
class MODULARSCENEBUILDER_API UStageAIAssistantSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//~ Begin UDeveloperSettings Interface
	virtual FName GetCategoryName() const override { return TEXT("Game"); }
	//~ End UDeveloperSettings Interface

	/** The request options, sanitized: a known effort level, a model id, a bounded token limit. */
	FStageAIModelOptions GetModelOptions() const;

	bool UsesOnlineModel() const { return bUseOnlineModel; }

	/** Seconds before an online request is abandoned and the local parser answers instead, 5 to 120. */
	float GetRequestTimeoutSeconds() const;

	/** Earlier conversation lines sent with an online request, 0 to 40. */
	int32 GetMaxHistoryMessages() const;

	/** The environment variable the API key is read from. */
	static const TCHAR* GetApiKeyVariable() { return TEXT("ANTHROPIC_API_KEY"); }

private:
	/**
	 * Let a Claude model interpret requests the local grammar may not know. Off by default: when on, prompts, a scene summary
	 * and recent conversation lines are sent to Anthropic's API. Every resulting command is validated and runs through the
	 * same rules as the local parser's. Without an API key in ANTHROPIC_API_KEY the local parser is used.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Online Model")
	bool bUseOnlineModel = false;

	UPROPERTY(Config, EditAnywhere, Category = "Online Model", meta = (EditCondition = "bUseOnlineModel"))
	FString Model = TEXT("claude-opus-5-5");

	/** output_config.effort: low, medium, high, xhigh or max. Low suits translating one request into one command. */
	UPROPERTY(Config, EditAnywhere, Category = "Online Model", meta = (EditCondition = "bUseOnlineModel"))
	FString Effort = TEXT("low");

	UPROPERTY(Config, EditAnywhere, Category = "Online Model", meta = (EditCondition = "bUseOnlineModel", ClampMin = "1024", ClampMax = "64000"))
	int32 MaxTokens = 16000;

	UPROPERTY(Config, EditAnywhere, Category = "Online Model", meta = (EditCondition = "bUseOnlineModel", ClampMin = "5", ClampMax = "120", Units = "s"))
	float RequestTimeoutSeconds = 30.f;

	UPROPERTY(Config, EditAnywhere, Category = "Online Model", meta = (EditCondition = "bUseOnlineModel", ClampMin = "0", ClampMax = "40"))
	int32 MaxHistoryMessages = 10;
};
