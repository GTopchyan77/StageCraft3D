// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/StageAICommandSchema.h"

#include "AI/StageAICommandParser.h"
#include "Algo/Find.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Show/ShowControlTypes.h"

#define LOCTEXT_NAMESPACE "StageAICommandSchema"

namespace StageAICommandSchema
{
	namespace
	{
		struct FIntentName
		{
			EStageAIIntent Intent;
			const TCHAR* Name;
		};

		const FIntentName IntentNames[] = {
			{ EStageAIIntent::Help, TEXT("help") },
			{ EStageAIIntent::StageSummary, TEXT("stage_summary") },
			{ EStageAIIntent::ItemCount, TEXT("item_count") },
			{ EStageAIIntent::TotalWeight, TEXT("total_weight") },
			{ EStageAIIntent::PowerDraw, TEXT("power_draw") },
			{ EStageAIIntent::FixtureCount, TEXT("fixture_count") },
			{ EStageAIIntent::CountMatching, TEXT("count_matching") },
			{ EStageAIIntent::ListCues, TEXT("list_cues") },
			{ EStageAIIntent::EnvironmentStatus, TEXT("environment_status") },
			{ EStageAIIntent::AddFixtures, TEXT("add_fixtures") },
			{ EStageAIIntent::StoreCue, TEXT("store_cue") },
			{ EStageAIIntent::GoToCue, TEXT("go_to_cue") },
			{ EStageAIIntent::CueGo, TEXT("cue_go") },
			{ EStageAIIntent::CueBack, TEXT("cue_back") },
			{ EStageAIIntent::SetWeather, TEXT("set_weather") },
			{ EStageAIIntent::SetTimeOfDay, TEXT("set_time_of_day") },
			{ EStageAIIntent::SetSky, TEXT("set_sky") },
			{ EStageAIIntent::Export, TEXT("export") },
			{ EStageAIIntent::Undo, TEXT("undo") },
			{ EStageAIIntent::Redo, TEXT("redo") },
			{ EStageAIIntent::Unsupported, TEXT("unsupported") },
		};
		static_assert(UE_ARRAY_COUNT(IntentNames) == int32(EStageAIIntent::Count), "Every intent needs a wire name.");

		struct FWeatherName { EStageWeatherPreset Preset; const TCHAR* Name; };
		const FWeatherName WeatherNames[] = {
			{ EStageWeatherPreset::Clear, TEXT("clear") }, { EStageWeatherPreset::Cloudy, TEXT("cloudy") }, { EStageWeatherPreset::Rain, TEXT("rain") },
			{ EStageWeatherPreset::Snow, TEXT("snow") }, { EStageWeatherPreset::Fog, TEXT("fog") } };

		struct FReportName { EStageAIReport Report; const TCHAR* Name; };
		const FReportName ReportNames[] = {
			{ EStageAIReport::BillOfMaterials, TEXT("bill_of_materials") }, { EStageAIReport::PatchSheet, TEXT("patch_sheet") },
			{ EStageAIReport::CableSchedule, TEXT("cable_schedule") }, { EStageAIReport::All, TEXT("all") } };

		const TCHAR* const UnsupportedOperations[] = { TEXT("delete"), TEXT("open"), TEXT("save") };

		FString TrimAndCap(const FString& Text)
		{
			return Text.TrimStartAndEnd().Left(MaxQueryLength).TrimEnd();
		}

		TArray<TSharedPtr<FJsonValue>> StringValues(TConstArrayView<const TCHAR*> Names)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TCHAR* Name : Names)
			{
				Values.Add(MakeShared<FJsonValueString>(Name));
			}
			return Values;
		}

		TSharedRef<FJsonObject> MakeProperty(const TCHAR* Type, const FString& Description)
		{
			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), Type);
			Property->SetStringField(TEXT("description"), Description);
			return Property;
		}

		TSharedRef<FJsonObject> MakeToolDefinition()
		{
			TArray<const TCHAR*> Intents, Weathers, Reports;
			for (const FIntentName& Entry : IntentNames) { Intents.Add(Entry.Name); }
			for (const FWeatherName& Entry : WeatherNames) { Weathers.Add(Entry.Name); }
			for (const FReportName& Entry : ReportNames) { Reports.Add(Entry.Name); }

			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Intent = MakeProperty(TEXT("string"), TEXT("What to do. Questions (stage_summary, item_count, total_weight, power_draw, fixture_count, count_matching, list_cues, environment_status) are answered from the live stage; the others change it."));
			Intent->SetArrayField(TEXT("enum"), StringValues(Intents));
			Properties->SetObjectField(TEXT("intent"), Intent);
			Properties->SetObjectField(TEXT("count"), MakeProperty(TEXT("integer"), FString::Printf(TEXT("add_fixtures: how many, 1 to %d. Default 1."), StageAICommandParser::MaxFixturesPerCommand)));
			Properties->SetObjectField(TEXT("item"), MakeProperty(TEXT("string"), TEXT("add_fixtures: words naming the fixture type or catalog name (e.g. \"moving head spot\"); empty picks the first fixture. count_matching: words naming the placed item to count. unsupported: one of delete, open, save.")));
			Properties->SetObjectField(TEXT("cue_number"), MakeProperty(TEXT("number"), TEXT("store_cue (omit for the next free number) and go_to_cue: the cue number, 0.001 to 9999.999.")));
			Properties->SetObjectField(TEXT("label"), MakeProperty(TEXT("string"), TEXT("store_cue: optional cue label.")));
			Properties->SetObjectField(TEXT("fade_seconds"), MakeProperty(TEXT("number"), TEXT("store_cue: optional crossfade time in seconds, 0 to 600.")));
			TSharedRef<FJsonObject> Weather = MakeProperty(TEXT("string"), TEXT("set_weather: the weather preset."));
			Weather->SetArrayField(TEXT("enum"), StringValues(Weathers));
			Properties->SetObjectField(TEXT("weather"), Weather);
			Properties->SetObjectField(TEXT("hour"), MakeProperty(TEXT("number"), TEXT("set_time_of_day: local time in hours, 0 to 24 (18.5 is 18:30).")));
			Properties->SetObjectField(TEXT("sky_on"), MakeProperty(TEXT("boolean"), TEXT("set_sky: true for the outdoor sky, false for an indoor stage.")));
			TSharedRef<FJsonObject> Report = MakeProperty(TEXT("string"), TEXT("export: which report (PDF + CSV)."));
			Report->SetArrayField(TEXT("enum"), StringValues(Reports));
			Properties->SetObjectField(TEXT("report"), Report);

			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Properties);
			Schema->SetArrayField(TEXT("required"), StringValues({ TEXT("intent") }));
			Schema->SetBoolField(TEXT("additionalProperties"), false);

			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), GetToolName());
			Tool->SetStringField(TEXT("description"), TEXT("Run one StageCraft 3D stage command: answer a question about the live stage, or make one safe, undoable change through the stage's rules. Call it once per request."));
			Tool->SetBoolField(TEXT("strict"), true);
			Tool->SetObjectField(TEXT("input_schema"), Schema);
			return Tool;
		}

		FString Serialize(const TSharedRef<FJsonObject>& Object)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
			return Out;
		}

		TSharedPtr<FJsonObject> Deserialize(const FString& Json)
		{
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
			return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
		}

		bool CommandFromToolInput(const FJsonObject& Input, FStageAICommand& OutCommand, FText& OutError)
		{
			static const TCHAR* const KnownFields[] = { TEXT("intent"), TEXT("count"), TEXT("item"), TEXT("cue_number"), TEXT("label"), TEXT("fade_seconds"),
				TEXT("weather"), TEXT("hour"), TEXT("sky_on"), TEXT("report") };
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Input.Values)
			{
				if (!Algo::FindByPredicate(KnownFields, [&Field](const TCHAR* Known) { return Field.Key == Known; }))
				{
					OutError = FText::Format(LOCTEXT("UnknownField", "The command had an unknown field \"{0}\"."), FText::FromString(Field.Key.Left(32)));
					return false;
				}
			}

			FString IntentName;
			if (!Input.TryGetStringField(TEXT("intent"), IntentName))
			{
				OutError = LOCTEXT("NoIntent", "The command had no intent.");
				return false;
			}
			const FIntentName* Intent = Algo::FindByPredicate(IntentNames, [&IntentName](const FIntentName& Entry) { return IntentName == Entry.Name; });
			if (!Intent)
			{
				OutError = FText::Format(LOCTEXT("UnknownIntent", "\"{0}\" is not a command I can run."), FText::FromString(IntentName.Left(32)));
				return false;
			}

			FStageAICommand Command;
			Command.Intent = Intent->Intent;
			const auto ReadNumber = [&Input, &OutError](const TCHAR* Field, TOptional<double>& Out)
			{
				const TSharedPtr<FJsonValue> Value = Input.TryGetField(Field);
				if (!Value.IsValid() || Value->IsNull())
				{
					return true;
				}
				double Number = 0.0;
				if (!Value->TryGetNumber(Number))
				{
					OutError = FText::Format(LOCTEXT("NotANumber", "\"{0}\" must be a number."), FText::FromString(Field));
					return false;
				}
				Out = Number;
				return true;
			};
			TOptional<double> Count, CueNumber, Fade, Hour;
			if (!ReadNumber(TEXT("count"), Count) || !ReadNumber(TEXT("cue_number"), CueNumber) || !ReadNumber(TEXT("fade_seconds"), Fade) || !ReadNumber(TEXT("hour"), Hour))
			{
				return false;
			}
			if (Count.IsSet())
			{
				if (!FMath::IsFinite(*Count) || *Count != FMath::RoundToDouble(*Count))
				{
					OutError = LOCTEXT("CountWhole", "The fixture count must be a whole number.");
					return false;
				}
				Command.Count = int32(FMath::Clamp(*Count, -1.0, 1000.0));
			}
			if (CueNumber.IsSet())
			{
				Command.CueNumber = float(*CueNumber);
			}
			if (Fade.IsSet())
			{
				Command.FadeSeconds = float(*Fade);
			}
			if (Hour.IsSet())
			{
				Command.Hour = float(*Hour);
			}
			Input.TryGetStringField(TEXT("item"), Command.ItemQuery);
			Input.TryGetStringField(TEXT("label"), Command.Label);
			Input.TryGetBoolField(TEXT("sky_on"), Command.bSkyOn);

			FString WeatherName;
			if (Input.TryGetStringField(TEXT("weather"), WeatherName))
			{
				const FWeatherName* Weather = Algo::FindByPredicate(WeatherNames, [&WeatherName](const FWeatherName& Entry) { return WeatherName == Entry.Name; });
				if (!Weather)
				{
					OutError = LOCTEXT("UnknownWeather", "That weather is not one of clear, cloudy, rain, snow or fog.");
					return false;
				}
				Command.Weather = Weather->Preset;
			}
			else if (Command.Intent == EStageAIIntent::SetWeather)
			{
				OutError = LOCTEXT("NoWeather", "The weather command did not say which weather.");
				return false;
			}
			if (Command.Intent == EStageAIIntent::SetTimeOfDay && !Hour.IsSet())
			{
				OutError = LOCTEXT("NoHour", "The time command did not say which time.");
				return false;
			}
			if (Command.Intent == EStageAIIntent::SetSky && !Input.HasField(TEXT("sky_on")))
			{
				OutError = LOCTEXT("NoSky", "The sky command did not say on or off.");
				return false;
			}

			FString ReportName;
			if (Input.TryGetStringField(TEXT("report"), ReportName))
			{
				const FReportName* Report = Algo::FindByPredicate(ReportNames, [&ReportName](const FReportName& Entry) { return ReportName == Entry.Name; });
				if (!Report)
				{
					OutError = LOCTEXT("UnknownReport", "That report is not one I can export.");
					return false;
				}
				Command.Report = Report->Report;
			}

			if (!Validate(Command, OutError))
			{
				return false;
			}
			OutCommand = Command;
			return true;
		}

		/** Plain-text turns for the API: user first, alternating, consecutive lines of one speaker joined. */
		TArray<TPair<EStageAISpeaker, FString>> MakeTurns(TConstArrayView<FStageAIMessage> History)
		{
			TArray<TPair<EStageAISpeaker, FString>> Turns;
			for (const FStageAIMessage& Message : History)
			{
				FString Text = Message.Text.ToString().TrimStartAndEnd();
				if (Text.IsEmpty() || (Turns.IsEmpty() && Message.Speaker != EStageAISpeaker::User))
				{
					continue;
				}
				if (!Turns.IsEmpty() && Turns.Last().Key == Message.Speaker)
				{
					Turns.Last().Value += TEXT("\n") + Text;
				}
				else
				{
					Turns.Emplace(Message.Speaker, MoveTemp(Text));
				}
			}
			// The new prompt is a user turn, so the history must end with the assistant.
			if (!Turns.IsEmpty() && Turns.Last().Key == EStageAISpeaker::User)
			{
				Turns.Pop();
			}
			return Turns;
		}
	}

	const TCHAR* GetToolName()
	{
		return TEXT("stage_command");
	}

	FString GetIntentName(EStageAIIntent Intent)
	{
		const FIntentName* Entry = Algo::FindByPredicate(IntentNames, [Intent](const FIntentName& Candidate) { return Candidate.Intent == Intent; });
		return Entry ? FString(Entry->Name) : FString();
	}

	bool Validate(FStageAICommand& Command, FText& OutError)
	{
		if (uint8(Command.Intent) >= uint8(EStageAIIntent::Count))
		{
			OutError = LOCTEXT("InvalidIntent", "That is not a command I can run.");
			return false;
		}
		Command.ItemQuery = TrimAndCap(Command.ItemQuery);
		Command.Label = Command.Label.TrimStartAndEnd().Left(StageCueRules::MaxLabelLength);

		switch (Command.Intent)
		{
		case EStageAIIntent::AddFixtures:
		{
			const int32 Count = Command.Count.Get(1);
			if (Count < 1 || Count > StageAICommandParser::MaxFixturesPerCommand)
			{
				OutError = FText::Format(LOCTEXT("FixtureCountRange", "I can add 1 to {0} fixtures at a time."), FText::AsNumber(StageAICommandParser::MaxFixturesPerCommand));
				return false;
			}
			Command.Count = Count;
			return true;
		}

		case EStageAIIntent::StoreCue:
			if (Command.CueNumber.IsSet() && !StageCueRules::IsValidCueNumber(*Command.CueNumber))
			{
				OutError = LOCTEXT("CueNumberRange", "Cue numbers go from 0.001 to 9999.999.");
				return false;
			}
			if (Command.FadeSeconds.IsSet() && (!FMath::IsFinite(*Command.FadeSeconds) || *Command.FadeSeconds < 0.f || *Command.FadeSeconds > StageCueRules::MaxFadeTime))
			{
				OutError = FText::Format(LOCTEXT("FadeRange", "Fade times go from 0 to {0} seconds."), FText::AsNumber(StageCueRules::MaxFadeTime));
				return false;
			}
			if (Command.CueNumber.IsSet())
			{
				Command.CueNumber = StageCueRules::RoundCueNumber(*Command.CueNumber);
			}
			return true;

		case EStageAIIntent::GoToCue:
			if (!Command.CueNumber.IsSet() || !StageCueRules::IsValidCueNumber(*Command.CueNumber))
			{
				OutError = LOCTEXT("GoToCueNumber", "Which cue? Cue numbers go from 0.001 to 9999.999.");
				return false;
			}
			Command.CueNumber = StageCueRules::RoundCueNumber(*Command.CueNumber);
			return true;

		case EStageAIIntent::SetWeather:
			if (Command.Weather == EStageWeatherPreset::Custom)
			{
				OutError = LOCTEXT("CustomWeather", "Pick clear, cloudy, rain, snow or fog.");
				return false;
			}
			return true;

		case EStageAIIntent::SetTimeOfDay:
			if (!FMath::IsFinite(Command.Hour) || Command.Hour < 0.f || Command.Hour > 24.f)
			{
				OutError = LOCTEXT("HourRange", "The time of day goes from 00:00 to 24:00.");
				return false;
			}
			Command.Hour = Command.Hour >= 24.f ? 0.f : Command.Hour;
			return true;

		case EStageAIIntent::CountMatching:
			if (Command.ItemQuery.IsEmpty())
			{
				OutError = LOCTEXT("CountWhat", "Count which items?");
				return false;
			}
			return true;

		case EStageAIIntent::Unsupported:
			if (!Algo::FindByPredicate(UnsupportedOperations, [&Command](const TCHAR* Operation) { return Command.ItemQuery == Operation; }))
			{
				Command.ItemQuery = UnsupportedOperations[0];
			}
			return true;

		default:
			return true;
		}
	}

	FString MakeToolDefinitionJson()
	{
		return Serialize(MakeToolDefinition());
	}

	bool CommandFromToolInputJson(const FString& InputJson, FStageAICommand& OutCommand, FText& OutError)
	{
		const TSharedPtr<FJsonObject> Input = Deserialize(InputJson);
		if (!Input.IsValid())
		{
			OutError = LOCTEXT("BadInput", "The command was not valid JSON.");
			return false;
		}
		return CommandFromToolInput(*Input, OutCommand, OutError);
	}

	FString GetSystemPrompt()
	{
		return TEXT(
			"You are the assistant built into StageCraft 3D, a tool for designing concert and event stages: truss, decks, lighting fixtures, "
			"audio gear, cues and the outdoor environment.\n"
			"Translate each request into exactly one call of the stage_command tool. Questions about the stage (counts, weight, power, cues, "
			"weather) are answered by the tool from the live stage, so call it rather than guessing numbers; the scene summary in the request "
			"is only context. Every change goes through the stage's rules and can be undone.\n"
			"If a request needs something the tool cannot do (deleting or clearing items, opening, saving or replacing scenes, several commands "
			"at once), do not call the tool: say briefly what to do instead, or which single step to start with. If a request is unclear, ask "
			"one short question. Keep text replies to one or two sentences.");
	}

	FString DescribeScene(const FStageAISceneView& Scene)
	{
		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("Placed items: %d%s. Total power: %.0f W%s. Total weight: %.0f kg%s."),
			Scene.PlacedItems, Scene.MaxPlacedItems > 0 ? *FString::Printf(TEXT(" (limit %d)"), Scene.MaxPlacedItems) : TEXT(""),
			Scene.TotalPowerWatts, Scene.MaxPowerWatts > 0.f ? *FString::Printf(TEXT(" (limit %.0f W)"), Scene.MaxPowerWatts) : TEXT(""),
			Scene.TotalWeightKg, Scene.MaxWeightKg > 0.f ? *FString::Printf(TEXT(" (limit %.0f kg)"), Scene.MaxWeightKg) : TEXT("")));
		TArray<FString> Fixtures;
		for (const TPair<FString, int32>& Entry : Scene.FixturesByName)
		{
			Fixtures.Add(FString::Printf(TEXT("%d x %s"), Entry.Value, *Entry.Key));
		}
		Lines.Add(FString::Printf(TEXT("Fixtures: %d%s%s."), Scene.GetFixtureCount(), Fixtures.IsEmpty() ? TEXT("") : TEXT(" - "), *FString::Join(Fixtures, TEXT(", "))));
		Lines.Add(FString::Printf(TEXT("Cues: %d%s."), Scene.Cues.Num(), Scene.ActiveCue > 0.f ? *FString::Printf(TEXT(", cue %g on stage"), Scene.ActiveCue) : TEXT("")));
		if (Scene.bHasEnvironment)
		{
			Lines.Add(FString::Printf(TEXT("Environment: %s, time %s, weather %s%s."), Scene.Environment.bSkyEnabled ? TEXT("outdoor sky on") : TEXT("indoor (sky off)"),
				*StageAIAnswers::FormatHour(Scene.Environment.TimeOfDay), *StageAIAnswers::DescribeWeather(StageEnvironment::FindWeatherPreset(Scene.Environment)).ToString(),
				Scene.bSkyAvailable ? TEXT("") : TEXT(", no sky system installed")));
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	FString BuildRequestBody(const FStageAIModelOptions& Options, TConstArrayView<FStageAIMessage> History, const FString& Prompt, const FString& SceneSummary)
	{
		TArray<TSharedPtr<FJsonValue>> Messages;
		const auto AddMessage = [&Messages](EStageAISpeaker Speaker, const FString& Text)
		{
			TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
			Message->SetStringField(TEXT("role"), Speaker == EStageAISpeaker::User ? TEXT("user") : TEXT("assistant"));
			Message->SetStringField(TEXT("content"), Text);
			Messages.Add(MakeShared<FJsonValueObject>(Message));
		};
		for (const TPair<EStageAISpeaker, FString>& Turn : MakeTurns(History))
		{
			AddMessage(Turn.Key, Turn.Value);
		}
		AddMessage(EStageAISpeaker::User, FString::Printf(TEXT("Current stage:\n%s\n\nRequest: %s"), *SceneSummary,
			*Prompt.Left(StageAICommandParser::MaxPromptLength)));

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Options.Model);
		Body->SetNumberField(TEXT("max_tokens"), Options.MaxTokens);
		Body->SetStringField(TEXT("system"), GetSystemPrompt());
		// Retries a safety decline on a fallback model inside the same call (beta server-side-fallback-2026-07-01).
		Body->SetStringField(TEXT("fallbacks"), TEXT("default"));
		TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
		OutputConfig->SetStringField(TEXT("effort"), Options.Effort);
		Body->SetObjectField(TEXT("output_config"), OutputConfig);
		Body->SetArrayField(TEXT("tools"), { MakeShared<FJsonValueObject>(MakeToolDefinition()) });
		// Forced tool choice is not supported on this model family: auto, at most one call.
		TSharedRef<FJsonObject> ToolChoice = MakeShared<FJsonObject>();
		ToolChoice->SetStringField(TEXT("type"), TEXT("auto"));
		ToolChoice->SetBoolField(TEXT("disable_parallel_tool_use"), true);
		Body->SetObjectField(TEXT("tool_choice"), ToolChoice);
		Body->SetArrayField(TEXT("messages"), Messages);
		return Serialize(Body);
	}

	FStageAIModelResponse ParseResponseBody(int32 HttpStatus, const FString& Body)
	{
		FStageAIModelResponse Response;
		const TSharedPtr<FJsonObject> Root = Deserialize(Body);
		FString ErrorMessage;
		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (Root.IsValid() && Root->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject)
		{
			(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
		}
		if (HttpStatus != 200 || !Root.IsValid() || ErrorObject)
		{
			Response.Error = FText::Format(LOCTEXT("HttpError", "The online model answered with HTTP {0}{1}."), FText::AsNumber(HttpStatus),
				ErrorMessage.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT(": ") + ErrorMessage.Left(200)));
			return Response;
		}

		FString StopReason;
		Root->TryGetStringField(TEXT("stop_reason"), StopReason);
		if (StopReason == TEXT("refusal"))
		{
			Response.Status = FStageAIModelResponse::EStatus::Refused;
			return Response;
		}

		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!Root->TryGetArrayField(TEXT("content"), Content) || !Content)
		{
			Response.Error = LOCTEXT("NoContent", "The online model's answer had no content.");
			return Response;
		}
		TArray<FString> Texts;
		for (const TSharedPtr<FJsonValue>& Value : *Content)
		{
			const TSharedPtr<FJsonObject> Block = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Type;
			if (!Block.IsValid() || !Block->TryGetStringField(TEXT("type"), Type))
			{
				continue;
			}
			FString Name;
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (Type == TEXT("tool_use") && Block->TryGetStringField(TEXT("name"), Name) && Name == GetToolName()
				&& Block->TryGetObjectField(TEXT("input"), Input) && Input)
			{
				if (CommandFromToolInput(**Input, Response.Command, Response.Error))
				{
					Response.Status = FStageAIModelResponse::EStatus::Command;
				}
				else
				{
					Response.bInvalidCommand = true;
				}
				return Response;
			}
			FString Text;
			if (Type == TEXT("text") && Block->TryGetStringField(TEXT("text"), Text) && !Text.TrimStartAndEnd().IsEmpty())
			{
				Texts.Add(Text.TrimStartAndEnd());
			}
		}
		if (Texts.IsEmpty())
		{
			Response.Error = StopReason == TEXT("max_tokens")
				? LOCTEXT("MaxTokens", "The online model ran out of room before answering.")
				: LOCTEXT("EmptyAnswer", "The online model gave no answer.");
			return Response;
		}
		Response.Status = FStageAIModelResponse::EStatus::Text;
		Response.Text = FString::Join(Texts, TEXT("\n"));
		return Response;
	}
}

#undef LOCTEXT_NAMESPACE
