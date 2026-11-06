// Copyright Epic Games, Inc. All Rights Reserved.

// Phase H0 spike (Docs/ADR/0030 §8, STATE.md #72): StageCraft.Assistant.Spike streams Messages API requests shaped like the assistant v2
// prefix (the stage_command tool, then the system prompt with the knowledge digest and a cache breakpoint) and measures, per model:
// time to the first byte, the first content block and the first text, how the body arrives (chunks over time), the cache write and
// read tokens, and the cost. Each model is asked twice, so the second request shows whether the prefix was read from the cache.
// Url= points it at a local mock server (Scripts/Assistant/MockSseServer.ps1) to check streaming with no key and no cost.
// Development only: compiled out of Shipping, never run by tests (no network in tests).

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "AI/StageAICommandSchema.h"
#include "AI/StageClaudeHttp.h"
#include "AI/StageKnowledgeSubsystem.h"
#include "AI/StageSseParser.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace StageClaudeSpike
{
	/** Prices per million tokens (input, output, cache read), from the API model table on 2026-11-06; a 5-minute cache write is 1.25 x input. */
	struct FPrice
	{
		const TCHAR* Model;
		double Input;
		double Output;
		double CacheRead;
	};
	// Haiku 5.5's cache read is not listed in the table used; 0.1 x input is the documented default ratio.
	const FPrice Prices[] = {
		{ TEXT("claude-opus-5-5"), 4.0, 20.0, 0.20 },
		{ TEXT("claude-sonnet-5-5"), 2.0, 10.0, 0.20 },
		{ TEXT("claude-haiku-5-5"), 0.10, 0.50, 0.01 },
	};

	/** What the HTTP thread records while the body streams in; read on the game thread once the request completes. */
	struct FStreamState
	{
		FCriticalSection Lock;
		double StartSeconds = 0.0;
		double FirstByteSeconds = -1.0;
		double LastByteSeconds = -1.0;
		int32 Chunks = 0;
		int64 Bytes = 0;
		FStageSseParser Parser;
		TArray<TPair<double, FStageSseEvent>> Events;
		/** The first 4 KB of the body as it came, to report an error response. */
		TArray<uint8> Head;
	};

	struct FRunResult
	{
		FString Model;
		int32 Run = 0;
		int32 Status = 0;
		double FirstByteMs = -1.0;
		double FirstBlockMs = -1.0;
		double FirstTextMs = -1.0;
		double TotalMs = 0.0;
		int32 Chunks = 0;
		int32 TextDeltas = 0;
		double SpreadMs = 0.0;
		int64 InputTokens = 0;
		int64 CacheWriteTokens = 0;
		int64 CacheReadTokens = 0;
		int64 OutputTokens = 0;
		FString StopReason;
		FString Error;
		double CostUsd = 0.0;
	};

	struct FSpike
	{
		TArray<FString> Models;
		int32 RunsPerModel = 2;
		FString Url;
		bool bFallbacks = true;
		FString Body;
		int32 Next = 0;
		TArray<FRunResult> Results;
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
	};
	TSharedPtr<FSpike> Active;

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> Parse(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	/** The assistant v2 request shape (ADR 0030 §8): tools, then the system prompt and digest with the cache breakpoint, then the prompt. */
	FString BuildBody(const FString& Model, bool bFallbacks, const FString& Digest)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Model);
		Body->SetNumberField(TEXT("max_tokens"), 1024);
		Body->SetBoolField(TEXT("stream"), true);
		if (bFallbacks)
		{
			Body->SetStringField(TEXT("fallbacks"), TEXT("default"));
		}
		TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
		OutputConfig->SetStringField(TEXT("effort"), TEXT("low"));
		Body->SetObjectField(TEXT("output_config"), OutputConfig);

		TArray<TSharedPtr<FJsonValue>> Tools;
		if (const TSharedPtr<FJsonObject> Tool = Parse(StageAICommandSchema::MakeToolDefinitionJson()))
		{
			Tools.Add(MakeShared<FJsonValueObject>(Tool));
		}
		Body->SetArrayField(TEXT("tools"), Tools);
		TSharedRef<FJsonObject> ToolChoice = MakeShared<FJsonObject>();
		ToolChoice->SetStringField(TEXT("type"), TEXT("auto"));
		Body->SetObjectField(TEXT("tool_choice"), ToolChoice);

		TSharedRef<FJsonObject> System = MakeShared<FJsonObject>();
		System->SetStringField(TEXT("type"), TEXT("text"));
		System->SetStringField(TEXT("text"), StageAICommandSchema::GetSystemPrompt() + TEXT("\n\nWhat StageCraft has (names only):\n") + Digest);
		TSharedRef<FJsonObject> Cache = MakeShared<FJsonObject>();
		Cache->SetStringField(TEXT("type"), TEXT("ephemeral"));
		System->SetObjectField(TEXT("cache_control"), Cache);
		Body->SetArrayField(TEXT("system"), { MakeShared<FJsonValueObject>(System) });

		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), TEXT("user"));
		Message->SetStringField(TEXT("content"), TEXT("In one sentence: where is the blackout button?"));
		Body->SetArrayField(TEXT("messages"), { MakeShared<FJsonValueObject>(Message) });
		return Serialize(Body);
	}

	double PriceRun(FRunResult& Result)
	{
		for (const FPrice& Price : Prices)
		{
			if (Result.Model == Price.Model)
			{
				return (Result.InputTokens * Price.Input + Result.CacheWriteTokens * Price.Input * 1.25 + Result.CacheReadTokens * Price.CacheRead
					+ Result.OutputTokens * Price.Output) / 1.0e6;
			}
		}
		return 0.0;
	}

	FRunResult Summarise(const FString& Model, int32 Run, int32 Status, FStreamState& State)
	{
		FScopeLock Guard(&State.Lock);
		TArray<FStageSseEvent> Tail;
		State.Parser.Finish(Tail);
		for (FStageSseEvent& Event : Tail)
		{
			State.Events.Emplace(State.LastByteSeconds, MoveTemp(Event));
		}

		FRunResult Result;
		Result.Model = Model;
		Result.Run = Run;
		Result.Status = Status;
		Result.Chunks = State.Chunks;
		const auto Ms = [&State](double Seconds) { return Seconds < 0.0 ? -1.0 : (Seconds - State.StartSeconds) * 1000.0; };
		Result.FirstByteMs = Ms(State.FirstByteSeconds);
		Result.TotalMs = Ms(State.LastByteSeconds);
		Result.SpreadMs = State.FirstByteSeconds < 0.0 ? 0.0 : (State.LastByteSeconds - State.FirstByteSeconds) * 1000.0;
		for (const TPair<double, FStageSseEvent>& Timed : State.Events)
		{
			const TSharedPtr<FJsonObject> Data = Parse(Timed.Value.Data);
			if (!Data.IsValid())
			{
				continue;
			}
			const FString Type = Data->GetStringField(TEXT("type"));
			const TSharedPtr<FJsonObject>* Usage = nullptr;
			if (Type == TEXT("message_start"))
			{
				const TSharedPtr<FJsonObject>* Message = nullptr;
				if (Data->TryGetObjectField(TEXT("message"), Message) && (*Message)->TryGetObjectField(TEXT("usage"), Usage))
				{
					(*Usage)->TryGetNumberField(TEXT("input_tokens"), Result.InputTokens);
					(*Usage)->TryGetNumberField(TEXT("cache_creation_input_tokens"), Result.CacheWriteTokens);
					(*Usage)->TryGetNumberField(TEXT("cache_read_input_tokens"), Result.CacheReadTokens);
				}
			}
			else if (Type == TEXT("content_block_start") && Result.FirstBlockMs < 0.0)
			{
				Result.FirstBlockMs = Ms(Timed.Key);
			}
			else if (Type == TEXT("content_block_delta"))
			{
				const TSharedPtr<FJsonObject>* Delta = nullptr;
				if (Data->TryGetObjectField(TEXT("delta"), Delta) && (*Delta)->GetStringField(TEXT("type")) == TEXT("text_delta"))
				{
					++Result.TextDeltas;
					if (Result.FirstTextMs < 0.0)
					{
						Result.FirstTextMs = Ms(Timed.Key);
					}
				}
			}
			else if (Type == TEXT("message_delta"))
			{
				const TSharedPtr<FJsonObject>* Delta = nullptr;
				if (Data->TryGetObjectField(TEXT("delta"), Delta))
				{
					(*Delta)->TryGetStringField(TEXT("stop_reason"), Result.StopReason);
				}
				if (Data->TryGetObjectField(TEXT("usage"), Usage))
				{
					(*Usage)->TryGetNumberField(TEXT("output_tokens"), Result.OutputTokens);
				}
			}
			else if (Type == TEXT("error"))
			{
				const TSharedPtr<FJsonObject>* Error = nullptr;
				if (Data->TryGetObjectField(TEXT("error"), Error))
				{
					Result.Error = (*Error)->GetStringField(TEXT("message")).Left(200);
				}
			}
		}
		if (Status != 200 && Result.Error.IsEmpty())
		{
			const FUTF8ToTCHAR Head(reinterpret_cast<const ANSICHAR*>(State.Head.GetData()), State.Head.Num());
			Result.Error = FString(Head.Length(), Head.Get()).Left(300).Replace(TEXT("\n"), TEXT(" "));
		}
		Result.CostUsd = PriceRun(Result);
		return Result;
	}

	void Report(const FRunResult& Result)
	{
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Spike: %s run %d: HTTP %d, first byte %.0f ms, first block %.0f ms, first text %.0f ms, total %.0f ms; %d chunks over %.0f ms, %d text deltas; input %lld, cache write %lld, cache read %lld, output %lld tokens; stop %s; $%.5f%s%s"),
			*Result.Model, Result.Run, Result.Status, Result.FirstByteMs, Result.FirstBlockMs, Result.FirstTextMs, Result.TotalMs, Result.Chunks, Result.SpreadMs,
			Result.TextDeltas, Result.InputTokens, Result.CacheWriteTokens, Result.CacheReadTokens, Result.OutputTokens,
			Result.StopReason.IsEmpty() ? TEXT("-") : *Result.StopReason, Result.CostUsd, Result.Error.IsEmpty() ? TEXT("") : TEXT("; error: "), *Result.Error);
	}

	void WriteCsv(const TArray<FRunResult>& Results)
	{
		const FString Path = FPaths::Combine(FPaths::ProfilingDir(), TEXT("AssistantSpike.csv"));
		FString Csv;
		if (!FPaths::FileExists(Path))
		{
			Csv = TEXT("time,model,run,status,first_byte_ms,first_block_ms,first_text_ms,total_ms,chunks,spread_ms,text_deltas,input,cache_write,cache_read,output,stop,cost_usd,error\n");
		}
		const FString Now = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
		for (const FRunResult& Result : Results)
		{
			Csv += FString::Printf(TEXT("%s,%s,%d,%d,%.0f,%.0f,%.0f,%.0f,%d,%.0f,%d,%lld,%lld,%lld,%lld,%s,%.5f,\"%s\"\n"), *Now, *Result.Model, Result.Run, Result.Status,
				Result.FirstByteMs, Result.FirstBlockMs, Result.FirstTextMs, Result.TotalMs, Result.Chunks, Result.SpreadMs, Result.TextDeltas, Result.InputTokens,
				Result.CacheWriteTokens, Result.CacheReadTokens, Result.OutputTokens, *Result.StopReason, Result.CostUsd, *Result.Error.Replace(TEXT("\""), TEXT("'")));
		}
		FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Spike: wrote %d rows to %s"), Results.Num(), *Path);
	}

	void StartNext(TSharedRef<FSpike> Spike);

	void StartRun(TSharedRef<FSpike> Spike, const FString& Model, int32 Run, const FString& Digest)
	{
		const FString ApiKey = StageClaudeHttp::ReadApiKey();
		const FString Body = BuildBody(Model, Spike->bFallbacks, Digest);
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = StageClaudeHttp::MakeMessagesRequest(ApiKey.IsEmpty() ? TEXT("mock") : ApiKey, Body, 60.f);
		if (!Spike->Url.IsEmpty())
		{
			Request->SetURL(Spike->Url);
		}
		TSharedRef<FStreamState, ESPMode::ThreadSafe> State = MakeShared<FStreamState, ESPMode::ThreadSafe>();
		// Called on the HTTP thread for each piece of the body as it arrives.
		Request->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([State](void* Data, int64& Length)
		{
			const double Now = FPlatformTime::Seconds();
			FScopeLock Guard(&State->Lock);
			if (State->FirstByteSeconds < 0.0)
			{
				State->FirstByteSeconds = Now;
			}
			State->LastByteSeconds = Now;
			++State->Chunks;
			State->Bytes += Length;
			const uint8* Bytes = static_cast<const uint8*>(Data);
			const int32 Count = static_cast<int32>(FMath::Min<int64>(Length, MAX_int32));
			const int32 HeadRoom = FMath::Max(0, 4096 - State->Head.Num());
			State->Head.Append(Bytes, FMath::Min(Count, HeadRoom));
			TArray<FStageSseEvent> Events;
			State->Parser.Feed(Bytes, Count, Events);
			for (FStageSseEvent& Event : Events)
			{
				State->Events.Emplace(Now, MoveTemp(Event));
			}
		}));
		const TWeakPtr<FSpike> WeakSpike = Spike;
		Request->OnProcessRequestComplete().BindLambda([WeakSpike, State, Model, Run](FHttpRequestPtr, FHttpResponsePtr Response, bool)
		{
			const TSharedPtr<FSpike> Spike = WeakSpike.Pin();
			if (!Spike.IsValid())
			{
				return;
			}
			{
				FScopeLock Guard(&State->Lock);
				if (State->LastByteSeconds < 0.0)
				{
					State->LastByteSeconds = FPlatformTime::Seconds();
				}
			}
			const FRunResult Result = Summarise(Model, Run, Response.IsValid() ? Response->GetResponseCode() : 0, *State);
			Report(Result);
			Spike->Results.Add(Result);
			Spike->Request.Reset();
			StartNext(Spike.ToSharedRef());
		});
		State->StartSeconds = FPlatformTime::Seconds();
		Spike->Request = Request;
		if (!Request->ProcessRequest())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Spike: %s run %d could not start."), *Model, Run);
			Spike->Request.Reset();
		}
	}

	FString Digest;

	void StartNext(TSharedRef<FSpike> Spike)
	{
		const int32 Total = Spike->Models.Num() * Spike->RunsPerModel;
		if (Spike->Next >= Total)
		{
			WriteCsv(Spike->Results);
			double Cost = 0.0;
			for (const FRunResult& Result : Spike->Results)
			{
				Cost += Result.CostUsd;
			}
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Spike: done, %d requests, $%.4f in total."), Spike->Results.Num(), Cost);
			if (Active.Get() == &Spike.Get())
			{
				Active.Reset();
			}
			return;
		}
		const int32 Index = Spike->Next++;
		StartRun(Spike, Spike->Models[Index / Spike->RunsPerModel], Index % Spike->RunsPerModel + 1, Digest);
	}

	void Run(const TArray<FString>& Args, UWorld* World)
	{
		if (Active.IsValid())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Spike: a spike is already running."));
			return;
		}
		TSharedRef<FSpike> Spike = MakeShared<FSpike>();
		FString Model = TEXT("claude-opus-5-5");
		for (const FString& Arg : Args)
		{
			FString Key;
			FString Value;
			if (!Arg.Split(TEXT("="), &Key, &Value))
			{
				Key = Arg;
			}
			if (Key.Equals(TEXT("Model"), ESearchCase::IgnoreCase))
			{
				Model = Value;
			}
			else if (Key.Equals(TEXT("Runs"), ESearchCase::IgnoreCase))
			{
				Spike->RunsPerModel = FMath::Clamp(FCString::Atoi(*Value), 1, 5);
			}
			else if (Key.Equals(TEXT("Url"), ESearchCase::IgnoreCase))
			{
				Spike->Url = Value;
			}
			else if (Key.Equals(TEXT("NoFallbacks"), ESearchCase::IgnoreCase))
			{
				Spike->bFallbacks = false;
			}
		}
		if (Model.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			for (const FPrice& Price : Prices)
			{
				Spike->Models.Add(Price.Model);
			}
		}
		else
		{
			Spike->Models.Add(Model);
		}
		if (Spike->Url.IsEmpty() && StageClaudeHttp::ReadApiKey().IsEmpty())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("StageCraft.Spike: no API key in the environment, and no Url= of a mock server. Nothing sent."));
			return;
		}
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UStageKnowledgeSubsystem* Knowledge = GameInstance ? GameInstance->GetSubsystem<UStageKnowledgeSubsystem>() : nullptr;
		Digest = Knowledge ? StageKnowledge::BuildDigest(Knowledge->GetIndex()) : FString();
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Spike: %d model(s) x %d run(s) to %s; system prefix %d characters (digest %d)."), Spike->Models.Num(),
			Spike->RunsPerModel, Spike->Url.IsEmpty() ? TEXT("the Messages API") : *Spike->Url, StageAICommandSchema::GetSystemPrompt().Len() + Digest.Len(), Digest.Len());
		Active = Spike;
		StartNext(Spike);
	}

	FAutoConsoleCommandWithWorldAndArgs SpikeCommand(TEXT("StageCraft.Assistant.Spike"),
		TEXT("[Model=claude-opus-5-5|claude-sonnet-5-5|claude-haiku-5-5|all] [Runs=2] [Url=http://127.0.0.1:8787/v1/messages] [NoFallbacks] Phase H0: streams assistant-shaped requests and logs first-token latency, chunking, cache write/read and cost per run (Saved/Profiling/AssistantSpike.csv). Spends real money without Url=."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Run));
}

#endif // !UE_BUILD_SHIPPING
