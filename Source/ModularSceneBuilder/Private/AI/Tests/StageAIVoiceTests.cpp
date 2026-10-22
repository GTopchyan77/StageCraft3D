// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for voice prompts (Docs/ADR/0017-multilingual-voice-and-photo-import.md §2.5): resampling and loudness, the WAV file, the
// multipart upload body, the transcription response, the settings, and the voice input component refusing while turned off.
// The microphone and the speech service themselves need hardware and a key, so they are verified by hand (STATE.md #44).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.AI; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AI/StageAIAssistantSettings.h"
#include "AI/StageSpeechTypes.h"
#include "AI/StageVoiceInputComponent.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include <limits>

namespace StageAIVoiceTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	TArray<float> Sine(int32 Rate, double Seconds, double Frequency, float Amplitude)
	{
		TArray<float> Samples;
		const int32 Count = int32(Rate * Seconds);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Samples.Add(Amplitude * float(FMath::Sin(2.0 * UE_DOUBLE_PI * Frequency * double(Index) / double(Rate))));
		}
		return Samples;
	}

	uint32 ReadUInt32(const TArray<uint8>& Bytes, int32 Offset)
	{
		return uint32(Bytes[Offset]) | (uint32(Bytes[Offset + 1]) << 8) | (uint32(Bytes[Offset + 2]) << 16) | (uint32(Bytes[Offset + 3]) << 24);
	}

	int16 ReadInt16(const TArray<uint8>& Bytes, int32 Offset)
	{
		return int16(uint16(Bytes[Offset]) | (uint16(Bytes[Offset + 1]) << 8));
	}

	bool ContainsBytes(const TArray<uint8>& Haystack, const FString& Needle)
	{
		const FTCHARToUTF8 Utf8(*Needle);
		const int32 Length = Utf8.Length();
		for (int32 Start = 0; Start + Length <= Haystack.Num(); ++Start)
		{
			if (FMemory::Memcmp(Haystack.GetData() + Start, Utf8.Get(), Length) == 0)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageAIVoiceTest, "StageCraft.AI.Voice", StageAIVoiceTests::Flags)

bool FStageAIVoiceTest::RunTest(const FString& Parameters)
{
	using namespace StageAIVoiceTests;
	using namespace StageSpeechAudio;

	// --- Resampling and loudness ---
	{
		const TArray<float> Recorded = Sine(48000, 1.0, 440.0, 0.5f);
		const TArray<float> Speech = Resample(Recorded, 48000, TranscriptionSampleRate);
		TestEqual(TEXT("Resample: 48 kHz to 16 kHz keeps the length in time"), Speech.Num(), 16000);
		TestTrue(TEXT("Resample: a 440 Hz tone keeps its loudness"), FMath::IsNearlyEqual(MeasureRms(Speech), MeasureRms(Recorded), 0.02f));
		const TArray<float> High = Resample(Sine(48000, 1.0, 15000.0, 0.5f), 48000, TranscriptionSampleRate);
		TestTrue(TEXT("Resample: a tone above the new Nyquist rate is damped, not folded in at full level"), MeasureRms(High) < 0.2f);
		TestEqual(TEXT("Resample: same rate copies"), Resample(Speech, 16000, 16000).Num(), 16000);
		TestEqual(TEXT("Resample: upsampling doubles"), Resample(Sine(8000, 0.5, 200.0, 0.5f), 8000, 16000).Num(), 8000);
		TestEqual(TEXT("Resample: bad rate"), Resample(Speech, 0, 16000).Num(), 0);
		TestEqual(TEXT("Resample: nothing"), Resample(TArray<float>(), 48000, 16000).Num(), 0);
		TestEqual(TEXT("RMS: silence"), MeasureRms(TArray<float>{ 0.f, 0.f, 0.f }), 0.f);
		TestEqual(TEXT("RMS: full scale"), MeasureRms(TArray<float>{ 1.f, -1.f }), 1.f);
		TestEqual(TEXT("RMS: none"), MeasureRms(TArray<float>()), 0.f);
	}

	// --- WAV ---
	{
		const TArray<float> Samples = { 1.f, -1.f, 0.f, 2.f, std::numeric_limits<float>::quiet_NaN(), 0.5f };
		const TArray<uint8> Wav = EncodeWav(Samples, 16000);
		TestEqual(TEXT("WAV: size"), Wav.Num(), 44 + 2 * Samples.Num());
		TestTrue(TEXT("WAV: RIFF/WAVE/fmt/data tags"), FMemory::Memcmp(Wav.GetData(), "RIFF", 4) == 0 && FMemory::Memcmp(Wav.GetData() + 8, "WAVE", 4) == 0
			&& FMemory::Memcmp(Wav.GetData() + 12, "fmt ", 4) == 0 && FMemory::Memcmp(Wav.GetData() + 36, "data", 4) == 0);
		TestEqual(TEXT("WAV: RIFF size"), ReadUInt32(Wav, 4), uint32(36 + 2 * Samples.Num()));
		TestEqual(TEXT("WAV: PCM, mono"), ReadUInt32(Wav, 20), uint32(1 | (1 << 16)));
		TestEqual(TEXT("WAV: rate"), ReadUInt32(Wav, 24), 16000u);
		TestEqual(TEXT("WAV: byte rate"), ReadUInt32(Wav, 28), 32000u);
		TestEqual(TEXT("WAV: block align and bits"), ReadUInt32(Wav, 32), uint32(2 | (16 << 16)));
		TestEqual(TEXT("WAV: data size"), ReadUInt32(Wav, 40), uint32(2 * Samples.Num()));
		TestEqual(TEXT("WAV: +1"), ReadInt16(Wav, 44), int16(32767));
		TestEqual(TEXT("WAV: -1"), ReadInt16(Wav, 46), int16(-32767));
		TestEqual(TEXT("WAV: 0"), ReadInt16(Wav, 48), int16(0));
		TestEqual(TEXT("WAV: clamped"), ReadInt16(Wav, 50), int16(32767));
		TestEqual(TEXT("WAV: NaN is silence"), ReadInt16(Wav, 52), int16(0));
		TestEqual(TEXT("WAV: half"), ReadInt16(Wav, 54), int16(16384));
		TestTrue(TEXT("WAV: 30 s of speech stays under 1 MB"), EncodeWav(Sine(TranscriptionSampleRate, 30.0, 200.0, 0.3f), TranscriptionSampleRate).Num() < 1024 * 1024);
	}

	// --- Multipart body ---
	{
		const TArray<uint8> File = { 'R', 'I', 'F', 'F', 0, 1, 2, 3 };
		const FString Boundary = MakeBoundary(FGuid(1, 2, 3, 4));
		TestTrue(TEXT("Boundary: prefix"), Boundary.StartsWith(TEXT("----StageCraftVoice")));
		const FFormField Fields[] = { { TEXT("model"), TEXT("gpt-4o-transcribe") }, { TEXT("prompt"), TEXT("ֆերմա, cue") } };
		const TArray<uint8> Body = BuildMultipartBody(Boundary, Fields, TEXT("file"), TEXT("prompt.wav"), TEXT("audio/wav"), File);
		TestTrue(TEXT("Multipart: text field"), ContainsBytes(Body, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\ngpt-4o-transcribe\r\n"), *Boundary)));
		TestTrue(TEXT("Multipart: Armenian as UTF-8"), ContainsBytes(Body, TEXT("name=\"prompt\"\r\n\r\nֆերմա, cue\r\n")));
		TestTrue(TEXT("Multipart: file header"), ContainsBytes(Body, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"prompt.wav\"\r\nContent-Type: audio/wav\r\n\r\nRIFF"), *Boundary)));
		const FTCHARToUTF8 Closing(*FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));
		TestTrue(TEXT("Multipart: binary file then the closing boundary"), Body.Num() > Closing.Length()
			&& FMemory::Memcmp(Body.GetData() + Body.Num() - Closing.Length(), Closing.Get(), Closing.Length()) == 0
			&& FMemory::Memcmp(Body.GetData() + Body.Num() - Closing.Length() - 4, File.GetData() + 4, 4) == 0);
	}

	// --- Transcription response ---
	{
		const FStageTranscriptionResult English = ParseTranscriptionResponse(200, TEXT("{\"text\":\"  add four moving heads  \"}"));
		TestTrue(TEXT("Response: text trimmed"), English.bSucceeded && English.Text == TEXT("add four moving heads"));
		const FStageTranscriptionResult Armenian = ParseTranscriptionResponse(200, TEXT("{\"text\":\"Ավելացրու չորս շարժական գլուխ\"}"));
		TestTrue(TEXT("Response: Armenian"), Armenian.bSucceeded && Armenian.Text == TEXT("Ավելացրու չորս շարժական գլուխ"));
		const FStageTranscriptionResult Unauthorized = ParseTranscriptionResponse(401, TEXT("{\"error\":{\"message\":\"Incorrect API key provided\",\"type\":\"invalid_request_error\"}}"));
		TestTrue(TEXT("Response: 401 with the service's message"), !Unauthorized.bSucceeded && Unauthorized.Error.ToString().Contains(TEXT("401"))
			&& Unauthorized.Error.ToString().Contains(TEXT("Incorrect API key")));
		TestFalse(TEXT("Response: error object on 200"), ParseTranscriptionResponse(200, TEXT("{\"error\":{\"message\":\"x\"}}")).bSucceeded);
		TestFalse(TEXT("Response: not JSON"), ParseTranscriptionResponse(200, TEXT("<html>")).bSucceeded);
		TestFalse(TEXT("Response: no text"), ParseTranscriptionResponse(200, TEXT("{\"duration\":2}")).bSucceeded);
		TestFalse(TEXT("Response: empty text"), ParseTranscriptionResponse(200, TEXT("{\"text\":\"  \"}")).bSucceeded);
		const FStageTranscriptionResult Long = ParseTranscriptionResponse(200, FString::Printf(TEXT("{\"text\":\"%s\"}"), *FString::ChrN(900, TEXT('a'))));
		TestEqual(TEXT("Response: capped"), Long.Text.Len(), MaxTranscriptLength);
	}

	// --- Settings and the component while voice input is off (the shipped default) ---
	{
		const UStageAIAssistantSettings* Settings = GetDefault<UStageAIAssistantSettings>();
		const FStageTranscriptionOptions Options = Settings->GetTranscriptionOptions();
		TestEqual(TEXT("Settings: model"), Options.Model, FString(TEXT("gpt-4o-transcribe")));
		TestTrue(TEXT("Settings: language detected by default"), Options.LanguageCode.IsEmpty());
		TestTrue(TEXT("Settings: vocabulary in both languages"), Options.VocabularyHint.Contains(TEXT("truss")) && Options.VocabularyHint.Contains(TEXT("ֆերմա")));
		TestTrue(TEXT("Settings: limits"), Settings->GetMaxRecordingSeconds() >= 3.f && Settings->GetMaxRecordingSeconds() <= 120.f && Options.TimeoutSeconds >= 5.f);
		TestEqual(TEXT("Settings: key variable"), FString(UStageAIAssistantSettings::GetSpeechApiKeyVariable()), FString(TEXT("OPENAI_API_KEY")));

		if (!Settings->UsesVoiceInput())
		{
			UStageVoiceInputComponent* Voice = NewObject<UStageVoiceInputComponent>(GetTransientPackage());
			FText Reason;
			TestFalse(TEXT("Component: unavailable while off"), Voice->IsAvailable(Reason));
			TestTrue(TEXT("Component: says where to turn it on"), Reason.ToString().Contains(TEXT("Voice Input")));
			TestFalse(TEXT("Component: does not record while off"), Voice->StartRecording());
			TestTrue(TEXT("Component: stays idle with the reason"), Voice->GetState() == EStageVoiceInputState::Idle && Voice->GetStatus().EqualTo(Reason));
			Voice->StopRecording();
			Voice->Cancel();
			TestTrue(TEXT("Component: stop and cancel while idle are harmless"), Voice->GetState() == EStageVoiceInputState::Idle);
		}
		else
		{
			AddInfo(TEXT("Voice input is on in this project's settings; the off-state checks were skipped."));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
