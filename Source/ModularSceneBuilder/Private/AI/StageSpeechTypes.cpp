// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageSpeechTypes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSpeechTypes)

#define LOCTEXT_NAMESPACE "StageSpeechAudio"

namespace StageSpeechAudio
{
	namespace
	{
		void AppendUtf8(TArray<uint8>& Out, const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}

		void AppendUInt32(TArray<uint8>& Out, uint32 Value)
		{
			for (int32 Byte = 0; Byte < 4; ++Byte)
			{
				Out.Add(uint8((Value >> (8 * Byte)) & 0xFF));
			}
		}

		void AppendUInt16(TArray<uint8>& Out, uint16 Value)
		{
			Out.Add(uint8(Value & 0xFF));
			Out.Add(uint8(Value >> 8));
		}

		void AppendTag(TArray<uint8>& Out, const char (&Tag)[5])
		{
			Out.Append(reinterpret_cast<const uint8*>(Tag), 4);
		}
	}

	TArray<float> Resample(TConstArrayView<float> Samples, int32 FromRate, int32 ToRate)
	{
		TArray<float> Out;
		if (FromRate <= 0 || ToRate <= 0 || Samples.IsEmpty())
		{
			return Out;
		}
		if (FromRate == ToRate)
		{
			Out.Append(Samples.GetData(), Samples.Num());
			return Out;
		}
		const double Step = double(FromRate) / double(ToRate);
		const int32 Count = FMath::Max(1, int32(FMath::FloorToDouble(double(Samples.Num()) / Step)));
		Out.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Start = double(Index) * Step;
			if (Step > 1.0)
			{
				// The input samples this output sample covers, averaged.
				const int32 First = FMath::Clamp(int32(Start), 0, Samples.Num() - 1);
				const int32 Last = FMath::Clamp(int32(Start + Step), First + 1, Samples.Num());
				double Sum = 0.0;
				for (int32 Source = First; Source < Last; ++Source)
				{
					Sum += Samples[Source];
				}
				Out.Add(float(Sum / double(Last - First)));
			}
			else
			{
				const int32 Below = FMath::Clamp(int32(Start), 0, Samples.Num() - 1);
				const int32 Above = FMath::Min(Below + 1, Samples.Num() - 1);
				const float Alpha = float(Start - double(Below));
				Out.Add(FMath::Lerp(Samples[Below], Samples[Above], Alpha));
			}
		}
		return Out;
	}

	float MeasureRms(TConstArrayView<float> Samples)
	{
		if (Samples.IsEmpty())
		{
			return 0.f;
		}
		double Sum = 0.0;
		for (const float Sample : Samples)
		{
			Sum += double(Sample) * double(Sample);
		}
		return float(FMath::Sqrt(Sum / double(Samples.Num())));
	}

	TArray<uint8> EncodeWav(TConstArrayView<float> Samples, int32 SampleRate)
	{
		constexpr uint16 Channels = 1;
		constexpr uint16 BitsPerSample = 16;
		const uint32 DataBytes = uint32(Samples.Num()) * Channels * (BitsPerSample / 8);
		TArray<uint8> Wav;
		Wav.Reserve(44 + int32(DataBytes));
		AppendTag(Wav, "RIFF");
		AppendUInt32(Wav, 36 + DataBytes);
		AppendTag(Wav, "WAVE");
		AppendTag(Wav, "fmt ");
		AppendUInt32(Wav, 16);
		AppendUInt16(Wav, 1);	// PCM
		AppendUInt16(Wav, Channels);
		AppendUInt32(Wav, uint32(FMath::Max(SampleRate, 0)));
		AppendUInt32(Wav, uint32(FMath::Max(SampleRate, 0)) * Channels * (BitsPerSample / 8));
		AppendUInt16(Wav, Channels * (BitsPerSample / 8));
		AppendUInt16(Wav, BitsPerSample);
		AppendTag(Wav, "data");
		AppendUInt32(Wav, DataBytes);
		for (const float Sample : Samples)
		{
			const float Clamped = FMath::IsFinite(Sample) ? FMath::Clamp(Sample, -1.f, 1.f) : 0.f;
			AppendUInt16(Wav, uint16(int16(FMath::RoundToInt(Clamped * 32767.f))));
		}
		return Wav;
	}

	TArray<uint8> BuildMultipartBody(const FString& Boundary, TConstArrayView<FFormField> Fields, const FString& FileField, const FString& FileName,
		const FString& ContentType, TConstArrayView<uint8> File)
	{
		TArray<uint8> Body;
		Body.Reserve(File.Num() + 1024);
		for (const FFormField& Field : Fields)
		{
			AppendUtf8(Body, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n"), *Boundary, *Field.Name));
			AppendUtf8(Body, Field.Value);
			AppendUtf8(Body, TEXT("\r\n"));
		}
		AppendUtf8(Body, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: %s\r\n\r\n"),
			*Boundary, *FileField, *FileName, *ContentType));
		Body.Append(File.GetData(), File.Num());
		AppendUtf8(Body, FString::Printf(TEXT("\r\n--%s--\r\n"), *Boundary));
		return Body;
	}

	FString MakeBoundary(const FGuid& Seed)
	{
		return FString::Printf(TEXT("----StageCraftVoice%s"), *Seed.ToString(EGuidFormats::Digits));
	}

	FStageTranscriptionResult ParseTranscriptionResponse(int32 HttpStatus, const FString& Body)
	{
		FStageTranscriptionResult Result;
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Body), Root);
		FString ErrorMessage;
		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (Root.IsValid() && Root->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject)
		{
			(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
		}
		if (HttpStatus != 200 || !Root.IsValid() || ErrorObject)
		{
			Result.Error = FText::Format(LOCTEXT("HttpError", "The speech service answered with HTTP {0}{1}."), FText::AsNumber(HttpStatus),
				ErrorMessage.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT(": ") + ErrorMessage.Left(200)));
			return Result;
		}
		FString Text;
		if (!Root->TryGetStringField(TEXT("text"), Text))
		{
			Result.Error = LOCTEXT("NoText", "The speech service's answer had no text.");
			return Result;
		}
		Text = Text.TrimStartAndEnd().Left(MaxTranscriptLength).TrimEnd();
		if (Text.IsEmpty())
		{
			Result.Error = LOCTEXT("NothingHeard", "No words were recognised in the recording.");
			return Result;
		}
		Result.bSucceeded = true;
		Result.Text = MoveTemp(Text);
		return Result;
	}
}

#undef LOCTEXT_NAMESPACE
