// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageAICommandSchema.h"
#include "AI/StageSpeechTypes.h"
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

	// --- Voice prompts (Docs/ADR/0017 §2.5) ---

	bool UsesVoiceInput() const { return bUseVoiceInput; }

	/** How recordings are transcribed: the model, the expected language, the stage vocabulary and the timeout, sanitized. */
	FStageTranscriptionOptions GetTranscriptionOptions() const;

	/** Longest recording, 3 to 120 s; the microphone closes by itself after it. */
	float GetMaxRecordingSeconds() const;

	/** Send a transcript as soon as it arrives, instead of putting it in the field to check first. */
	bool SendsTranscriptImmediately() const { return bSendTranscriptImmediately; }

	/** The environment variable the speech service's API key is read from. */
	static const TCHAR* GetSpeechApiKeyVariable() { return TEXT("OPENAI_API_KEY"); }

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

	/**
	 * Show the MIC button: a spoken request is recorded from the default microphone and transcribed into the prompt field, in
	 * English or Armenian. Off by default: when on, each recording is sent to OpenAI's transcription API, with the key from the
	 * OPENAI_API_KEY environment variable. Nothing is recorded until MIC is pressed.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Voice Input")
	bool bUseVoiceInput = false;

	/** The transcription model: gpt-4o-transcribe, gpt-4o-mini-transcribe or whisper-1. */
	UPROPERTY(Config, EditAnywhere, Category = "Voice Input", meta = (EditCondition = "bUseVoiceInput"))
	FString SpeechModel = TEXT("gpt-4o-transcribe");

	/** The language to expect. Auto detects English or Armenian per recording; fixing it helps with short or noisy recordings. */
	UPROPERTY(Config, EditAnywhere, Category = "Voice Input", meta = (EditCondition = "bUseVoiceInput"))
	EStageSpeechLanguage SpeechLanguage = EStageSpeechLanguage::Auto;

	UPROPERTY(Config, EditAnywhere, Category = "Voice Input", meta = (EditCondition = "bUseVoiceInput", ClampMin = "3", ClampMax = "120", Units = "s"))
	float MaxRecordingSeconds = 30.f;

	UPROPERTY(Config, EditAnywhere, Category = "Voice Input", meta = (EditCondition = "bUseVoiceInput", ClampMin = "5", ClampMax = "120", Units = "s"))
	float SpeechTimeoutSeconds = 30.f;

	/** Off: the transcript goes into the prompt field to check, and Enter sends it. On: it is sent right away. */
	UPROPERTY(Config, EditAnywhere, Category = "Voice Input", meta = (EditCondition = "bUseVoiceInput"))
	bool bSendTranscriptImmediately = false;
};
