// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAITypes.h"

/** The online model's request settings (UStageAIAssistantSettings, sanitized). */
struct FStageAIModelOptions
{
	FString Model = TEXT("claude-opus-5-5");
	/** output_config.effort: low, medium, high, xhigh or max. */
	FString Effort = TEXT("low");
	int32 MaxTokens = 16000;
};

/** What one Messages API response asked for. */
struct FStageAIModelResponse
{
	enum class EStatus : uint8
	{
		/** One stage_command tool call, already validated. */
		Command,
		/** A plain-text answer. */
		Text,
		/** The model declined (stop_reason "refusal"). */
		Refused,
		/** Transport, status, format or validation failure; Error says which. */
		Failed,
	};

	EStatus Status = EStatus::Failed;
	FStageAICommand Command;
	FString Text;
	FText Error;

	/** The model called the tool with a command that failed validation: reported as it is, never re-interpreted locally. */
	bool bInvalidCommand = false;
};

/**
 * The trust boundary of the AI assistant (Docs/ADR/0016-ai-assistant.md §2.1, §2.5): validation of every command before it runs,
 * whichever interpreter produced it, and the online model's wire format (the stage_command tool, the Messages API request body and
 * response parsing). Pure: no HTTP and no world; covered by the StageCraft.AI.Schema tests. Model output is untrusted input.
 */
namespace StageAICommandSchema
{
	/** Longest item query and label kept (characters). */
	constexpr int32 MaxQueryLength = 64;

	/** Name of the one tool the model may call. */
	MODULARSCENEBUILDER_API const TCHAR* GetToolName();

	/**
	 * Checks and normalizes Command in place: the intent is known, counts and numbers are finite and in range, strings trimmed and
	 * capped. False, with a user-facing reason, when it must not run; Command is then unspecified.
	 */
	MODULARSCENEBUILDER_API bool Validate(FStageAICommand& Command, FText& OutError);

	/** The wire name of Intent ("add_fixtures"); empty for Count. */
	MODULARSCENEBUILDER_API FString GetIntentName(EStageAIIntent Intent);

	/** The stage_command tool definition, as a JSON object (name, description, strict, input_schema). */
	MODULARSCENEBUILDER_API FString MakeToolDefinitionJson();

	/** A command from the tool's input object (JSON text), then validated. False, with a reason, when it is malformed or invalid. */
	MODULARSCENEBUILDER_API bool CommandFromToolInputJson(const FString& InputJson, FStageAICommand& OutCommand, FText& OutError);

	/** The assistant's instructions to the model. Stable text (no per-request values), so it can be cached. */
	MODULARSCENEBUILDER_API FString GetSystemPrompt();

	/** The scene as a few plain lines for the model: totals and limits, fixtures by name, cues, environment. */
	MODULARSCENEBUILDER_API FString DescribeScene(const FStageAISceneView& Scene);

	/**
	 * The JSON body of POST /v1/messages. History (oldest first, without the new prompt) becomes alternating plain-text turns:
	 * leading assistant lines are dropped and consecutive lines of one speaker are joined. The new turn carries SceneSummary
	 * and Prompt.
	 */
	MODULARSCENEBUILDER_API FString BuildRequestBody(const FStageAIModelOptions& Options, TConstArrayView<FStageAIMessage> History, const FString& Prompt,
		const FString& SceneSummary);

	/** Reads a response: status, error object, stop_reason, then one stage_command tool call or the text blocks. */
	MODULARSCENEBUILDER_API FStageAIModelResponse ParseResponseBody(int32 HttpStatus, const FString& Body);
}
