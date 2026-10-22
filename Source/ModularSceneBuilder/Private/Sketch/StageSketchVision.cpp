// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sketch/StageSketchVision.h"

#include "Algo/Find.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sketch/StageSketchImageTrace.h"

#define LOCTEXT_NAMESPACE "StageSketchVision"

namespace StageSketchVision
{
	namespace
	{
		/** Limits of one answer, the same as the local reading's. */
		constexpr int32 MaxPointsPerElement = 128;

		/** Points this far outside the image (a fraction of its size) are clamped onto it; farther ones are dropped. */
		constexpr double EdgeSlack = 0.02;

		TSharedRef<FJsonObject> MakeObjectSchema(const TSharedRef<FJsonObject>& Properties, TArray<TSharedPtr<FJsonValue>> Required)
		{
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Properties);
			Schema->SetArrayField(TEXT("required"), Required);
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			return Schema;
		}

		TSharedRef<FJsonObject> MakeTyped(const TCHAR* Type, const FString& Description)
		{
			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), Type);
			if (!Description.IsEmpty())
			{
				Property->SetStringField(TEXT("description"), Description);
			}
			return Property;
		}

		TSharedRef<FJsonObject> MakeArray(const TSharedRef<FJsonObject>& Items, const FString& Description)
		{
			TSharedRef<FJsonObject> Array = MakeTyped(TEXT("array"), Description);
			Array->SetObjectField(TEXT("items"), Items);
			return Array;
		}

		TArray<TSharedPtr<FJsonValue>> Names(std::initializer_list<const TCHAR*> List)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TCHAR* Name : List)
			{
				Values.Add(MakeShared<FJsonValueString>(Name));
			}
			return Values;
		}

		TSharedRef<FJsonObject> MakePointSchema()
		{
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("x"), MakeTyped(TEXT("number"), TEXT("Fraction of the image width from its left edge, 0 to 1.")));
			Properties->SetObjectField(TEXT("y"), MakeTyped(TEXT("number"), TEXT("Fraction of the image height from its top edge, 0 to 1.")));
			return MakeObjectSchema(Properties, Names({ TEXT("x"), TEXT("y") }));
		}

		TSharedRef<FJsonObject> MakeToolDefinition()
		{
			TSharedRef<FJsonObject> DeckProperties = MakeShared<FJsonObject>();
			DeckProperties->SetObjectField(TEXT("corners"), MakeArray(MakePointSchema(), TEXT("The outline's corners in order around it (at least 3).")));
			DeckProperties->SetObjectField(TEXT("height_cm"), MakeTyped(TEXT("number"), TEXT("Deck height in cm, only when the drawing states it.")));
			TSharedRef<FJsonObject> TrussProperties = MakeShared<FJsonObject>();
			TrussProperties->SetObjectField(TEXT("points"), MakeArray(MakePointSchema(), TEXT("The run's points in order (at least 2): its ends and every bend.")));
			TrussProperties->SetObjectField(TEXT("closed"), MakeTyped(TEXT("boolean"), TEXT("True when the run is a closed loop (the last point joins the first).")));

			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("decks"), MakeArray(MakeObjectSchema(DeckProperties, Names({ TEXT("corners") })),
				TEXT("Stage decks, risers and platforms: closed areas people stand on.")));
			Properties->SetObjectField(TEXT("truss"), MakeArray(MakeObjectSchema(TrussProperties, Names({ TEXT("points"), TEXT("closed") })),
				TEXT("Truss runs: lines, double lines or ladder / zig-zag bands hung above the stage.")));
			Properties->SetObjectField(TEXT("fixtures"), MakeArray(MakePointSchema(), TEXT("Lighting fixtures: small circles, dots or crosses, at their centre.")));
			Properties->SetObjectField(TEXT("notes"), MakeTyped(TEXT("string"), TEXT("One short sentence on anything that could not be read.")));

			TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
			Tool->SetStringField(TEXT("name"), GetToolName());
			Tool->SetStringField(TEXT("description"), TEXT("Report the stage elements drawn in the image, as seen from above, in image fractions. Call it once."));
			Tool->SetBoolField(TEXT("strict"), true);
			Tool->SetObjectField(TEXT("input_schema"), MakeObjectSchema(Properties, Names({ TEXT("decks"), TEXT("truss"), TEXT("fixtures") })));
			return Tool;
		}

		FString Serialize(const TSharedRef<FJsonObject>& Object)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
			return Out;
		}

		/** A point in 0..1 (slightly outside is clamped), or unset when it is missing, not a number or too far out. */
		TOptional<FVector2D> ReadPoint(const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			double X = 0.0;
			double Y = 0.0;
			if (!Object.IsValid() || !Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y)
				|| !FMath::IsFinite(X) || !FMath::IsFinite(Y) || X < -EdgeSlack || Y < -EdgeSlack || X > 1.0 + EdgeSlack || Y > 1.0 + EdgeSlack)
			{
				return {};
			}
			return FVector2D(FMath::Clamp(X, 0.0, 1.0), FMath::Clamp(Y, 0.0, 1.0));
		}

		/** Up to MaxPointsPerElement valid points of Field; the rest counted as dropped. */
		TArray<FVector2D> ReadPoints(const FJsonObject& Element, const TCHAR* Field, int32& Dropped)
		{
			TArray<FVector2D> Points;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Element.TryGetArrayField(Field, Values) || !Values)
			{
				return Points;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TOptional<FVector2D> Point = ReadPoint(Value);
				if (Point.IsSet() && Points.Num() < MaxPointsPerElement)
				{
					Points.Add(*Point);
				}
				else
				{
					++Dropped;
				}
			}
			return Points;
		}

		/** The plan in the tool's input, with every element checked. False only when the input is not shaped like the tool. */
		bool ReadPlan(const FJsonObject& Input, FStageSketchVisionResult& Result)
		{
			static const TCHAR* const KnownFields[] = { TEXT("decks"), TEXT("truss"), TEXT("fixtures"), TEXT("notes") };
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Input.Values)
			{
				if (!Algo::FindByPredicate(KnownFields, [&Field](const TCHAR* Known) { return Field.Key == Known; }))
				{
					Result.Error = FText::Format(LOCTEXT("UnknownField", "The reading had an unknown field \"{0}\"."), FText::FromString(Field.Key.Left(32)));
					return false;
				}
			}
			const auto ReadElements = [&Input, &Result](const TCHAR* Field, int32 Limit, TFunctionRef<void(const TSharedPtr<FJsonValue>&)> Read)
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Input.TryGetArrayField(Field, Values) || !Values)
				{
					return;
				}
				for (int32 Index = 0; Index < Values->Num(); ++Index)
				{
					if (Index < Limit)
					{
						Read((*Values)[Index]);
					}
					else
					{
						++Result.Dropped;
					}
				}
			};

			ReadElements(TEXT("decks"), StageSketchImageTrace::MaxDecks, [&Result](const TSharedPtr<FJsonValue>& Value)
			{
				const TSharedPtr<FJsonObject> Deck = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Deck.IsValid())
				{
					++Result.Dropped;
					return;
				}
				FStageSketchStroke Stroke;
				Stroke.Kind = EStageSketchStrokeKind::Shape;
				Stroke.bClosed = true;
				Stroke.Points = ReadPoints(*Deck, TEXT("corners"), Result.Dropped);
				double Height = 0.0;
				// 0 = "not given": the importer applies the panel's deck height; the sketch clamps any stated one.
				Stroke.DeckHeight = Deck->TryGetNumberField(TEXT("height_cm"), Height) && FMath::IsFinite(Height) && Height > 0.0 ? float(Height) : 0.f;
				if (Stroke.Points.Num() >= 3)
				{
					Result.Strokes.Add(MoveTemp(Stroke));
				}
				else
				{
					++Result.Dropped;
				}
			});
			ReadElements(TEXT("truss"), StageSketchImageTrace::MaxTrussLines, [&Result](const TSharedPtr<FJsonValue>& Value)
			{
				const TSharedPtr<FJsonObject> Truss = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Truss.IsValid())
				{
					++Result.Dropped;
					return;
				}
				FStageSketchStroke Stroke;
				Stroke.Kind = EStageSketchStrokeKind::Truss;
				Stroke.Points = ReadPoints(*Truss, TEXT("points"), Result.Dropped);
				Truss->TryGetBoolField(TEXT("closed"), Stroke.bClosed);
				if (Stroke.Points.Num() >= (Stroke.bClosed ? 3 : 2))
				{
					Result.Strokes.Add(MoveTemp(Stroke));
				}
				else
				{
					++Result.Dropped;
				}
			});
			ReadElements(TEXT("fixtures"), StageSketchImageTrace::MaxFixtures, [&Result](const TSharedPtr<FJsonValue>& Value)
			{
				const TOptional<FVector2D> Point = ReadPoint(Value);
				if (!Point.IsSet())
				{
					++Result.Dropped;
					return;
				}
				FStageSketchStroke& Stroke = Result.Strokes.AddDefaulted_GetRef();
				Stroke.Kind = EStageSketchStrokeKind::Fixture;
				Stroke.Points.Add(*Point);
			});
			Input.TryGetStringField(TEXT("notes"), Result.Text);
			Result.Text = Result.Text.TrimStartAndEnd().Left(200);
			return true;
		}
	}

	const TCHAR* GetToolName()
	{
		return TEXT("sketch_plan");
	}

	FString MakeToolDefinitionJson()
	{
		return Serialize(MakeToolDefinition());
	}

	FString GetSystemPrompt()
	{
		return TEXT(
			"You read stage plans for StageCraft 3D, a concert and event stage designer. The image is a hand drawing, a printed plan, a scan "
			"or a photo of one, seen from above. Report what is drawn with one sketch_plan call:\n"
			"- decks: closed outlines of stage decks, risers and platforms, each as its corners in order. A smaller outline drawn inside a "
			"larger one is a riser on it: report both.\n"
			"- truss: truss runs, drawn as single or double lines or as ladder / zig-zag bands. Report the centre line, its ends and bends; "
			"closed for a loop.\n"
			"- fixtures: lighting fixtures, drawn as small circles, dots or crosses, at their centre.\n"
			"Coordinates are fractions of the image: x from its left edge (0) to its right edge (1), y from its top edge (0) to its bottom "
			"edge (1). Read the drawing, not the paper: ignore text, dimensions, arrows, the table, shadows and the sheet's edges. Labels "
			"may be in English or Armenian. Set height_cm only when the drawing states a deck's height. If the image is not a stage plan, "
			"do not call the tool; say so in one sentence.");
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
		Text->SetStringField(TEXT("text"), TEXT("Read this stage plan."));
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), TEXT("user"));
		Message->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(Image), MakeShared<FJsonValueObject>(Text) });

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("model"), Options.Model);
		Body->SetNumberField(TEXT("max_tokens"), Options.MaxTokens);
		Body->SetStringField(TEXT("system"), GetSystemPrompt());
		// Retries a safety decline on a fallback model inside the same call (beta server-side-fallback-2026-07-01).
		Body->SetStringField(TEXT("fallbacks"), TEXT("default"));
		// Reading a drawing needs more care than turning one sentence into a command: never below medium.
		TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
		OutputConfig->SetStringField(TEXT("effort"), Options.Effort == TEXT("low") ? FString(TEXT("medium")) : Options.Effort);
		Body->SetObjectField(TEXT("output_config"), OutputConfig);
		Body->SetArrayField(TEXT("tools"), { MakeShared<FJsonValueObject>(MakeToolDefinition()) });
		// Forced tool choice is not supported on this model family: auto, at most one call.
		TSharedRef<FJsonObject> ToolChoice = MakeShared<FJsonObject>();
		ToolChoice->SetStringField(TEXT("type"), TEXT("auto"));
		ToolChoice->SetBoolField(TEXT("disable_parallel_tool_use"), true);
		Body->SetObjectField(TEXT("tool_choice"), ToolChoice);
		Body->SetArrayField(TEXT("messages"), { MakeShared<FJsonValueObject>(Message) });
		return Serialize(Body);
	}

	FStageSketchVisionResult ParseResponseBody(int32 HttpStatus, const FString& Body)
	{
		FStageSketchVisionResult Result;
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
				ErrorMessage.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT(": ") + ErrorMessage.Left(200)));
			return Result;
		}
		FString StopReason;
		Root->TryGetStringField(TEXT("stop_reason"), StopReason);
		if (StopReason == TEXT("refusal"))
		{
			Result.Status = FStageSketchVisionResult::EStatus::Refused;
			Result.Error = LOCTEXT("Declined", "The online model declined to read the image.");
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
			const TSharedPtr<FJsonObject> Block = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Type;
			if (!Block.IsValid() || !Block->TryGetStringField(TEXT("type"), Type))
			{
				continue;
			}
			FString Name;
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (Type == TEXT("tool_use") && Block->TryGetStringField(TEXT("name"), Name) && Name == GetToolName() && Block->TryGetObjectField(TEXT("input"), Input) && Input)
			{
				if (ReadPlan(**Input, Result))
				{
					Result.Status = FStageSketchVisionResult::EStatus::Plan;
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
		Result.Status = FStageSketchVisionResult::EStatus::Text;
		Result.Text = FString::Join(Texts, TEXT(" ")).Left(300);
		return Result;
	}

	TArray<FStageSketchStroke> ToPlan(TConstArrayView<FStageSketchStroke> Fractions, int32 Width, int32 Height, double WidthCm, float DeckHeight)
	{
		TArray<FStageSketchStroke> Strokes;
		Strokes.Reserve(Fractions.Num());
		for (const FStageSketchStroke& Fraction : Fractions)
		{
			FStageSketchStroke& Stroke = Strokes.Add_GetRef(Fraction);
			if (Stroke.GetElement() == EStageSketchElement::Deck && Stroke.DeckHeight <= 0.f)
			{
				Stroke.DeckHeight = DeckHeight;
			}
			for (FVector2D& Point : Stroke.Points)
			{
				Point = StageSketchImageTrace::PixelToPlan(FVector2D(Point.X * Width, Point.Y * Height), Width, Height, WidthCm);
			}
		}
		return Strokes;
	}
}

#undef LOCTEXT_NAMESPACE
