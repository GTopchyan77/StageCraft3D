// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/Tests/StageTestAIToolProvider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageTestAIToolProvider)

const FName UStageTestAIToolProvider::ProviderId(TEXT("testtool"));

void UStageTestAIToolProvider::Answer(FOnStageAIToolAnswer& OnAnswer)
{
	switch (Mode)
	{
	case EMode::AnswerNow:
		OnAnswer.ExecuteIfBound(FStageAIToolResultInfo::Ok(ProviderId, FText::FromString(TEXT("Accepted."))));
		return;
	case EMode::RejectKey:
		OnAnswer.ExecuteIfBound(FStageAIToolResultInfo::Fail(EStageAIToolResult::KeyRejected, ProviderId, FText::FromString(TEXT("Refused."))));
		return;
	case EMode::Hold:
	default:
		Held = OnAnswer;
		return;
	}
}

void UStageTestAIToolProvider::VerifyKey(const FString& ApiKey, FOnStageAIToolAnswer OnAnswer)
{
	++VerifyCount;
	LastKey = ApiKey;
	Answer(OnAnswer);
}

void UStageTestAIToolProvider::SubmitModel(const FStageAIToolJob& Job, const FString& ApiKey, FOnStageAIToolAnswer OnAnswer)
{
	++SubmitCount;
	LastKey = ApiKey;
	LastJob = Job;
	Answer(OnAnswer);
}

void UStageTestAIToolProvider::Cancel()
{
	++CancelCount;
	// A cancelled call is not answered by the tool; the subsystem closes it.
	Held.Unbind();
}
