// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchVisionBackend.h"

#include "AI/StageAIAssistantSettings.h"
#include "AI/StageClaudeHttp.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSketchVisionBackend)

#define LOCTEXT_NAMESPACE "StageClaudeSketchVisionBackend"

namespace StageClaudeSketchVision
{
	/** Reading an image takes longer than interpreting a sentence. */
	constexpr float MinTimeoutSeconds = 60.f;
}

bool UStageClaudeSketchVisionBackend::IsAvailable(FText& OutReason) const
{
	if (!GetDefault<UStageAIAssistantSettings>()->UsesOnlineModel())
	{
		OutReason = LOCTEXT("Off", "Claude reading is off. Turn on the online model in Project Settings › StageCraft AI Assistant.");
		return false;
	}
	if (StageClaudeHttp::ReadApiKey().IsEmpty())
	{
		OutReason = FText::Format(LOCTEXT("NoKey", "Claude reading needs an API key in the {0} environment variable."),
			FText::FromString(UStageAIAssistantSettings::GetApiKeyVariable()));
		return false;
	}
	return true;
}

FText UStageClaudeSketchVisionBackend::GetDisplayName() const
{
	return FText::Format(LOCTEXT("DisplayName", "Claude ({0})"), FText::FromString(GetDefault<UStageAIAssistantSettings>()->GetModelOptions().Model));
}

bool UStageClaudeSketchVisionBackend::Read(const TArray<uint8>& Jpeg, FOnStageSketchVisionFinished OnFinished)
{
	FText Unavailable;
	const FString ApiKey = StageClaudeHttp::ReadApiKey();
	if (PendingRequest.IsValid() || Jpeg.IsEmpty() || !IsAvailable(Unavailable))
	{
		return false;
	}
	const UStageAIAssistantSettings* Settings = GetDefault<UStageAIAssistantSettings>();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = StageClaudeHttp::MakeMessagesRequest(ApiKey,
		StageSketchVision::BuildRequestBody(Settings->GetModelOptions(), FBase64::Encode(Jpeg)),
		FMath::Max(Settings->GetRequestTimeoutSeconds(), StageClaudeSketchVision::MinTimeoutSeconds));

	// Completion runs on the game thread. The weak binding never calls into a destroyed backend; Cancel unbinds it first.
	Request->OnProcessRequestComplete().BindWeakLambda(this, [this, OnFinished](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		PendingRequest.Reset();
		FStageSketchVisionResult Result;
		if (!bConnectedSuccessfully || !Response.IsValid())
		{
			Result.Error = LOCTEXT("NoConnection", "The online model could not be reached (no connection or timed out).");
		}
		else
		{
			Result = StageSketchVision::ParseResponseBody(Response->GetResponseCode(), Response->GetContentAsString());
		}
		if (Result.Status == FStageSketchVisionResult::EStatus::Failed)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Sketch: online reading failed: %s"), *Result.Error.ToString());
		}
		OnFinished.ExecuteIfBound(Result);
	});
	if (!Request->ProcessRequest())
	{
		Request->OnProcessRequestComplete().Unbind();
		return false;
	}
	PendingRequest = Request;
	return true;
}

void UStageClaudeSketchVisionBackend::Cancel()
{
	if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = MoveTemp(PendingRequest))
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
	}
}

void UStageClaudeSketchVisionBackend::BeginDestroy()
{
	Cancel();
	Super::BeginDestroy();
}

#undef LOCTEXT_NAMESPACE
