// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"

/**
 * The Claude Messages API transport shared by the assistant (UStageClaudeAssistantBackend) and the sketch photo reader
 * (UStageClaudeSketchVisionBackend): endpoint, version and beta headers, and the API key. Game thread only.
 */
namespace StageClaudeHttp
{
	/** The key from ANTHROPIC_API_KEY, read at request time and never stored or logged. Empty when unset. */
	FString ReadApiKey();

	/**
	 * A POST /v1/messages request with Body, ready for a completion delegate and ProcessRequest. Sends the fallbacks beta header,
	 * which the request bodies' fallbacks "default" field requires.
	 */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeMessagesRequest(const FString& ApiKey, const FString& Body, float TimeoutSeconds);
}
