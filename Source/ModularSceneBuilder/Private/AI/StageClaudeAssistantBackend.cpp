// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAIAssistantBackend.h"

#include "AI/StageAIAssistantSettings.h"
#include "AI/StageClaudeHttp.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAIAssistantBackend)

#define LOCTEXT_NAMESPACE "StageClaudeAssistantBackend"

bool UStageClaudeAssistantBackend::IsAvailable(FText& OutReason) const
{
	if (StageClaudeHttp::ReadApiKey().IsEmpty())
	{
		OutReason = FText::Format(LOCTEXT("NoKey", "the online model needs an API key in the {0} environment variable"),
			FText::FromString(UStageAIAssistantSettings::GetApiKeyVariable()));
		return false;
	}
	return true;
}

FText UStageClaudeAssistantBackend::GetDisplayName() const
{
	return FText::Format(LOCTEXT("DisplayName", "Claude ({0})"), FText::FromString(GetDefault<UStageAIAssistantSettings>()->GetModelOptions().Model));
}

bool UStageClaudeAssistantBackend::Interpret(const FStageAIBackendRequest& Request, FOnStageAIBackendFinished OnFinished)
{
	const FString ApiKey = StageClaudeHttp::ReadApiKey();
	if (PendingRequest.IsValid() || ApiKey.IsEmpty())
	{
		return false;
	}
	const UStageAIAssistantSettings* Settings = GetDefault<UStageAIAssistantSettings>();

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = StageClaudeHttp::MakeMessagesRequest(ApiKey,
		StageAICommandSchema::BuildRequestBody(Settings->GetModelOptions(), Request.History, Request.Prompt, Request.SceneSummary), Settings->GetRequestTimeoutSeconds());
	const EStageAILanguage PromptLanguage = StageAILanguage::Detect(Request.Prompt);

	// Completion runs on the game thread. The weak binding never calls into a destroyed backend; Cancel unbinds it first.
	HttpRequest->OnProcessRequestComplete().BindWeakLambda(this,
		[this, OnFinished, PromptLanguage](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			PendingRequest.Reset();
			FStageAIModelResponse Result;
			if (!bConnectedSuccessfully || !Response.IsValid())
			{
				Result.Error = LOCTEXT("NoConnection", "The online model could not be reached (no connection or timed out).");
			}
			else
			{
				Result = StageAICommandSchema::ParseResponseBody(Response->GetResponseCode(), Response->GetContentAsString(), PromptLanguage);
			}
			if (Result.Status == FStageAIModelResponse::EStatus::Failed)
			{
				UE_LOG(LogStageCraft, Warning, TEXT("AI assistant: online request failed: %s"), *Result.Error.ToString());
			}
			OnFinished.ExecuteIfBound(Result);
		});

	if (!HttpRequest->ProcessRequest())
	{
		HttpRequest->OnProcessRequestComplete().Unbind();
		return false;
	}
	PendingRequest = HttpRequest;
	return true;
}

void UStageClaudeAssistantBackend::Cancel()
{
	if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = MoveTemp(PendingRequest))
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
	}
}

void UStageClaudeAssistantBackend::BeginDestroy()
{
	Cancel();
	Super::BeginDestroy();
}

#undef LOCTEXT_NAMESPACE
