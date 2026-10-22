// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "StageSpeechTypes.generated.h"

/** What the assistant's microphone is doing (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.5). */
UENUM(BlueprintType)
enum class EStageVoiceInputState : uint8
{
	Idle,
	/** The microphone is open and recording the prompt. */
	Recording,
	/** The recording was sent for transcription; waiting for the text. */
	Transcribing,
};

/** The language the speech service is told to expect. Auto lets it detect English or Armenian per recording. */
UENUM()
enum class EStageSpeechLanguage : uint8
{
	Auto,
	English,
	Armenian,
};

/** One recording, mono, in -1..1. */
struct FStageVoiceClip
{
	TArray<float> Samples;
	int32 SampleRate = 0;

	double GetSeconds() const { return SampleRate > 0 ? double(Samples.Num()) / double(SampleRate) : 0.0; }
};

/** How one recording is transcribed. */
struct FStageTranscriptionOptions
{
	FString Model;
	/** ISO 639-1 ("en", "hy"), or empty to let the service detect the language. */
	FString LanguageCode;
	/** Words the service should expect (stage vocabulary in both languages). */
	FString VocabularyHint;
	float TimeoutSeconds = 30.f;
};

/** The transcript of one recording, or why there is none. */
struct FStageTranscriptionResult
{
	bool bSucceeded = false;
	FString Text;
	FText Error;
};

/**
 * Pure audio and wire-format helpers of voice prompts (ADR 0017 §2.5): resampling to the transcription rate, loudness, a 16-bit
 * PCM WAV file, the multipart/form-data body of an upload and the transcription response. No HTTP, no devices; covered by the
 * StageCraft.AI.Voice tests. Deterministic.
 */
namespace StageSpeechAudio
{
	/** The rate recordings are sent at: plenty for speech, and a 30 s clip stays under 1 MB. */
	constexpr int32 TranscriptionSampleRate = 16000;

	/** Shorter recordings (a click on MIC and straight back) are not sent. */
	constexpr double MinSeconds = 0.3;

	/** A recording quieter than this RMS is treated as silence and not sent. */
	constexpr float SilenceRms = 0.003f;

	/**
	 * Samples converted from FromRate to ToRate: each output sample averages the input it covers when downsampling (a box filter,
	 * so higher frequencies do not fold into speech), and interpolates linearly when upsampling. Empty for a rate that is not positive.
	 */
	MODULARSCENEBUILDER_API TArray<float> Resample(TConstArrayView<float> Samples, int32 FromRate, int32 ToRate);

	/** Root mean square of the samples; 0 for none. */
	MODULARSCENEBUILDER_API float MeasureRms(TConstArrayView<float> Samples);

	/** A mono 16-bit PCM WAV file (RIFF, 44-byte header) of the samples, clamped to -1..1. */
	MODULARSCENEBUILDER_API TArray<uint8> EncodeWav(TConstArrayView<float> Samples, int32 SampleRate);

	/** One text field of a multipart/form-data body. */
	struct FFormField
	{
		FString Name;
		FString Value;
	};

	/**
	 * A multipart/form-data body (RFC 7578): every field as UTF-8 text, then the file part, then the closing boundary. Boundary must
	 * not occur in the content (MakeBoundary gives a random one).
	 */
	MODULARSCENEBUILDER_API TArray<uint8> BuildMultipartBody(const FString& Boundary, TConstArrayView<FFormField> Fields, const FString& FileField,
		const FString& FileName, const FString& ContentType, TConstArrayView<uint8> File);

	/** A boundary that will not occur in a WAV file or in the text fields. */
	MODULARSCENEBUILDER_API FString MakeBoundary(const FGuid& Seed);

	/**
	 * Reads a transcription response ({"text": "..."}). Failed, with the service's error message, for a status other than 200, a
	 * body that is not JSON or has no text; the text is trimmed and capped at MaxTranscriptLength characters.
	 */
	MODULARSCENEBUILDER_API FStageTranscriptionResult ParseTranscriptionResponse(int32 HttpStatus, const FString& Body);

	/** Longest transcript kept: the assistant reads at most StageAICommandParser::MaxPromptLength characters. */
	constexpr int32 MaxTranscriptLength = 500;
}
