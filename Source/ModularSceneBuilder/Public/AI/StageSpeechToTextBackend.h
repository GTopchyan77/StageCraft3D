// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AI/StageSpeechTypes.h"
#include "UObject/Object.h"
#include "StageSpeechToTextBackend.generated.h"

DECLARE_DELEGATE_OneParam(FOnStageTranscriptionFinished, const FStageTranscriptionResult& /*Result*/);

/**
 * A speech-to-text service for voice prompts (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.5). It only transcribes:
 * the text goes into the assistant's prompt field and is interpreted and validated like typed text. One request in flight at a
 * time; owned by UStageVoiceInputComponent, which cancels it in EndPlay. Game thread only.
 */
UCLASS(Abstract)
class MODULARSCENEBUILDER_API UStageSpeechToTextBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Whether a recording can be sent now; OutReason says why not ("no API key"). */
	virtual bool IsAvailable(FText& OutReason) const PURE_VIRTUAL(UStageSpeechToTextBackend::IsAvailable, return false;);

	/** User-facing name of the service ("OpenAI gpt-4o-transcribe"). */
	virtual FText GetDisplayName() const PURE_VIRTUAL(UStageSpeechToTextBackend::GetDisplayName, return FText::GetEmpty(););

	/**
	 * Sends WavFile (mono 16-bit PCM) for transcription. OnFinished runs exactly once, on the game thread, unless cancelled first.
	 * False, and no callback, when it cannot start.
	 */
	virtual bool Transcribe(TArray<uint8> WavFile, const FStageTranscriptionOptions& Options, FOnStageTranscriptionFinished OnFinished)
		PURE_VIRTUAL(UStageSpeechToTextBackend::Transcribe, return false;);

	/** Abandons the request in flight without calling back. Safe when there is none. */
	virtual void Cancel() PURE_VIRTUAL(UStageSpeechToTextBackend::Cancel, );
};

/**
 * OpenAI's transcription API (POST https://api.openai.com/v1/audio/transcriptions, multipart/form-data) over raw HTTP. It
 * recognises English and Armenian and detects the language when none is given. The key comes from OPENAI_API_KEY at request time
 * and is never stored or logged.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageOpenAITranscriptionBackend : public UStageSpeechToTextBackend
{
	GENERATED_BODY()

public:
	//~ Begin UStageSpeechToTextBackend Interface
	virtual bool IsAvailable(FText& OutReason) const override;
	virtual FText GetDisplayName() const override;
	virtual bool Transcribe(TArray<uint8> WavFile, const FStageTranscriptionOptions& Options, FOnStageTranscriptionFinished OnFinished) override;
	virtual void Cancel() override;
	//~ End UStageSpeechToTextBackend Interface

	//~ Begin UObject Interface
	virtual void BeginDestroy() override;
	//~ End UObject Interface

private:
	/** The request in flight; its completion delegate is unbound on Cancel so it can never call back afterwards. */
	TSharedPtr<class IHttpRequest, ESPMode::ThreadSafe> PendingRequest;
};
