// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAIAssistantSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAIAssistantSettings)

FStageAIModelOptions UStageAIAssistantSettings::GetModelOptions() const
{
	FStageAIModelOptions Options;
	const FString TrimmedModel = Model.TrimStartAndEnd();
	if (!TrimmedModel.IsEmpty())
	{
		Options.Model = TrimmedModel;
	}
	static const TCHAR* const EffortLevels[] = { TEXT("low"), TEXT("medium"), TEXT("high"), TEXT("xhigh"), TEXT("max") };
	const FString LowerEffort = Effort.TrimStartAndEnd().ToLower();
	for (const TCHAR* Level : EffortLevels)
	{
		if (LowerEffort == Level)
		{
			Options.Effort = Level;
		}
	}
	Options.MaxTokens = FMath::Clamp(MaxTokens, 1024, 64000);
	return Options;
}

float UStageAIAssistantSettings::GetRequestTimeoutSeconds() const
{
	return FMath::IsFinite(RequestTimeoutSeconds) ? FMath::Clamp(RequestTimeoutSeconds, 5.f, 120.f) : 30.f;
}

int32 UStageAIAssistantSettings::GetMaxHistoryMessages() const
{
	return FMath::Clamp(MaxHistoryMessages, 0, 40);
}
