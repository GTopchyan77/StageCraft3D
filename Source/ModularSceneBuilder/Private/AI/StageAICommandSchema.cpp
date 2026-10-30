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
			{ EStageAIIntent::DeleteItems, TEXT("delete_items") },
			{ EStageAIIntent::MoveItems, TEXT("move_items") },
			{ EStageAIIntent::ReplaceItems, TEXT("replace_items") },
			{ EStageAIIntent::BuildShape, TEXT("build_shape") },
			{ EStageAIIntent::SetFixture, TEXT("set_fixture") },
			{ EStageAIIntent::Unsupported, TEXT("unsupported") },
		};
		static_assert(UE_ARRAY_COUNT(IntentNames) == int32(EStageAIIntent::Count), "Every intent needs a wire name.");

		struct FTargetName { EStageAITarget Target; const TCHAR* Name; };
		const FTargetName TargetNames[] = { { EStageAITarget::Selection, TEXT("selection") }, { EStageAITarget::Named, TEXT("named") } };

		struct FDirectionName { EStageAIDirection Direction; const TCHAR* Name; };
		const FDirectionName DirectionNames[] = {
			{ EStageAIDirection::Left, TEXT("left") }, { EStageAIDirection::Right, TEXT("right") }, { EStageAIDirection::Forward, TEXT("forward") },
			{ EStageAIDirection::Back, TEXT("back") }, { EStageAIDirection::Up, TEXT("up") }, { EStageAIDirection::Down, TEXT("down") } };

		struct FControlName { EStageAIFixtureControl Control; const TCHAR* Name; };
		const FControlName ControlNames[] = {
			{ EStageAIFixtureControl::Dimmer, TEXT("dimmer") }, { EStageAIFixtureControl::Color, TEXT("color") }, { EStageAIFixtureControl::Gel, TEXT("gel") },
			{ EStageAIFixtureControl::Zoom, TEXT("zoom") }, { EStageAIFixtureControl::Frost, TEXT("frost") }, { EStageAIFixtureControl::Iris, TEXT("iris") },
			{ EStageAIFixtureControl::CTO, TEXT("cto") }, { EStageAIFixtureControl::Prism, TEXT("prism") }, { EStageAIFixtureControl::PrismSpin, TEXT("prism_spin") },
			{ EStageAIFixtureControl::Gobo, TEXT("gobo") }, { EStageAIFixtureControl::GoboSpin, TEXT("gobo_spin") }, { EStageAIFixtureControl::Strobe, TEXT("strobe") },
			{ EStageAIFixtureControl::Pan, TEXT("pan") }, { EStageAIFixtureControl::Tilt, TEXT("tilt") } };
		static_assert(UE_ARRAY_COUNT(ControlNames) == int32(EStageAIFixtureControl::Count), "Every fixture control needs a wire name.");

		struct FShapeName { EStageAIShape Shape; const TCHAR* Name; };
		const FShapeName ShapeNames[] = {
			{ EStageAIShape::RoundRiser, TEXT("round_riser") }, { EStageAIShape::ArcRiser, TEXT("arc_riser") }, { EStageAIShape::Stairs, TEXT("stairs") },
			{ EStageAIShape::ArcTruss, TEXT("arc_truss") } };

		/** Sane bounds of the shape sizes a command may ask for; the items' own ranges clamp further when they are built. */
		constexpr double MinShapeSizeCm = 10.0;
		constexpr double MaxShapeSizeCm = 5000.0;
		constexpr double MaxShapeHeightCm = 2000.0;
		constexpr double MinShapeAngle = 5.0;
		constexpr double MaxShapeAngle = 360.0;

		struct FWeatherName { EStageWeatherPreset Preset; const TCHAR* Name; };
		const FWeatherName WeatherNames[] = {
			{ EStageWeatherPreset::Clear, TEXT("clear") }, { EStageWeatherPreset::Cloudy, TEXT("cloudy") }, { EStageWeatherPreset::Rain, TEXT("rain") },
			{ EStageWeatherPreset::Snow, TEXT("snow") }, { EStageWeatherPreset::Fog, TEXT("fog") } };

		struct FReportName { EStageAIReport Report; const TCHAR* Name; };
		const FReportName ReportNames[] = {
			{ EStageAIReport::BillOfMaterials, TEXT("bill_of_materials") }, { EStageAIReport::PatchSheet, TEXT("patch_sheet") },
			{ EStageAIReport::CableSchedule, TEXT("cable_schedule") }, { EStageAIReport::All, TEXT("all") } };

		/** "delete" is clearing the whole stage; targeted deletes are delete_items. */
		const TCHAR* const UnsupportedOperations[] = { TEXT("delete"), TEXT("open"), TEXT("save") };

		template <typename TEntry>
		const TEntry* FindByName(TConstArrayView<TEntry> Entries, const FString& Name)
		{
			return Algo::FindByPredicate(Entries, [&Name](const TEntry& Entry) { return Name == Entry.Name; });
		}

		template <typename TEntry>
		TArray<const TCHAR*> NamesOf(TConstArrayView<TEntry> Entries)
		{
			TArray<const TCHAR*> Names;
			for (const TEntry& Entry : Entries)
			{
				Names.Add(Entry.Name);
			}
			return Names;
		}

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
			Properties->SetObjectField(TEXT("count"), MakeProperty(TEXT("integer"), FString::Printf(TEXT("add_fixtures: how many, 1 to %d. Default 1. delete_items, move_items, replace_items with target named: at most how many of the matching items (the latest placed first); omit for all of them."), StageAICommandParser::MaxFixturesPerCommand)));
			Properties->SetObjectField(TEXT("item"), MakeProperty(TEXT("string"), TEXT("add_fixtures: words naming the fixture type or catalog name (e.g. \"moving head spot\"); empty picks the first fixture. count_matching, and delete_items / move_items / replace_items with target named: words naming the placed items (\"spot\", \"truss\", \"lights\" for every fixture). unsupported: one of delete (clearing the whole stage), open, save.")));
			TSharedRef<FJsonObject> Target = MakeProperty(TEXT("string"), TEXT("delete_items, move_items, replace_items, set_fixture: selection for the user's selected items (\"this\", \"the selected\", \"it\"), named for the placed items matching item."));
			Target->SetArrayField(TEXT("enum"), StringValues(NamesOf(MakeArrayView(TargetNames))));
			Properties->SetObjectField(TEXT("target"), Target);
			Properties->SetObjectField(TEXT("replacement"), MakeProperty(TEXT("string"), TEXT("replace_items: words naming the catalog item to put in each target's place (e.g. \"wash\").")));
			TSharedRef<FJsonObject> Direction = MakeProperty(TEXT("string"), TEXT("move_items: left, right, forward or back as seen from the user's camera (snapped to the stage axes), or up / down."));
			Direction->SetArrayField(TEXT("enum"), StringValues(NamesOf(MakeArrayView(DirectionNames))));
			Properties->SetObjectField(TEXT("direction"), Direction);
			Properties->SetObjectField(TEXT("distance_cm"), MakeProperty(TEXT("number"), FString::Printf(TEXT("move_items: how far in cm, 1 to %.0f. Default 100."), StageAICommandParser::MaxMoveDistanceCm)));
			TSharedRef<FJsonObject> Shape = MakeProperty(TEXT("string"), TEXT("build_shape: round_riser (a round deck: radius_cm, height_cm), arc_riser (a curved deck ring: radius_cm to its outer edge, width_cm deep, angle_degrees, height_cm), stairs (height_cm = rise, width_cm, handrails), arc_truss (curved box truss: radius_cm, angle_degrees, 360 for a circle, height_cm = hang height)."));
			Shape->SetArrayField(TEXT("enum"), StringValues(NamesOf(MakeArrayView(ShapeNames))));
			Properties->SetObjectField(TEXT("shape"), Shape);
			Properties->SetObjectField(TEXT("radius_cm"), MakeProperty(TEXT("number"), TEXT("build_shape: radius in cm (half the diameter). Omit for the catalog item's default.")));
			Properties->SetObjectField(TEXT("width_cm"), MakeProperty(TEXT("number"), TEXT("build_shape: width in cm. Omit for the default.")));
			Properties->SetObjectField(TEXT("height_cm"), MakeProperty(TEXT("number"), TEXT("build_shape: height in cm (riser top, stair rise, or the truss's hang height). Omit for the default.")));
			Properties->SetObjectField(TEXT("angle_degrees"), MakeProperty(TEXT("number"), TEXT("build_shape arc_riser and arc_truss: the angle the arc spans, 5 to 360. Omit for the default.")));
			Properties->SetObjectField(TEXT("handrails"), MakeProperty(TEXT("boolean"), TEXT("build_shape stairs: handrails on both sides. Default true.")));
			{
				// set_fixture: the controls to set on the targeted fixtures (target / item / count as for move_items).
				TSharedRef<FJsonObject> SettingProperties = MakeShared<FJsonObject>();
				TSharedRef<FJsonObject> Control = MakeProperty(TEXT("string"), TEXT("Which control."));
				Control->SetArrayField(TEXT("enum"), StringValues(NamesOf(MakeArrayView(ControlNames))));
				SettingProperties->SetObjectField(TEXT("control"), Control);
				SettingProperties->SetObjectField(TEXT("value"), MakeProperty(TEXT("number"),
					TEXT("dimmer, frost, iris, cto: 0 to 1. zoom, pan, tilt: degrees. gobo_spin, prism_spin: rpm, signed. strobe: flashes per second, 0 = off. prism, gobo: the slot, 0 = out / open. Omit for color, gel, a named gobo, or zoom narrow / wide.")));
				SettingProperties->SetObjectField(TEXT("name"), MakeProperty(TEXT("string"),
					TEXT("color: a colour name (red, green, blue, white, warm white, cool white, amber, orange, yellow, cyan, magenta, pink, purple, lavender, uv, ...). gel: Lee or Rosco gel by number or name (\"lee 106\", \"rosco 27\", \"congo blue\"). gobo: a gobo name. zoom: narrow or wide.")));
				TSharedRef<FJsonObject> Setting = MakeShared<FJsonObject>();
				Setting->SetStringField(TEXT("type"), TEXT("object"));
				Setting->SetObjectField(TEXT("properties"), SettingProperties);
				Setting->SetArrayField(TEXT("required"), StringValues({ TEXT("control") }));
				Setting->SetBoolField(TEXT("additionalProperties"), false);
				TSharedRef<FJsonObject> Settings = MakeProperty(TEXT("array"), FString::Printf(TEXT("set_fixture: 1 to %d controls to set on the fixtures."), StageAICommandParser::MaxFixtureSettings));
				Settings->SetObjectField(TEXT("items"), Setting);
				Properties->SetObjectField(TEXT("fixture_settings"), Settings);
			}
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
			TSharedRef<FJsonObject> ReplyLanguage = MakeProperty(TEXT("string"),
				TEXT("The language of the user's request, which the stage's reply is written in: hy for Armenian (in Armenian script or in Latin letters), en otherwise."));
			ReplyLanguage->SetArrayField(TEXT("enum"), StringValues({ TEXT("en"), TEXT("hy") }));
			Properties->SetObjectField(TEXT("reply_language"), ReplyLanguage);

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

		bool CommandFromToolInput(const FJsonObject& Input, FStageAICommand& OutCommand, FText& OutError, EStageAILanguage DefaultLanguage)
		{
			static const TCHAR* const KnownFields[] = { TEXT("intent"), TEXT("count"), TEXT("item"), TEXT("cue_number"), TEXT("label"), TEXT("fade_seconds"),
				TEXT("weather"), TEXT("hour"), TEXT("sky_on"), TEXT("report"), TEXT("reply_language"), TEXT("target"), TEXT("replacement"), TEXT("direction"),
				TEXT("distance_cm"), TEXT("shape"), TEXT("radius_cm"), TEXT("width_cm"), TEXT("height_cm"), TEXT("angle_degrees"), TEXT("handrails"),
				TEXT("fixture_settings") };
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
			Command.Language = DefaultLanguage;
			FString LanguageCode;
			if (Input.TryGetStringField(TEXT("reply_language"), LanguageCode))
			{
				const TOptional<EStageAILanguage> Language = StageAILanguage::FromCode(LanguageCode);
				if (!Language.IsSet())
				{
					OutError = LOCTEXT("UnknownLanguage", "The command asked for a reply language other than English or Armenian.");
					return false;
				}
				Command.Language = *Language;
			}
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
			if (!ReadNumber(TEXT("count"), Count) || !ReadNumber(TEXT("cue_number"), CueNumber) || !ReadNumber(TEXT("fade_seconds"), Fade) || !ReadNumber(TEXT("hour"), Hour)
				|| !ReadNumber(TEXT("distance_cm"), Command.DistanceCm) || !ReadNumber(TEXT("radius_cm"), Command.RadiusCm) || !ReadNumber(TEXT("width_cm"), Command.WidthCm)
				|| !ReadNumber(TEXT("height_cm"), Command.HeightCm) || !ReadNumber(TEXT("angle_degrees"), Command.AngleDegrees))
			{
				return false;
			}
			if (Count.IsSet())
			{
				if (!FMath::IsFinite(*Count) || *Count != FMath::RoundToDouble(*Count))
				{
					OutError = LOCTEXT("CountWhole", "The count must be a whole number.");
					return false;
				}
				Command.Count = int32(FMath::Clamp(*Count, -1.0, 100000.0));
			}

			FString EnumName;
			if (Input.TryGetStringField(TEXT("target"), EnumName))
			{
				const FTargetName* Found = FindByName(MakeArrayView(TargetNames), EnumName);
				if (!Found)
				{
					OutError = LOCTEXT("UnknownTarget", "The target must be selection or named.");
					return false;
				}
				Command.Target = Found->Target;
			}
			if (Input.TryGetStringField(TEXT("direction"), EnumName))
			{
				const FDirectionName* Found = FindByName(MakeArrayView(DirectionNames), EnumName);
				if (!Found)
				{
					OutError = LOCTEXT("UnknownDirection", "The direction must be left, right, forward, back, up or down.");
					return false;
				}
				Command.Direction = Found->Direction;
			}
			else if (Command.Intent == EStageAIIntent::MoveItems)
			{
				OutError = LOCTEXT("NoDirection", "The move command did not say which way.");
				return false;
			}
			if (Input.TryGetStringField(TEXT("shape"), EnumName))
			{
				const FShapeName* Found = FindByName(MakeArrayView(ShapeNames), EnumName);
				if (!Found)
				{
					OutError = LOCTEXT("UnknownShape", "That shape is not one I can build.");
					return false;
				}
				Command.Shape = Found->Shape;
			}
			else if (Command.Intent == EStageAIIntent::BuildShape)
			{
				OutError = LOCTEXT("NoShape", "The build command did not say which shape.");
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Settings = nullptr;
			if (Input.TryGetArrayField(TEXT("fixture_settings"), Settings))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Settings)
				{
					const TSharedPtr<FJsonObject>* SettingObject = nullptr;
					FString ControlName;
					if (Command.FixtureSettings.Num() >= StageAICommandParser::MaxFixtureSettings || !Value.IsValid() || !Value->TryGetObject(SettingObject)
						|| !(*SettingObject)->TryGetStringField(TEXT("control"), ControlName))
					{
						OutError = LOCTEXT("BadSetting", "A fixture setting was not a control with a value.");
						return false;
					}
					const FControlName* Control = FindByName(MakeArrayView(ControlNames), ControlName);
					if (!Control)
					{
						OutError = FText::Format(LOCTEXT("UnknownControlName", "\"{0}\" is not a fixture control."), FText::FromString(ControlName.Left(32)));
						return false;
					}
					FStageAIFixtureSetting& Setting = Command.FixtureSettings.AddDefaulted_GetRef();
					Setting.Control = Control->Control;
					double Number = 0.0;
					if ((*SettingObject)->TryGetNumberField(TEXT("value"), Number))
					{
						Setting.Value = Number;
					}
					(*SettingObject)->TryGetStringField(TEXT("name"), Setting.Name);
					if (Setting.Control == EStageAIFixtureControl::Color)
					{
						// The model names the colour; the stage owns what that colour is.
						const TOptional<FLinearColor> Color = StageAICommandParser::FindColorName(Setting.Name);
						if (!Color.IsSet())
						{
							OutError = FText::Format(LOCTEXT("UnknownColorName", "\"{0}\" is not a colour I know."), FText::FromString(Setting.Name.Left(32)));
							return false;
						}
						Setting.Color = *Color;
					}
				}
			}
			else if (Command.Intent == EStageAIIntent::SetFixture)
			{
				OutError = LOCTEXT("NoSettings", "The fixture command did not say what to set.");
				return false;
			}
			Input.TryGetStringField(TEXT("replacement"), Command.Replacement);
			Input.TryGetBoolField(TEXT("handrails"), Command.bHandrails);
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
			// Items named without a target are the placed items of that name, not the selected ones.
			if (!Input.HasField(TEXT("target")) && !Command.ItemQuery.TrimStartAndEnd().IsEmpty())
			{
				Command.Target = EStageAITarget::Named;
			}
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
		using StageAILanguage::Pick;
		const EStageAILanguage Language = Command.Language == EStageAILanguage::Armenian ? EStageAILanguage::Armenian : EStageAILanguage::English;
		Command.Language = Language;
		if (uint8(Command.Intent) >= uint8(EStageAIIntent::Count))
		{
			OutError = Pick(Language, LOCTEXT("InvalidIntent", "That is not a command I can run."), INVTEXT("Դա այն հրամանը չէ, որը կարող եմ կատարել։"));
			return false;
		}
		Command.ItemQuery = TrimAndCap(Command.ItemQuery);
		Command.Label = Command.Label.TrimStartAndEnd().Left(StageCueRules::MaxLabelLength);
		const FText CueRange = Pick(Language, LOCTEXT("CueNumberRange", "Cue numbers go from 0.001 to 9999.999."), INVTEXT("Կյուների համարները 0.001-ից 9999.999 են։"));

		switch (Command.Intent)
		{
		case EStageAIIntent::AddFixtures:
		{
			const int32 Count = Command.Count.Get(1);
			if (Count < 1 || Count > StageAICommandParser::MaxFixturesPerCommand)
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("FixtureCountRange", "I can add 1 to {0} fixtures at a time."), INVTEXT("Միանգամից կարող եմ ավելացնել 1-ից {0} սարք։")),
					FText::AsNumber(StageAICommandParser::MaxFixturesPerCommand));
				return false;
			}
			Command.Count = Count;
			return true;
		}

		case EStageAIIntent::StoreCue:
			if (Command.CueNumber.IsSet() && !StageCueRules::IsValidCueNumber(*Command.CueNumber))
			{
				OutError = CueRange;
				return false;
			}
			if (Command.FadeSeconds.IsSet() && (!FMath::IsFinite(*Command.FadeSeconds) || *Command.FadeSeconds < 0.f || *Command.FadeSeconds > StageCueRules::MaxFadeTime))
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("FadeRange", "Fade times go from 0 to {0} seconds."), INVTEXT("Անցման ժամանակը 0-ից {0} վայրկյան է։")),
					FText::AsNumber(StageCueRules::MaxFadeTime));
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
				OutError = FText::Format(Pick(Language, LOCTEXT("GoToCueNumber", "Which cue? {0}"), INVTEXT("Ո՞ր կյուն։ {0}")), CueRange);
				return false;
			}
			Command.CueNumber = StageCueRules::RoundCueNumber(*Command.CueNumber);
			return true;

		case EStageAIIntent::SetWeather:
			if (Command.Weather == EStageWeatherPreset::Custom)
			{
				OutError = Pick(Language, LOCTEXT("CustomWeather", "Pick clear, cloudy, rain, snow or fog."), INVTEXT("Ընտրեք պարզ, ամպամած, անձրև, ձյուն կամ մառախուղ։"));
				return false;
			}
			return true;

		case EStageAIIntent::SetTimeOfDay:
			if (!FMath::IsFinite(Command.Hour) || Command.Hour < 0.f || Command.Hour > 24.f)
			{
				OutError = Pick(Language, LOCTEXT("HourRange", "The time of day goes from 00:00 to 24:00."), INVTEXT("Օրվա ժամը 00:00-ից 24:00 է։"));
				return false;
			}
			Command.Hour = Command.Hour >= 24.f ? 0.f : Command.Hour;
			return true;

		case EStageAIIntent::CountMatching:
			if (Command.ItemQuery.IsEmpty())
			{
				OutError = Pick(Language, LOCTEXT("CountWhat", "Count which items?"), INVTEXT("Ո՞ր իրերը հաշվեմ։"));
				return false;
			}
			return true;

		case EStageAIIntent::DeleteItems:
		case EStageAIIntent::MoveItems:
		case EStageAIIntent::ReplaceItems:
			if (Command.Target == EStageAITarget::Named && Command.ItemQuery.IsEmpty())
			{
				OutError = Pick(Language, LOCTEXT("EditWhich", "Which items? Select them first, or name them (\"the spots\")."),
					INVTEXT("Ո՞ր իրերը։ Նախ ընտրեք դրանք կամ նշեք անունը («սփոթերը»)։"));
				return false;
			}
			if (Command.Count.IsSet() && (*Command.Count < 1 || *Command.Count > StageAICommandParser::MaxEditTargets))
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("EditCountRange", "I can change 1 to {0} items at a time."), INVTEXT("Միանգամից կարող եմ փոխել 1-ից {0} իր։")),
					FText::AsNumber(StageAICommandParser::MaxEditTargets));
				return false;
			}
			if (Command.Intent == EStageAIIntent::MoveItems)
			{
				const double Distance = Command.DistanceCm.Get(StageAICommandParser::DefaultMoveDistanceCm);
				if (!FMath::IsFinite(Distance) || Distance < 1.0 || Distance > StageAICommandParser::MaxMoveDistanceCm)
				{
					OutError = FText::Format(Pick(Language, LOCTEXT("MoveRange", "I can move items 1 cm to {0} m at a time."), INVTEXT("Միանգամից կարող եմ տեղափոխել 1 սմ-ից {0} մ։")),
						FText::AsNumber(StageAICommandParser::MaxMoveDistanceCm / 100.0));
					return false;
				}
				Command.DistanceCm = Distance;
			}
			if (Command.Intent == EStageAIIntent::ReplaceItems)
			{
				Command.Replacement = TrimAndCap(Command.Replacement);
				if (Command.Replacement.IsEmpty())
				{
					OutError = Pick(Language, LOCTEXT("ReplaceWith", "Replace them with what? Name a catalog item (\"with washes\")."),
						INVTEXT("Ինչո՞վ փոխարինեմ։ Նշեք կատալոգի իրը («վոշերով»)։"));
					return false;
				}
			}
			return true;

		case EStageAIIntent::SetFixture:
		{
			if (Command.Target == EStageAITarget::Named && Command.ItemQuery.IsEmpty())
			{
				OutError = Pick(Language, LOCTEXT("SetWhich", "Which fixtures? Select them first, or name them (\"the washes\")."),
					INVTEXT("Ո՞ր սարքերը։ Նախ ընտրեք դրանք կամ նշեք անունը («վոշերը»)։"));
				return false;
			}
			if (Command.Count.IsSet() && (*Command.Count < 1 || *Command.Count > StageAICommandParser::MaxEditTargets))
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("SetCountRange", "I can change 1 to {0} fixtures at a time."), INVTEXT("Միանգամից կարող եմ փոխել 1-ից {0} սարք։")),
					FText::AsNumber(StageAICommandParser::MaxEditTargets));
				return false;
			}
			if (Command.FixtureSettings.IsEmpty() || Command.FixtureSettings.Num() > StageAICommandParser::MaxFixtureSettings)
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("SettingCount", "Tell me what to set (dimmer, colour, gel, zoom, frost, iris, CTO, prism, gobo, strobe, pan, tilt), at most {0} at once."),
					INVTEXT("Նշեք, թե ինչ փոխեմ (դիմեր, գույն, ժել, զում, ֆրոստ, իրիս, CTO, պրիզմա, գոբո, ստրոբ, պան, թիլթ), միանգամից առավելագույնը {0}։")),
					FText::AsNumber(StageAICommandParser::MaxFixtureSettings));
				return false;
			}
			for (FStageAIFixtureSetting& Setting : Command.FixtureSettings)
			{
				Setting.Name = TrimAndCap(Setting.Name);
				if (uint8(Setting.Control) >= uint8(EStageAIFixtureControl::Count))
				{
					OutError = Pick(Language, LOCTEXT("UnknownControl", "That is not a fixture control I know."), INVTEXT("Այդպիսի կառավարում չգիտեմ։"));
					return false;
				}
				// Unset values are allowed only where a name stands in for them.
				const bool bNamed = Setting.Control == EStageAIFixtureControl::Gel || Setting.Control == EStageAIFixtureControl::Color
					|| Setting.Control == EStageAIFixtureControl::Gobo || Setting.Control == EStageAIFixtureControl::Zoom;
				if (!Setting.Value.IsSet() && !(bNamed && !Setting.Name.IsEmpty()))
				{
					OutError = Pick(Language, LOCTEXT("SettingValue", "Give a value, e.g. \"dimmer 50%\", \"zoom 20 degrees\", \"gobo 3\"."),
						INVTEXT("Նշեք արժեքը, օրինակ՝ «դիմեր 50 տոկոս», «զում 20 աստիճան», «գոբո 3»։"));
					return false;
				}
				if (Setting.Control == EStageAIFixtureControl::Color
					&& !(FMath::IsFinite(Setting.Color.R) && FMath::IsFinite(Setting.Color.G) && FMath::IsFinite(Setting.Color.B)))
				{
					OutError = Pick(Language, LOCTEXT("BadColor", "That colour is not one I can mix."), INVTEXT("Այդ գույնը չեմ կարող խառնել։"));
					return false;
				}
				if (!Setting.Value.IsSet())
				{
					continue;
				}
				double Min = 0.0;
				double Max = 1.0;
				switch (Setting.Control)
				{
				case EStageAIFixtureControl::Zoom:		Min = 0.5; Max = 170.0; break;
				case EStageAIFixtureControl::Pan:
				case EStageAIFixtureControl::Tilt:		Min = -720.0; Max = 720.0; break;
				case EStageAIFixtureControl::GoboSpin:
				case EStageAIFixtureControl::PrismSpin:	Min = -300.0; Max = 300.0; break;
				case EStageAIFixtureControl::Strobe:	Max = 50.0; break;
				case EStageAIFixtureControl::Prism:
				case EStageAIFixtureControl::Gobo:		Max = 254.0; break;
				default: break;
				}
				const double Value = *Setting.Value;
				const bool bSlot = Setting.Control == EStageAIFixtureControl::Prism || Setting.Control == EStageAIFixtureControl::Gobo;
				const bool bWhole = !bSlot || Value == FMath::RoundToDouble(Value);
				if (!FMath::IsFinite(Value) || Value < Min || Value > Max || !bWhole)
				{
					FNumberFormattingOptions Format;
					Format.SetMaximumFractionalDigits(2);
					OutError = FText::Format(Pick(Language, LOCTEXT("SettingRange", "That value is outside {0} to {1}."), INVTEXT("Այդ արժեքը {0}-ից {1} միջակայքից դուրս է։")),
						FText::AsNumber(Min, &Format), FText::AsNumber(Max, &Format));
					return false;
				}
			}
			return true;
		}

		case EStageAIIntent::BuildShape:
		{
			const auto InRange = [](const TOptional<double>& Value, double Min, double Max)
			{
				return !Value.IsSet() || (FMath::IsFinite(*Value) && *Value >= Min && *Value <= Max);
			};
			if (!InRange(Command.RadiusCm, MinShapeSizeCm, MaxShapeSizeCm) || !InRange(Command.WidthCm, MinShapeSizeCm, MaxShapeSizeCm)
				|| !InRange(Command.HeightCm, 0.0, MaxShapeHeightCm))
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("ShapeSizeRange", "Shape sizes go from {0} cm to {1} m, and heights up to {2} m."),
						INVTEXT("Ձևերի չափերը {0} սմ-ից {1} մ են, իսկ բարձրությունը՝ մինչև {2} մ։")),
					FText::AsNumber(MinShapeSizeCm), FText::AsNumber(MaxShapeSizeCm / 100.0), FText::AsNumber(MaxShapeHeightCm / 100.0));
				return false;
			}
			if (!InRange(Command.AngleDegrees, MinShapeAngle, MaxShapeAngle))
			{
				OutError = FText::Format(Pick(Language, LOCTEXT("ShapeAngleRange", "Arcs span {0} to {1} degrees."), INVTEXT("Աղեղները {0}-ից {1} աստիճան են։")),
					FText::AsNumber(MinShapeAngle), FText::AsNumber(MaxShapeAngle));
				return false;
			}
			return true;
		}

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

	bool CommandFromToolInputJson(const FString& InputJson, FStageAICommand& OutCommand, FText& OutError, EStageAILanguage DefaultLanguage)
	{
		const TSharedPtr<FJsonObject> Input = Deserialize(InputJson);
		if (!Input.IsValid())
		{
			OutError = LOCTEXT("BadInput", "The command was not valid JSON.");
			return false;
		}
		return CommandFromToolInput(*Input, OutCommand, OutError, DefaultLanguage);
	}

	FString GetSystemPrompt()
	{
		return TEXT(
			"You are the assistant built into StageCraft 3D, a tool for designing concert and event stages: truss, decks, lighting fixtures, "
			"audio gear, cues and the outdoor environment.\n"
			"Translate each request into exactly one call of the stage_command tool. Questions about the stage (counts, weight, power, cues, "
			"weather) are answered by the tool from the live stage, so call it rather than guessing numbers; the scene summary in the request "
			"is only context. Every change goes through the stage's rules and can be undone.\n"
			"Placed items can be deleted, moved and replaced: use target selection for what the user has selected (\"this\", \"it\", \"the "
			"selected spots\", with item to narrow it), or target named with item for placed items by name (\"the pars\"). Round and curved "
			"risers, stairs and curved or circular truss are built with build_shape in centimetres and degrees; a size given without a role "
			"is a round riser's or a truss circle's diameter.\n"
			"Lighting fixtures are adjusted with set_fixture (same target rules): dimmer, colour by name, a Lee or Rosco gel, zoom, frost, "
			"iris, CTO, prism, gobo, gobo or prism spin, strobe, pan and tilt, several in one call (\"make the washes red at 50%\").\n"
			"If a request needs something the tool cannot do (clearing the whole stage, opening, saving or replacing scenes, several commands "
			"at once), do not call the tool: say briefly what to do instead, or which single step to start with. If a request is unclear, ask "
			"one short question. Keep text replies to one or two sentences.\n"
			"Users write in English or Armenian, in Armenian script or in Latin letters, and voice prompts arrive as transcripts that may "
			"contain recognition errors. Set reply_language to hy for Armenian and en otherwise, and write any text reply in the user's "
			"language. Always fill the item field in English with the catalog's words (for example \"moving head\", \"wash\", \"par\", "
			"\"truss\"), translating Armenian equipment names such as շարժական գլուխ (moving head) or ֆերմա (truss).");
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
		// The selection, counted per name, so "this" and "the selected" can be resolved.
		TArray<TPair<FString, int32>> Selected;
		for (const FString& Name : Scene.SelectedItemNames)
		{
			if (TPair<FString, int32>* Entry = Selected.FindByPredicate([&Name](const TPair<FString, int32>& Existing) { return Existing.Key == Name; }))
			{
				++Entry->Value;
			}
			else
			{
				Selected.Emplace(Name, 1);
			}
		}
		TArray<FString> SelectedParts;
		for (const TPair<FString, int32>& Entry : Selected)
		{
			SelectedParts.Add(FString::Printf(TEXT("%d x %s"), Entry.Value, *Entry.Key));
		}
		Lines.Add(FString::Printf(TEXT("Selected: %d%s%s."), Scene.SelectedItemNames.Num(), SelectedParts.IsEmpty() ? TEXT("") : TEXT(" - "), *FString::Join(SelectedParts, TEXT(", "))));
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

	FStageAIModelResponse ParseResponseBody(int32 HttpStatus, const FString& Body, EStageAILanguage PromptLanguage)
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
				if (CommandFromToolInput(**Input, Response.Command, Response.Error, PromptLanguage))
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
