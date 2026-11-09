// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vision/StageVisionBomBackend.h"

#include "AI/StageAIAssistantSettings.h"
#include "AI/StageClaudeHttp.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageVisionBomBackend)

#define LOCTEXT_NAMESPACE "StageClaudeVisionBomBackend"

namespace StageClaudeVisionBom
{
	/** Counting equipment in a photo at high effort takes longer than reading a drawing. */
	constexpr float MinTimeoutSeconds = 120.f;
}

bool UStageClaudeVisionBomBackend::IsAvailable(FText& OutReason) const
{
	if (!GetDefault<UStageAIAssistantSettings>()->UsesOnlineModel())
	{
		OutReason = LOCTEXT("Off", "The Vision Inspector needs the online model. Turn it on in Project Settings › StageCraft AI Assistant.");
		return false;
	}
	if (StageClaudeHttp::ReadApiKey().IsEmpty())
	{
		OutReason = FText::Format(LOCTEXT("NoKey", "The Vision Inspector needs an API key in the {0} environment variable."),
			FText::FromString(UStageAIAssistantSettings::GetApiKeyVariable()));
		return false;
	}
	return true;
}

FText UStageClaudeVisionBomBackend::GetDisplayName() const
{
	return FText::Format(LOCTEXT("DisplayName", "Claude ({0})"), FText::FromString(GetDefault<UStageAIAssistantSettings>()->GetModelOptions().Model));
}

bool UStageClaudeVisionBomBackend::Read(const TArray<uint8>& Jpeg, FOnStageVisionBomRead OnRead)
{
	FText Unavailable;
	const FString ApiKey = StageClaudeHttp::ReadApiKey();
	if (PendingRequest.IsValid() || Jpeg.IsEmpty() || !IsAvailable(Unavailable))
	{
		return false;
	}
	const UStageAIAssistantSettings* Settings = GetDefault<UStageAIAssistantSettings>();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = StageClaudeHttp::MakeMessagesRequest(ApiKey,
		StageVisionBom::BuildRequestBody(Settings->GetModelOptions(), FBase64::Encode(Jpeg)),
		FMath::Max(Settings->GetRequestTimeoutSeconds(), StageClaudeVisionBom::MinTimeoutSeconds));

	// Completion runs on the game thread. The weak binding never calls into a destroyed backend; Cancel unbinds it first.
	Request->OnProcessRequestComplete().BindWeakLambda(this, [this, OnRead](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		PendingRequest.Reset();
		FStageVisionBomResult Result;
		if (!bConnectedSuccessfully || !Response.IsValid())
		{
			Result.Error = LOCTEXT("NoConnection", "The online model could not be reached (no connection or timed out).");
		}
		else
		{
			Result = StageVisionBom::ParseResponseBody(Response->GetResponseCode(), Response->GetContentAsString());
		}
		if (Result.Status == FStageVisionBomResult::EStatus::Failed)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Vision Inspector: online reading failed: %s"), *Result.Error.ToString());
		}
		OnRead.ExecuteIfBound(Result);
	});
	if (!Request->ProcessRequest())
	{
		Request->OnProcessRequestComplete().Unbind();
		return false;
	}
	PendingRequest = Request;
	return true;
}

void UStageClaudeVisionBomBackend::Cancel()
{
	if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = MoveTemp(PendingRequest))
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
	}
}

void UStageClaudeVisionBomBackend::BeginDestroy()
{
	Cancel();
	Super::BeginDestroy();
}

#undef LOCTEXT_NAMESPACE
