// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageClaudeHttp.h"

#include "AI/StageAIAssistantSettings.h"
#include "HttpModule.h"

namespace StageClaudeHttp
{
	namespace
	{
		const TCHAR* const Endpoint = TEXT("https://api.anthropic.com/v1/messages");
		const TCHAR* const ApiVersion = TEXT("2023-06-01");
		/** Required by the fallbacks "default" request field. */
		const TCHAR* const FallbackBeta = TEXT("server-side-fallback-2026-07-01");
	}

	FString ReadApiKey()
	{
		return FPlatformMisc::GetEnvironmentVariable(UStageAIAssistantSettings::GetApiKeyVariable()).TrimStartAndEnd();
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeMessagesRequest(const FString& ApiKey, const FString& Body, float TimeoutSeconds)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Endpoint);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("x-api-key"), ApiKey);
		Request->SetHeader(TEXT("anthropic-version"), ApiVersion);
		Request->SetHeader(TEXT("anthropic-beta"), FallbackBeta);
		Request->SetContentAsString(Body);
		Request->SetTimeout(TimeoutSeconds);
		return Request;
	}
}
