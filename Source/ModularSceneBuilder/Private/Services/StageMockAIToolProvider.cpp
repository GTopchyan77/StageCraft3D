// Copyright Epic Games, Inc. All Rights Reserved.

#include "Services/StageMockAIToolProvider.h"

#include "HAL/FileManager.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMockAIToolProvider)

#define LOCTEXT_NAMESPACE "StageCraftAITools"

const FName UStageMockAIToolProvider::ProviderId(TEXT("mock"));

FString UStageMockAIToolProvider::GetReceivedDirectory()
{
	return FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("AITools"), TEXT("Mock"));
}

FText UStageMockAIToolProvider::GetDisplayName() const
{
	return LOCTEXT("MockName", "Local test tool");
}

void UStageMockAIToolProvider::VerifyKey(const FString& ApiKey, FOnStageAIToolAnswer OnAnswer)
{
	OnAnswer.ExecuteIfBound(ApiKey.StartsWith(AcceptedKeyPrefix, ESearchCase::CaseSensitive)
		? FStageAIToolResultInfo::Ok(ProviderId, LOCTEXT("MockKeyOk", "The test tool accepted the key."))
		: FStageAIToolResultInfo::Fail(EStageAIToolResult::KeyRejected, ProviderId, LOCTEXT("MockKeyRejected", "The test tool refused the key.")));
}

void UStageMockAIToolProvider::SubmitModel(const FStageAIToolJob& Job, const FString& ApiKey, FOnStageAIToolAnswer OnAnswer)
{
	if (!ApiKey.StartsWith(AcceptedKeyPrefix, ESearchCase::CaseSensitive))
	{
		OnAnswer.ExecuteIfBound(FStageAIToolResultInfo::Fail(EStageAIToolResult::KeyRejected, ProviderId, LOCTEXT("MockSubmitKey", "The test tool refused the key.")));
		return;
	}
	if (FPaths::IsRelative(Job.ModelPath) || !IFileManager::Get().FileExists(*Job.ModelPath))
	{
		OnAnswer.ExecuteIfBound(FStageAIToolResultInfo::Fail(EStageAIToolResult::InvalidRequest, ProviderId, LOCTEXT("MockNoFile", "There is no model file to send.")));
		return;
	}

	const FString Target = FPaths::Combine(GetReceivedDirectory(), FPaths::GetCleanFilename(Job.ModelPath));
	IFileManager::Get().MakeDirectory(*GetReceivedDirectory(), /*Tree*/ true);
	if (IFileManager::Get().Copy(*Target, *Job.ModelPath, /*Replace*/ true) != COPY_OK)
	{
		OnAnswer.ExecuteIfBound(FStageAIToolResultInfo::Fail(EStageAIToolResult::Failed, ProviderId, LOCTEXT("MockCopy", "The test tool could not store the model.")));
		return;
	}
	FStageAIToolResultInfo Result = FStageAIToolResultInfo::Ok(ProviderId, FText::Format(LOCTEXT("MockReceived", "The test tool received \"{0}\"."), FText::FromString(Job.Name)));
	Result.OutputPath = Target;
	OnAnswer.ExecuteIfBound(Result);
}

#undef LOCTEXT_NAMESPACE
