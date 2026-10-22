// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageSpeechToTextBackend.h"

#include "AI/StageAIAssistantSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "ModularSceneBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpeechToTextBackend)

#define LOCTEXT_NAMESPACE "StageSpeechToTextBackend"

namespace StageOpenAITranscription
{
	const TCHAR* const Endpoint = TEXT("https://api.openai.com/v1/audio/transcriptions");

	FString ReadApiKey()
	{
		return FPlatformMisc::GetEnvironmentVariable(UStageAIAssistantSettings::GetSpeechApiKeyVariable()).TrimStartAndEnd();
	}
}

bool UStageOpenAITranscriptionBackend::IsAvailable(FText& OutReason) const
{
	if (StageOpenAITranscription::ReadApiKey().IsEmpty())
	{
		OutReason = FText::Format(LOCTEXT("NoKey", "Voice prompts need an OpenAI API key in the {0} environment variable."),
			FText::FromString(UStageAIAssistantSettings::GetSpeechApiKeyVariable()));
		return false;
	}
	return true;
}

FText UStageOpenAITranscriptionBackend::GetDisplayName() const
{
	return FText::Format(LOCTEXT("DisplayName", "OpenAI {0}"), FText::FromString(GetDefault<UStageAIAssistantSettings>()->GetTranscriptionOptions().Model));
}

bool UStageOpenAITranscriptionBackend::Transcribe(TArray<uint8> WavFile, const FStageTranscriptionOptions& Options, FOnStageTranscriptionFinished OnFinished)
{
	const FString ApiKey = StageOpenAITranscription::ReadApiKey();
	if (PendingRequest.IsValid() || ApiKey.IsEmpty() || WavFile.IsEmpty())
	{
		return false;
	}

	TArray<StageSpeechAudio::FFormField> Fields = { { TEXT("model"), Options.Model }, { TEXT("response_format"), TEXT("json") } };
	if (!Options.LanguageCode.IsEmpty())
	{
		Fields.Add({ TEXT("language"), Options.LanguageCode });
	}
	if (!Options.VocabularyHint.IsEmpty())
	{
		Fields.Add({ TEXT("prompt"), Options.VocabularyHint });
	}
	const FString Boundary = StageSpeechAudio::MakeBoundary(FGuid::NewGuid());

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(StageOpenAITranscription::Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
	Request->SetHeader(TEXT("Content-Type"), TEXT("multipart/form-data; boundary=") + Boundary);
	Request->SetContent(StageSpeechAudio::BuildMultipartBody(Boundary, Fields, TEXT("file"), TEXT("prompt.wav"), TEXT("audio/wav"), WavFile));
	Request->SetTimeout(Options.TimeoutSeconds);

	// Completion runs on the game thread. The weak binding never calls into a destroyed backend; Cancel unbinds it first.
	Request->OnProcessRequestComplete().BindWeakLambda(this, [this, OnFinished](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
	{
		PendingRequest.Reset();
		FStageTranscriptionResult Result;
		if (!bConnectedSuccessfully || !Response.IsValid())
		{
			Result.Error = LOCTEXT("NoConnection", "The speech service could not be reached (no connection or timed out).");
		}
		else
		{
			Result = StageSpeechAudio::ParseTranscriptionResponse(Response->GetResponseCode(), Response->GetContentAsString());
		}
		if (!Result.bSucceeded)
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Voice: transcription failed: %s"), *Result.Error.ToString());
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

void UStageOpenAITranscriptionBackend::Cancel()
{
	if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request = MoveTemp(PendingRequest))
	{
		Request->OnProcessRequestComplete().Unbind();
		Request->CancelRequest();
	}
}

void UStageOpenAITranscriptionBackend::BeginDestroy()
{
	Cancel();
	Super::BeginDestroy();
}

#undef LOCTEXT_NAMESPACE
