// Copyright Epic Games, Inc. All Rights Reserved.

#include "Vision/StageVisionBom.h"

#include "Data/BaseItemData.h"
#include "Data/StageItemTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "StageVisionBom"

bool FStageVisionBomLine::NeedsReview() const
{
	return Confidence < StageVisionBom::NeedsReviewBelow;
}

namespace StageVisionBom
{
	namespace
	{
		struct FKindInfo
		{
			EStageVisionBomKind Kind;
			const TCHAR* Name;
			/** Preferred word in a catalog item's name, among the items of the kind's category; null for any. */
			const TCHAR* Keyword;
			/** Only an item whose name has the keyword matches: the category alone would pick the wrong thing. */
			bool bKeywordRequired;
		};

		/** Wire names in kind order; the order is the enum's. */
		constexpr FKindInfo Kinds[] = {
			{ EStageVisionBomKind::TrussStraight, TEXT("truss_straight"), nullptr, false },
			{ EStageVisionBomKind::TrussCorner, TEXT("truss_corner"), nullptr, false },
			{ EStageVisionBomKind::TrussTower, TEXT("truss_tower"), nullptr, false },
			{ EStageVisionBomKind::GroundSupport, TEXT("ground_support"), nullptr, false },
			{ EStageVisionBomKind::ChainHoist, TEXT("chain_hoist"), nullptr, false },
			{ EStageVisionBomKind::Deck, TEXT("deck"), nullptr, false },
			{ EStageVisionBomKind::LedWall, TEXT("led_wall"), TEXT("LED"), true },
			{ EStageVisionBomKind::MovingHeadSpot, TEXT("moving_head_spot"), TEXT("Spot"), false },
			{ EStageVisionBomKind::MovingHeadWash, TEXT("moving_head_wash"), TEXT("Wash"), false },
			{ EStageVisionBomKind::MovingHeadBeam, TEXT("moving_head_beam"), TEXT("Beam"), false },
			{ EStageVisionBomKind::ParWash, TEXT("par_wash"), TEXT("Par"), false },
			{ EStageVisionBomKind::Strobe, TEXT("strobe"), TEXT("Strobe"), false },
			{ EStageVisionBomKind::Blinder, TEXT("blinder"), TEXT("Blinder"), false },
			{ EStageVisionBomKind::Followspot, TEXT("followspot"), TEXT("Follow"), true },
			{ EStageVisionBomKind::Laser, TEXT("laser"), nullptr, false },
			{ EStageVisionBomKind::LineArray, TEXT("line_array"), nullptr, false },
			{ EStageVisionBomKind::Subwoofer, TEXT("subwoofer"), nullptr, false },
			{ EStageVisionBomKind::SpeakerStack, TEXT("speaker_stack"), TEXT("Speaker"), true },
			{ EStageVisionBomKind::MonitorWedge, TEXT("monitor_wedge"), nullptr, false },
			{ EStageVisionBomKind::MixingDesk, TEXT("mixing_desk"), TEXT("Sound"), true },
			{ EStageVisionBomKind::LightingDesk, TEXT("lighting_desk"), TEXT("Lighting"), true },
			{ EStageVisionBomKind::Barricade, TEXT("barricade"), TEXT("Barri"), true },
			{ EStageVisionBomKind::HazeMachine, TEXT("haze_machine"), TEXT("Haze"), true },
			{ EStageVisionBomKind::Other, TEXT("other"), nullptr, false },
		};
		static_assert(UE_ARRAY_COUNT(Kinds) == int32(EStageVisionBomKind::Count), "Every kind needs a wire name");

		const TCHAR* const SideNames[] = { TEXT("full_width"), TEXT("stage_left"), TEXT("centre"), TEXT("stage_right") };
		const TCHAR* const DepthNames[] = { TEXT("midstage"), TEXT("upstage"), TEXT("downstage"), TEXT("front_of_house") };
		const TCHAR* const LevelNames[] = { TEXT("floor"), TEXT("deck"), TEXT("flown") };
		static_assert(UE_ARRAY_COUNT(SideNames) == int32(EStageVisionBomSide::Count), "Every side needs a wire name");
		static_assert(UE_ARRAY_COUNT(DepthNames) == int32(EStageVisionBomDepth::Count), "Every depth needs a wire name");
		static_assert(UE_ARRAY_COUNT(LevelNames) == int32(EStageVisionBomLevel::Count), "Every level needs a wire name");

		template <typename EnumType, int32 N>
		EnumType ParseName(const TCHAR* const (&Names)[N], const FString& Name, EnumType Default)
		{
			for (int32 Index = 0; Index < N; ++Index)
			{
				if (Name.Equals(Names[Index], ESearchCase::CaseSensitive))
				{
					return EnumType(Index);
				}
			}
			return Default;
		}

		TSharedRef<FJsonObject> MakeTyped(const TCHAR* Type, const FString& Description)
		{
			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), Type);
			Property->SetStringField(TEXT("description"), Description);
			return Property;
		}

		template <int32 N>
		TSharedRef<FJsonObject> MakeEnum(const TCHAR* const (&Names)[N], const FString& Description)
		{
			TSharedRef<FJsonObject> Property = MakeTyped(TEXT("string"), Description);
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TCHAR* Name : Names)
			{
				Values.Add(MakeShared<FJsonValueString>(Name));
			}
			Property->SetArrayField(TEXT("enum"), Values);
			return Property;
		}

		TSharedRef<FJsonObject> MakeObjectSchema(const TSharedRef<FJsonObject>& Properties, std::initializer_list<const TCHAR*> Required)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> RequiredValues;
			for (const TCHAR* Name : Required)
			{
				RequiredValues.Add(MakeShared<FJsonValueString>(Name));
			}
			Schema->SetArrayField(TEXT("required"), RequiredValues);
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			return Schema;
		}

		TSharedRef<FJsonObject> MakeToolDefinition()
		{
			TSharedRef<FJsonObject> KindProperty = MakeTyped(TEXT("string"), TEXT("What the equipment is."));
			TArray<TSharedPtr<FJsonValue>> KindValues;
			for (const FKindInfo& Info : Kinds)
			{
				KindValues.Add(MakeShared<FJsonValueString>(Info.Name));
			}
			KindProperty->SetArrayField(TEXT("enum"), KindValues);

			TSharedRef<FJsonObject> ItemProperties = MakeShared<FJsonObject>();
			ItemProperties->SetObjectField(TEXT("kind"), KindProperty);
			ItemProperties->SetObjectField(TEXT("quantity"), MakeTyped(TEXT("integer"),
				TEXT("How many you can count. For truss_straight: the number of separate runs. For deck and led_wall: separate surfaces.")));
			ItemProperties->SetObjectField(TEXT("confidence"), MakeTyped(TEXT("number"), TEXT("0 to 1: how sure you are of the kind and the count.")));
			ItemProperties->SetObjectField(TEXT("length_m"), MakeTyped(TEXT("number"), TEXT("Metres, only when you can estimate it: a truss run's length, a deck's depth.")));
			ItemProperties->SetObjectField(TEXT("width_m"), MakeTyped(TEXT("number"), TEXT("Metres, only when you can estimate it: a deck's or LED wall's width.")));
			ItemProperties->SetObjectField(TEXT("height_m"), MakeTyped(TEXT("number"), TEXT("Metres, only when you can estimate it: an LED wall's or tower's height.")));
			ItemProperties->SetObjectField(TEXT("side"), MakeEnum(SideNames, TEXT("Where across the stage, stage left being the performers' left.")));
			ItemProperties->SetObjectField(TEXT("depth"), MakeEnum(DepthNames, TEXT("Where along the stage; front_of_house is out in the audience.")));
			ItemProperties->SetObjectField(TEXT("level"), MakeEnum(LevelNames, TEXT("On the floor, on the stage deck, or flown (hung).")));
			ItemProperties->SetObjectField(TEXT("notes"), MakeTyped(TEXT("string"), TEXT("A few words: a visible make or model, a colour, why you are unsure.")));

			TSharedRef<FJsonObject> Items = MakeTyped(TEXT("array"), TEXT("One entry per kind and position. Count only what you can see."));
			Items->SetObjectField(TEXT("items"), MakeObjectSchema(ItemProperties,
				{ TEXT("kind"), TEXT("quantity"), TEXT("confidence"), TEXT("side"), TEXT("depth"), TEXT("level") }));

			TSharedRef<FJsonObject> StageProperties = MakeShared<FJsonObject>();
			StageProperties->SetObjectField(TEXT("width_m"), MakeTyped(TEXT("number"), TEXT("The performance area's width.")));
			StageProperties->SetObjectField(TEXT("depth_m"), MakeTyped(TEXT("number"), TEXT("The performance area's depth.")));
			StageProperties->SetObjectField(TEXT("deck_height_m"), MakeTyped(TEXT("number"), TEXT("The deck's height above the audience floor.")));

			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("items"), Items);
			Properties->SetObjectField(TEXT("stage"), MakeObjectSchema(StageProperties, {}));
			Properties->SetObjectField(TEXT("notes"), MakeTyped(TEXT("string"), TEXT("One short sentence on what could not be seen or counted.")));

			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), GetToolName());
			Tool->SetStringField(TEXT("description"), TEXT("Report the stage equipment visible in the photo with counts and positions. Call it once."));
			Tool->SetBoolField(TEXT("strict"), true);
			Tool->SetObjectField(TEXT("input_schema"), MakeObjectSchema(Properties, { TEXT("items") }));
			return Tool;
		}

		FString Serialize(const TSharedRef<FJsonObject>& Object)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
			return Out;
		}

		/** A dimension in metres: finite, above 0 and at most MaxDimensionM, else 0 (not given). */
		double ReadDimension(const FJsonObject& Object, const TCHAR* Field)
		{
			double Value = 0.0;
			return Object.TryGetNumberField(Field, Value) && FMath::IsFinite(Value) && Value > 0.0 && Value <= MaxDimensionM ? Value : 0.0;
		}

		/** Text the model wrote, for display: control characters removed, trimmed, capped. */
		FString CleanText(const FString& Text, int32 MaxLength)
		{
			FString Clean;
			Clean.Reserve(FMath::Min(Text.Len(), MaxLength));
			for (const TCHAR Character : Text)
			{
				Clean.AppendChar(FChar::IsControl(Character) ? TEXT(' ') : Character);
			}
			return Clean.TrimStartAndEnd().Left(MaxLength);
		}

		/** One validated line, or unset (counted as dropped by the caller) when its kind or quantity is unusable. */
		TOptional<FStageVisionBomLine> ReadLine(const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
			const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->TryGetObject(ObjectPtr) && ObjectPtr ? *ObjectPtr : nullptr;
			FString KindName;
			double Quantity = 0.0;
			if (!Object.IsValid() || !Object->TryGetStringField(TEXT("kind"), KindName) || !Object->TryGetNumberField(TEXT("quantity"), Quantity)
				|| !FMath::IsFinite(Quantity) || Quantity < 1.0)
			{
				return {};
			}
			const TOptional<EStageVisionBomKind> Kind = ParseKind(KindName);
			if (!Kind.IsSet())
			{
				return {};
			}
			FStageVisionBomLine Line;
			Line.Kind = *Kind;
			Line.Quantity = int32(FMath::Min(FMath::RoundToDouble(Quantity), double(MaxQuantity)));
			double Confidence = 0.0;
			Line.Confidence = Object->TryGetNumberField(TEXT("confidence"), Confidence) && FMath::IsFinite(Confidence) ? float(FMath::Clamp(Confidence, 0.0, 1.0)) : 0.f;
			Line.LengthM = ReadDimension(*Object, TEXT("length_m"));
			Line.WidthM = ReadDimension(*Object, TEXT("width_m"));
			Line.HeightM = ReadDimension(*Object, TEXT("height_m"));
			FString Name;
			Line.Side = Object->TryGetStringField(TEXT("side"), Name) ? ParseName(SideNames, Name, EStageVisionBomSide::Centre) : EStageVisionBomSide::Centre;
			Line.Depth = Object->TryGetStringField(TEXT("depth"), Name) ? ParseName(DepthNames, Name, EStageVisionBomDepth::Midstage) : EStageVisionBomDepth::Midstage;
			Line.Level = Object->TryGetStringField(TEXT("level"), Name) ? ParseName(LevelNames, Name, EStageVisionBomLevel::Floor) : EStageVisionBomLevel::Floor;
			FString Notes;
			Object->TryGetStringField(TEXT("notes"), Notes);
			Line.Notes = CleanText(Notes, MaxNotesLength);
			return Line;
		}

		/** The inventory in the tool's input. False only when the input is not shaped like the tool. */
		bool ReadInventory(const FJsonObject& Input, FStageVisionBomResult& Result)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Input.Values)
			{
				if (Field.Key != TEXT("items") && Field.Key != TEXT("stage") && Field.Key != TEXT("notes"))
				{
					Result.Error = FText::Format(LOCTEXT("UnknownField", "The reading had an unknown field \"{0}\"."), FText::FromString(Field.Key.Left(32)));
					return false;
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!Input.TryGetArrayField(TEXT("items"), Items) || !Items)
			{
				Result.Error = LOCTEXT("NoItems", "The reading had no equipment list.");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Items)
			{
				TOptional<FStageVisionBomLine> Line = Result.Lines.Num() < MaxLines ? ReadLine(Value) : TOptional<FStageVisionBomLine>();
				if (Line.IsSet())
				{
					Result.Lines.Add(MoveTemp(*Line));
				}
				else
				{
					++Result.Dropped;
				}
			}
			const TSharedPtr<FJsonObject>* Stage = nullptr;
			if (Input.TryGetObjectField(TEXT("stage"), Stage) && Stage)
			{
				Result.Stage.WidthM = ReadDimension(**Stage, TEXT("width_m"));
				Result.Stage.DepthM = ReadDimension(**Stage, TEXT("depth_m"));
				Result.Stage.DeckHeightM = ReadDimension(**Stage, TEXT("deck_height_m"));
			}
			FString Notes;
			Input.TryGetStringField(TEXT("notes"), Notes);
			Result.Text = CleanText(Notes, 300);
			return true;
		}

		FText FormatMetres(double Metres)
		{
			FNumberFormattingOptions Options;
			Options.MaximumFractionalDigits = 1;
			return FText::AsNumber(Metres, &Options);
		}

		/** Kinds the build lays out as deck or truss strokes, or does not build at all (spans drawn with their tool). */
		bool IsSpanKind(EStageVisionBomKind Kind)
		{
			return Kind == EStageVisionBomKind::TrussStraight || Kind == EStageVisionBomKind::TrussCorner || Kind == EStageVisionBomKind::Deck
				|| Kind == EStageVisionBomKind::Barricade;
		}

		bool IsLightingKind(EStageVisionBomKind Kind)
		{
			switch (Kind)
			{
			case EStageVisionBomKind::MovingHeadSpot:
			case EStageVisionBomKind::MovingHeadWash:
			case EStageVisionBomKind::MovingHeadBeam:
			case EStageVisionBomKind::ParWash:
			case EStageVisionBomKind::Strobe:
			case EStageVisionBomKind::Blinder:
			case EStageVisionBomKind::Laser:
				return true;
			default:
				return false;
			}
		}

		/** Closest spacing of a kind's items in a row, in cm. */
		double GetSpacingCm(EStageVisionBomKind Kind)
		{
			switch (Kind)
			{
			case EStageVisionBomKind::MixingDesk:
			case EStageVisionBomKind::LightingDesk:
				return 300.0;
			case EStageVisionBomKind::LineArray:
			case EStageVisionBomKind::SpeakerStack:
			case EStageVisionBomKind::Subwoofer:
			case EStageVisionBomKind::MonitorWedge:
			case EStageVisionBomKind::Followspot:
				return 120.0;
			case EStageVisionBomKind::LedWall:
				return 850.0;
			default:
				return 60.0;
			}
		}

		/** Desks look at the stage and wedges at the performers (+X); everything else faces the audience (-X). */
		double GetYaw(EStageVisionBomKind Kind)
		{
			return Kind == EStageVisionBomKind::MixingDesk || Kind == EStageVisionBomKind::LightingDesk || Kind == EStageVisionBomKind::MonitorWedge
				? 0.0
				: 180.0;
		}

		struct FTrussRun
		{
			double X = 0.0;
			double HalfLength = 0.0;
		};
	}

	const TCHAR* GetToolName()
	{
		return TEXT("stage_inventory");
	}

	FString MakeToolDefinitionJson()
	{
		return Serialize(MakeToolDefinition());
	}

	FString GetSystemPrompt()
	{
		return TEXT(
			"You inspect photos of real concert and event stages for StageCraft 3D, a stage designer, and list the production equipment "
			"needed to build the same stage, with one stage_inventory call.\n"
			"- Count what you can see: truss runs, corners and towers, ground support, chain hoists, stage decks, LED walls, moving heads "
			"(spot, wash, beam), PARs and washes, strobes, blinders, followspots, lasers, line arrays, subwoofers, speaker stacks, monitor "
			"wedges, mixing and lighting desks, barricade, haze machines. Use other for equipment outside these kinds.\n"
			"- One entry per kind and position. When a row of identical fixtures hangs on one truss, that is one entry with its count.\n"
			"- A symmetric rig is usually symmetric: count one side carefully and check the other.\n"
			"- Give lengths, widths and heights in metres only when the photo lets you estimate them (people are about 1.75 m tall, a "
			"standard truss is 0.3 to 0.5 m deep).\n"
			"- confidence is your honest certainty of the kind and the count; lower it for anything hidden, dark, blurred or guessed.\n"
			"- Stage left is the performers' left (the audience's right).\n"
			"Do not invent equipment you cannot see, do not give prices or brands you cannot read. If the image is not a stage, do not "
			"call the tool; say so in one sentence.");
	}

	FString BuildRequestBody(const FStageAIModelOptions& Options, const FString& JpegBase64)
	{
		TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("type"), TEXT("base64"));
		Source->SetStringField(TEXT("media_type"), TEXT("image/jpeg"));
		Source->SetStringField(TEXT("data"), JpegBase64);
		TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
		Image->SetStringField(TEXT("type"), TEXT("image"));
		Image->SetObjectField(TEXT("source"), Source);
		TSharedRef<FJsonObject> Text = MakeShared<FJsonObject>();
		Text->SetStringField(TEXT("type"), TEXT("text"));
		Text->SetStringField(TEXT("text"), TEXT("List the equipment needed to build this stage."));
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), TEXT("user"));
		Message->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(Image), MakeShared<FJsonValueObject>(Text) });

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Options.Model);
		Body->SetNumberField(TEXT("max_tokens"), FMath::Max(Options.MaxTokens, 4096));
		Body->SetStringField(TEXT("system"), GetSystemPrompt());
		Body->SetStringField(TEXT("fallbacks"), TEXT("default"));
		// Counting equipment in a photo needs care: never below high.
		TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
		OutputConfig->SetStringField(TEXT("effort"), Options.Effort == TEXT("max") || Options.Effort == TEXT("xhigh") ? Options.Effort : FString(TEXT("high")));
		Body->SetObjectField(TEXT("output_config"), OutputConfig);
		Body->SetArrayField(TEXT("tools"), { MakeShared<FJsonValueObject>(MakeToolDefinition()) });
		TSharedRef<FJsonObject> ToolChoice = MakeShared<FJsonObject>();
		ToolChoice->SetStringField(TEXT("type"), TEXT("auto"));
		ToolChoice->SetBoolField(TEXT("disable_parallel_tool_use"), true);
		Body->SetObjectField(TEXT("tool_choice"), ToolChoice);
		Body->SetArrayField(TEXT("messages"), { MakeShared<FJsonValueObject>(Message) });
		return Serialize(Body);
	}

	FStageVisionBomResult ParseResponseBody(int32 HttpStatus, const FString& Body)
	{
		FStageVisionBomResult Result;
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
			Result.Error = FText::Format(LOCTEXT("HttpError", "The online model answered with HTTP {0}{1}."), FText::AsNumber(HttpStatus),
				ErrorMessage.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT(": ") + CleanText(ErrorMessage, 200)));
			return Result;
		}
		FString StopReason;
		Root->TryGetStringField(TEXT("stop_reason"), StopReason);
		if (StopReason == TEXT("refusal"))
		{
			Result.Status = FStageVisionBomResult::EStatus::Refused;
			Result.Error = LOCTEXT("Declined", "The online model declined to read the photo.");
			return Result;
		}
		const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
		if (!Root->TryGetArrayField(TEXT("content"), Content) || !Content)
		{
			Result.Error = LOCTEXT("NoContent", "The online model's answer had no content.");
			return Result;
		}
		TArray<FString> Texts;
		for (const TSharedPtr<FJsonValue>& Value : *Content)
		{
			const TSharedPtr<FJsonObject>* BlockPtr = nullptr;
			const TSharedPtr<FJsonObject> Block = Value.IsValid() && Value->TryGetObject(BlockPtr) && BlockPtr ? *BlockPtr : nullptr;
			FString Type;
			if (!Block.IsValid() || !Block->TryGetStringField(TEXT("type"), Type))
			{
				continue;
			}
			FString Name;
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (Type.Equals(TEXT("tool_use"), ESearchCase::CaseSensitive) && Block->TryGetStringField(TEXT("name"), Name) && Name.Equals(GetToolName(), ESearchCase::CaseSensitive) && Block->TryGetObjectField(TEXT("input"), Input) && Input)
			{
				if (ReadInventory(**Input, Result))
				{
					Result.Status = FStageVisionBomResult::EStatus::Inventory;
				}
				return Result;
			}
			FString Text;
			if (Type == TEXT("text") && Block->TryGetStringField(TEXT("text"), Text) && !Text.TrimStartAndEnd().IsEmpty())
			{
				Texts.Add(Text.TrimStartAndEnd());
			}
		}
		if (Texts.IsEmpty())
		{
			Result.Error = StopReason == TEXT("max_tokens")
				? LOCTEXT("MaxTokens", "The online model ran out of room before answering.")
				: LOCTEXT("EmptyAnswer", "The online model gave no reading.");
			return Result;
		}
		Result.Status = FStageVisionBomResult::EStatus::Text;
		Result.Text = CleanText(FString::Join(Texts, TEXT(" ")), 300);
		return Result;
	}

	const TCHAR* GetKindName(EStageVisionBomKind Kind)
	{
		return Kind < EStageVisionBomKind::Count ? Kinds[int32(Kind)].Name : TEXT("other");
	}

	TOptional<EStageVisionBomKind> ParseKind(const FString& Name)
	{
		for (const FKindInfo& Info : Kinds)
		{
			if (Name.Equals(Info.Name, ESearchCase::CaseSensitive))
			{
				return Info.Kind;
			}
		}
		return {};
	}

	FText GetKindDisplayName(EStageVisionBomKind Kind)
	{
		switch (Kind)
		{
		case EStageVisionBomKind::TrussStraight: return LOCTEXT("TrussStraight", "Truss, straight run");
		case EStageVisionBomKind::TrussCorner: return LOCTEXT("TrussCorner", "Truss corner");
		case EStageVisionBomKind::TrussTower: return LOCTEXT("TrussTower", "Truss tower");
		case EStageVisionBomKind::GroundSupport: return LOCTEXT("GroundSupport", "Ground support");
		case EStageVisionBomKind::ChainHoist: return LOCTEXT("ChainHoist", "Chain hoist");
		case EStageVisionBomKind::Deck: return LOCTEXT("Deck", "Stage deck");
		case EStageVisionBomKind::LedWall: return LOCTEXT("LedWall", "LED wall");
		case EStageVisionBomKind::MovingHeadSpot: return LOCTEXT("MovingHeadSpot", "Moving head, spot");
		case EStageVisionBomKind::MovingHeadWash: return LOCTEXT("MovingHeadWash", "Moving head, wash");
		case EStageVisionBomKind::MovingHeadBeam: return LOCTEXT("MovingHeadBeam", "Moving head, beam");
		case EStageVisionBomKind::ParWash: return LOCTEXT("ParWash", "PAR / wash");
		case EStageVisionBomKind::Strobe: return LOCTEXT("Strobe", "Strobe");
		case EStageVisionBomKind::Blinder: return LOCTEXT("Blinder", "Blinder");
		case EStageVisionBomKind::Followspot: return LOCTEXT("Followspot", "Followspot");
		case EStageVisionBomKind::Laser: return LOCTEXT("Laser", "Laser");
		case EStageVisionBomKind::LineArray: return LOCTEXT("LineArray", "Line array box");
		case EStageVisionBomKind::Subwoofer: return LOCTEXT("Subwoofer", "Subwoofer");
		case EStageVisionBomKind::SpeakerStack: return LOCTEXT("SpeakerStack", "Speaker stack");
		case EStageVisionBomKind::MonitorWedge: return LOCTEXT("MonitorWedge", "Monitor wedge");
		case EStageVisionBomKind::MixingDesk: return LOCTEXT("MixingDesk", "Mixing desk");
		case EStageVisionBomKind::LightingDesk: return LOCTEXT("LightingDesk", "Lighting desk");
		case EStageVisionBomKind::Barricade: return LOCTEXT("Barricade", "Crowd barricade");
		case EStageVisionBomKind::HazeMachine: return LOCTEXT("HazeMachine", "Haze machine");
		default: return LOCTEXT("Other", "Other equipment");
		}
	}

	FText DescribePosition(const FStageVisionBomLine& Line)
	{
		static const FText Sides[] = { LOCTEXT("FullWidth", "Full width"), LOCTEXT("StageLeft", "Stage left"), LOCTEXT("Centre", "Centre"),
			LOCTEXT("StageRight", "Stage right") };
		static const FText Depths[] = { LOCTEXT("Midstage", "midstage"), LOCTEXT("Upstage", "upstage"), LOCTEXT("Downstage", "downstage"),
			LOCTEXT("FrontOfHouse", "front of house") };
		static const FText Levels[] = { LOCTEXT("Floor", "floor"), LOCTEXT("OnDeck", "on deck"), LOCTEXT("Flown", "flown") };
		return FText::Format(LOCTEXT("Position", "{0}, {1}, {2}"), Sides[int32(Line.Side)], Depths[int32(Line.Depth)], Levels[int32(Line.Level)]);
	}

	FGameplayTag GetCategoryForKind(EStageVisionBomKind Kind)
	{
		switch (Kind)
		{
		case EStageVisionBomKind::TrussStraight: return StageCraftTags::Category_Truss_Straight;
		case EStageVisionBomKind::TrussCorner: return StageCraftTags::Category_Truss_Corner;
		case EStageVisionBomKind::TrussTower:
		case EStageVisionBomKind::GroundSupport: return StageCraftTags::Category_Truss_Tower;
		case EStageVisionBomKind::ChainHoist: return StageCraftTags::Category_Rigging_Motor;
		case EStageVisionBomKind::Deck: return StageCraftTags::Category_Stage_Deck;
		case EStageVisionBomKind::LedWall: return StageCraftTags::Category_Stage_Set;
		case EStageVisionBomKind::MovingHeadSpot:
		case EStageVisionBomKind::MovingHeadWash:
		case EStageVisionBomKind::MovingHeadBeam: return StageCraftTags::Category_Lighting_MovingHead;
		case EStageVisionBomKind::ParWash: return StageCraftTags::Category_Lighting_Par;
		case EStageVisionBomKind::Strobe:
		case EStageVisionBomKind::Blinder: return StageCraftTags::Category_Lighting_Strobe;
		case EStageVisionBomKind::Followspot: return StageCraftTags::Category_Lighting_Conventional;
		case EStageVisionBomKind::Laser: return StageCraftTags::Category_Lighting_Laser;
		case EStageVisionBomKind::LineArray: return StageCraftTags::Category_Audio_LineArray;
		case EStageVisionBomKind::Subwoofer: return StageCraftTags::Category_Audio_Subwoofer;
		case EStageVisionBomKind::SpeakerStack: return StageCraftTags::Category_Audio;
		case EStageVisionBomKind::MonitorWedge: return StageCraftTags::Category_Audio_Monitor;
		case EStageVisionBomKind::MixingDesk:
		case EStageVisionBomKind::LightingDesk: return StageCraftTags::Category_People_Crew;
		case EStageVisionBomKind::Barricade: return StageCraftTags::Category_Stage_Railing;
		default: return FGameplayTag();
		}
	}

	void MatchCatalog(TArray<FStageVisionBomLine>& Lines, TConstArrayView<const UBaseItemData*> Catalog)
	{
		for (FStageVisionBomLine& Line : Lines)
		{
			Line.Match.Reset();
			const FGameplayTag Category = GetCategoryForKind(Line.Kind);
			if (!Category.IsValid())
			{
				continue;
			}
			const TCHAR* Keyword = Kinds[int32(Line.Kind)].Keyword;
			const UBaseItemData* First = nullptr;
			const UBaseItemData* Preferred = nullptr;
			for (const UBaseItemData* Item : Catalog)
			{
				if (!Item || !Item->CategoryTag.MatchesTag(Category) || Item->CategoryTag.MatchesTag(StageCraftTags::Category_Imported))
				{
					continue;
				}
				First = First ? First : Item;
				if (Keyword && Item->DisplayName.ToString().Contains(Keyword, ESearchCase::IgnoreCase))
				{
					Preferred = Item;
					break;
				}
			}
			// A keyword that matches nothing falls back to the category, except where the keyword is the whole point (the crew category
			// holds the FOH lighting and sound positions and a stagehand; a followspot is not just any conventional fixture).
			Line.Match = const_cast<UBaseItemData*>(Preferred ? Preferred : (Kinds[int32(Line.Kind)].bKeywordRequired ? nullptr : First));
		}
	}

	FStageVisionBomTrussSections SplitTrussRun(double LengthM)
	{
		FStageVisionBomTrussSections Sections;
		if (!FMath::IsFinite(LengthM) || LengthM <= 0.0)
		{
			return Sections;
		}
		// Whole half metres, rounded up: a run is never shorter than the gap it spans.
		int32 Halves = int32(FMath::CeilToDouble(FMath::Min(LengthM, MaxDimensionM) * 2.0 - UE_KINDA_SMALL_NUMBER));
		Sections.ThreeMetre = Halves / 6;
		Halves %= 6;
		Sections.TwoMetre = Halves / 4;
		Halves %= 4;
		Sections.OneMetre = Halves / 2;
		Sections.HalfMetre = Halves % 2;
		return Sections;
	}

	int32 GetLedPanelCount(double WidthM, double HeightM)
	{
		if (!FMath::IsFinite(WidthM) || !FMath::IsFinite(HeightM) || WidthM <= 0.0 || HeightM <= 0.0)
		{
			return 0;
		}
		const auto Panels = [](double Metres) { return int32(FMath::CeilToDouble(FMath::Min(Metres, MaxDimensionM) * 2.0 - UE_KINDA_SMALL_NUMBER)); };
		return Panels(WidthM) * Panels(HeightM);
	}

	FText DescribeSpec(const FStageVisionBomLine& Line)
	{
		switch (Line.Kind)
		{
		case EStageVisionBomKind::TrussStraight:
		{
			const FStageVisionBomTrussSections Sections = SplitTrussRun(Line.LengthM);
			if (Sections.Num() == 0)
			{
				return FText::GetEmpty();
			}
			TArray<FString> Parts;
			const auto Add = [&Parts, &Line](int32 Count, const TCHAR* Size)
			{
				if (Count > 0)
				{
					Parts.Add(FString::Printf(TEXT("%d x %s"), Count * Line.Quantity, Size));
				}
			};
			Add(Sections.ThreeMetre, TEXT("3 m"));
			Add(Sections.TwoMetre, TEXT("2 m"));
			Add(Sections.OneMetre, TEXT("1 m"));
			Add(Sections.HalfMetre, TEXT("0.5 m"));
			return FText::Format(LOCTEXT("TrussSpec", "{0} {0}|plural(one=run,other=runs) of {1} m: {2} sections"), FText::AsNumber(Line.Quantity),
				FormatMetres(Line.LengthM), FText::FromString(FString::Join(Parts, TEXT(", "))));
		}
		case EStageVisionBomKind::LedWall:
		{
			const int32 Panels = GetLedPanelCount(Line.WidthM, Line.HeightM);
			return Panels == 0 ? FText::GetEmpty()
				: FText::Format(LOCTEXT("LedSpec", "{0} x {1} m: {2} panels of 500 x 500 mm"), FormatMetres(Line.WidthM), FormatMetres(Line.HeightM),
					FText::AsNumber(Panels * Line.Quantity));
		}
		case EStageVisionBomKind::Deck:
			return Line.WidthM > 0.0 && Line.LengthM > 0.0
				? FText::Format(LOCTEXT("DeckSpec", "{0} x {1} m, about {2} m² ({3} decks of 2 x 1 m)"), FormatMetres(Line.WidthM), FormatMetres(Line.LengthM),
					FormatMetres(Line.WidthM * Line.LengthM * Line.Quantity),
					FText::AsNumber(int32(FMath::CeilToDouble(Line.WidthM * Line.LengthM / 2.0 - UE_KINDA_SMALL_NUMBER)) * Line.Quantity))
				: FText::GetEmpty();
		case EStageVisionBomKind::TrussTower:
		case EStageVisionBomKind::GroundSupport:
			return Line.HeightM > 0.0 ? FText::Format(LOCTEXT("TowerSpec", "{0} m high"), FormatMetres(Line.HeightM)) : FText::GetEmpty();
		default:
			return FText::GetEmpty();
		}
	}

	FStageVisionBomTotals ComputeTotals(TConstArrayView<FStageVisionBomLine> Lines)
	{
		FStageVisionBomTotals Totals;
		for (const FStageVisionBomLine& Line : Lines)
		{
			Totals.LinesToReview += Line.NeedsReview() ? 1 : 0;
			const UBaseItemData* Item = Line.Match.Get();
			if (!Item)
			{
				++Totals.UnmatchedLines;
				continue;
			}
			++Totals.MatchedLines;
			if (!IsSpanKind(Line.Kind))
			{
				Totals.WeightKg += double(FMath::Max(Item->Specs.WeightKg, 0.f)) * Line.Quantity;
				Totals.PowerW += double(FMath::Max(Item->Specs.PowerDrawWatts, 0.f)) * Line.Quantity;
			}
		}
		return Totals;
	}

	FString ToCsv(TConstArrayView<FStageVisionBomLine> Lines)
	{
		const auto Cell = [](const FString& Value)
		{
			FString Safe = Value;
			// A leading = + - @ (or a tab / return) makes spreadsheets run the cell as a formula: the text came from a model.
			if (!Safe.IsEmpty() && FCString::Strchr(TEXT("=+-@\t\r"), Safe[0]))
			{
				Safe.InsertAt(0, TEXT('\''));
			}
			if (Safe.Contains(TEXT(",")) || Safe.Contains(TEXT("\"")) || Safe.Contains(TEXT("\n")) || Safe.Contains(TEXT("\r")))
			{
				Safe = TEXT("\"") + Safe.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
			}
			return Safe;
		};
		FString Csv = TEXT("Kind,Quantity,Position,Spec,Confidence,Catalog item,Weight each (kg),Power each (W),Notes\r\n");
		for (const FStageVisionBomLine& Line : Lines)
		{
			const UBaseItemData* Item = Line.Match.Get();
			TArray<FString> Cells;
			Cells.Add(Cell(GetKindDisplayName(Line.Kind).ToString()));
			Cells.Add(FString::FromInt(Line.Quantity));
			Cells.Add(Cell(DescribePosition(Line).ToString()));
			Cells.Add(Cell(DescribeSpec(Line).ToString()));
			Cells.Add(FString::Printf(TEXT("%.2f"), Line.Confidence));
			Cells.Add(Cell(Item ? Item->DisplayName.ToString() : FString()));
			Cells.Add(Item && Item->Specs.WeightKg > 0.f ? FString::Printf(TEXT("%.1f"), Item->Specs.WeightKg) : FString());
			Cells.Add(Item && Item->Specs.PowerDrawWatts > 0.f ? FString::Printf(TEXT("%.0f"), Item->Specs.PowerDrawWatts) : FString());
			Cells.Add(Cell(Line.Notes));
			Csv += FString::Join(Cells, TEXT(",")) + TEXT("\r\n");
		}
		return Csv;
	}

	FStageVisionBomLayout MakeLayout(const FStageVisionBomStage& Stage, bool bHasDeck)
	{
		FStageVisionBomLayout Layout;
		if (Stage.WidthM > 0.0)
		{
			Layout.StageWidthCm = FMath::Clamp(Stage.WidthM, 4.0, 40.0) * 100.0;
		}
		if (Stage.DepthM > 0.0)
		{
			Layout.StageDepthCm = FMath::Clamp(Stage.DepthM, 3.0, 25.0) * 100.0;
		}
		Layout.DeckHeightCm = !bHasDeck ? 0.0 : (Stage.DeckHeightM > 0.0 ? FMath::Clamp(Stage.DeckHeightM, 0.2, 3.0) * 100.0 : 120.0);
		Layout.TrussHeightCm = FMath::Max(600.0, Layout.DeckHeightCm + 450.0);
		return Layout;
	}

	FStageVisionBomBuildInput MakeBuildInput(TConstArrayView<FStageVisionBomLine> Lines, const FStageVisionBomLayout& Layout)
	{
		FStageVisionBomBuildInput Input;
		const double HalfWidth = Layout.StageWidthCm * 0.5;
		const double HalfDepth = Layout.StageDepthCm * 0.5;
		const bool bHasDeck = Lines.ContainsByPredicate([](const FStageVisionBomLine& Line) { return Line.Kind == EStageVisionBomKind::Deck; });

		// The deck: the whole stage area, one surface.
		if (bHasDeck)
		{
			FStageSketchStroke& Deck = Input.Strokes.AddDefaulted_GetRef();
			Deck.Kind = EStageSketchStrokeKind::Shape;
			Deck.bClosed = true;
			Deck.DeckHeight = float(Layout.DeckHeightCm);
			Deck.Points = { FVector2D(-HalfDepth, -HalfWidth), FVector2D(HalfDepth, -HalfWidth), FVector2D(HalfDepth, HalfWidth), FVector2D(-HalfDepth, HalfWidth) };
		}

		// Straight truss runs across the stage: downstage first, then upstage, then evenly between.
		TArray<double> RunLengths;
		for (const FStageVisionBomLine& Line : Lines)
		{
			if (Line.Kind != EStageVisionBomKind::TrussStraight)
			{
				continue;
			}
			const double Length = Line.LengthM > 0.0 ? FMath::Clamp(Line.LengthM * 100.0, 100.0, 4000.0) : Layout.StageWidthCm;
			for (int32 Run = 0; Run < Line.Quantity; ++Run)
			{
				RunLengths.Add(Length);
			}
		}
		constexpr int32 MaxRuns = 8;
		if (RunLengths.Num() > MaxRuns)
		{
			Input.Warnings.Add(FText::Format(LOCTEXT("TooManyRuns", "{0} truss runs were read; the first {1} are built."), FText::AsNumber(RunLengths.Num()), FText::AsNumber(MaxRuns)));
			RunLengths.SetNum(MaxRuns);
		}
		TArray<FTrussRun> Runs;
		const double FrontX = -HalfDepth + 50.0;
		const double BackX = HalfDepth - 50.0;
		for (int32 Index = 0; Index < RunLengths.Num(); ++Index)
		{
			FTrussRun Run;
			if (RunLengths.Num() == 1)
			{
				Run.X = FrontX;
			}
			else if (Index == 0)
			{
				Run.X = FrontX;
			}
			else if (Index == 1)
			{
				Run.X = BackX;
			}
			else
			{
				Run.X = FrontX + (BackX - FrontX) * double(Index - 1) / double(RunLengths.Num() - 1);
			}
			Run.HalfLength = RunLengths[Index] * 0.5;
			Runs.Add(Run);
			FStageSketchStroke& Stroke = Input.Strokes.AddDefaulted_GetRef();
			Stroke.Kind = EStageSketchStrokeKind::Truss;
			Stroke.Points = { FVector2D(Run.X, -Run.HalfLength), FVector2D(Run.X, Run.HalfLength) };
		}

		int32 Skipped = 0;
		for (const FStageVisionBomLine& Line : Lines)
		{
			if (Line.Kind == EStageVisionBomKind::Barricade)
			{
				Input.Warnings.Add(LOCTEXT("Barricade", "Crowd barricade is not built: draw it with the barricade tool in front of the stage."));
				continue;
			}
			if (Line.Kind == EStageVisionBomKind::Deck && Line.Quantity > 1)
			{
				Input.Warnings.Add(LOCTEXT("ManyDecks", "Several deck surfaces were read; the whole stage is built as one deck. Add risers in the Sketch window."));
				continue;
			}
			if (IsSpanKind(Line.Kind))
			{
				continue;
			}
			UBaseItemData* Item = Line.Match.Get();
			if (!Item)
			{
				Input.Warnings.Add(FText::Format(LOCTEXT("NotInCatalog", "{0} x {1}: not in the catalog, listed only."), FText::AsNumber(Line.Quantity),
					GetKindDisplayName(Line.Kind)));
				continue;
			}

			// The region the line's items spread over.
			double MinY = -HalfWidth + 50.0;
			double MaxY = HalfWidth - 50.0;
			switch (Line.Side)
			{
			case EStageVisionBomSide::StageLeft: MinY = HalfWidth / 3.0; break;
			case EStageVisionBomSide::StageRight: MaxY = -HalfWidth / 3.0; break;
			case EStageVisionBomSide::Centre: MinY = -HalfWidth / 3.0; MaxY = HalfWidth / 3.0; break;
			default: break;
			}
			double X = 0.0;
			switch (Line.Depth)
			{
			case EStageVisionBomDepth::Upstage: X = HalfDepth * 2.0 / 3.0; break;
			case EStageVisionBomDepth::Downstage: X = -HalfDepth * 2.0 / 3.0; break;
			case EStageVisionBomDepth::FrontOfHouse: X = -HalfDepth - Layout.FrontOfHouseCm; break;
			default: break;
			}
			if (Line.Kind == EStageVisionBomKind::LedWall)
			{
				X = HalfDepth + 100.0;
			}
			const bool bOnStage = Line.Depth != EStageVisionBomDepth::FrontOfHouse && Line.Kind != EStageVisionBomKind::LedWall;
			double Z = 0.0;
			FRotator Rotation(0.0, GetYaw(Line.Kind), 0.0);
			if (Line.Level == EStageVisionBomLevel::Flown && Line.Kind != EStageVisionBomKind::LedWall)
			{
				Z = Layout.TrussHeightCm;
				if (IsLightingKind(Line.Kind))
				{
					// Hung under the nearest run, upside down, within its length (as the sketch generator hangs fixtures).
					Rotation = FRotator(0.0, 0.0, 180.0);
					const FTrussRun* Nearest = nullptr;
					for (const FTrussRun& Run : Runs)
					{
						Nearest = !Nearest || FMath::Abs(Run.X - X) < FMath::Abs(Nearest->X - X) ? &Run : Nearest;
					}
					if (Nearest)
					{
						X = Nearest->X;
						MinY = FMath::Max(MinY, -Nearest->HalfLength + 25.0);
						MaxY = FMath::Min(MaxY, Nearest->HalfLength - 25.0);
						MaxY = FMath::Max(MinY, MaxY);
					}
				}
			}
			else if (bHasDeck && bOnStage)
			{
				Z = Layout.DeckHeightCm;
			}

			// Evenly over the region; when a row is full the next starts 80 cm further upstage (towards the audience at FOH).
			const double Spacing = GetSpacingCm(Line.Kind);
			const int32 PerRow = FMath::Max(1, int32((MaxY - MinY) / Spacing) + 1);
			const double RowStep = Line.Depth == EStageVisionBomDepth::FrontOfHouse ? -150.0 : 80.0;
			for (int32 Index = 0; Index < Line.Quantity; ++Index)
			{
				if (Input.Items.Num() >= MaxPlacedItems)
				{
					++Skipped;
					continue;
				}
				const int32 Row = Index / PerRow;
				const int32 InRow = FMath::Min(Line.Quantity - Row * PerRow, PerRow);
				const int32 Column = Index % PerRow;
				const double Y = InRow == 1 ? (MinY + MaxY) * 0.5 : MinY + (MaxY - MinY) * double(Column) / double(InRow - 1);
				Input.Items.Add({ Item, FTransform(Rotation, FVector(X + Row * RowStep, Y, Z)) });
			}
		}
		if (Skipped > 0)
		{
			Input.Warnings.Add(FText::Format(LOCTEXT("TooManyItems", "{0} more items were not placed: at most {1} are built from one photo."),
				FText::AsNumber(Skipped), FText::AsNumber(MaxPlacedItems)));
		}
		return Input;
	}
}

#undef LOCTEXT_NAMESPACE
