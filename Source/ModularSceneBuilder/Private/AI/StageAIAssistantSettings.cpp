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

FStageTranscriptionOptions UStageAIAssistantSettings::GetTranscriptionOptions() const
{
	FStageTranscriptionOptions Options;
	static const TCHAR* const Models[] = { TEXT("gpt-4o-transcribe"), TEXT("gpt-4o-mini-transcribe"), TEXT("whisper-1") };
	const FString TrimmedModel = SpeechModel.TrimStartAndEnd();
	Options.Model = Models[0];
	for (const TCHAR* Known : Models)
	{
		if (TrimmedModel.Equals(Known, ESearchCase::IgnoreCase))
		{
			Options.Model = Known;
		}
	}
	switch (SpeechLanguage)
	{
	case EStageSpeechLanguage::English:		Options.LanguageCode = StageAILanguage::GetCode(EStageAILanguage::English); break;
	case EStageSpeechLanguage::Armenian:	Options.LanguageCode = StageAILanguage::GetCode(EStageAILanguage::Armenian); break;
	default:								break;
	}
	// Stage words the service would otherwise often mishear, in both languages.
	Options.VocabularyHint = TEXT("StageCraft stage design. Truss, deck, riser, moving head, spot, wash, LED par, beam, strobe, cue, fade, DMX, patch sheet, ")
		TEXT("bill of materials, cable schedule, outdoors, sunset. Հայերեն՝ ֆերմա, բեմ, հարթակ, շարժական գլուխ, լույս, սարք, կյու, անցում, անձրև, մառախուղ, մայրամուտ, բացօթյա։");
	Options.TimeoutSeconds = FMath::IsFinite(SpeechTimeoutSeconds) ? FMath::Clamp(SpeechTimeoutSeconds, 5.f, 120.f) : 30.f;
	return Options;
}

float UStageAIAssistantSettings::GetMaxRecordingSeconds() const
{
	return FMath::IsFinite(MaxRecordingSeconds) ? FMath::Clamp(MaxRecordingSeconds, 3.f, 120.f) : 30.f;
}
