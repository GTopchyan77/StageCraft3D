// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGInput.cpp
//
// Implementation of the Enhanced Input endpoint:
//   /input/configure_imc
//
// Builds an InputMappingContext's key mappings and their modifier stack from a
// JSON description, replacing whatever the asset held before.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- UE5 HTTP Server -------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
// ---- Engine ----------------------------------------------------------------
#include "Async/Async.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Editor.h"
// ---- Asset tools -----------------------------------------------------------
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
// ---- Enhanced Input --------------------------------------------------------
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "EnhancedActionKeyMapping.h"


// ============================================================================
// Handler: POST /input/configure_imc
//
// Clears an InputMappingContext and writes WASD / gamepad mappings for a given
// InputAction.  Supports modifier names: "Negate", "SwizzleAxis", "DeadZone", "Scalar".
//
// Request body:
// {
//   "imc_path": "/Game/MyGame/Input/IMC_MyGame",
//   "mappings": [
//     { "action_path": "/Game/MyGame/Input/IA_Move", "key": "D", "modifiers": [] },
//     { "action_path": "/Game/MyGame/Input/IA_Move", "key": "A", "modifiers": ["Negate"] },
//     { "action_path": "/Game/MyGame/Input/IA_Move", "key": "W", "modifiers": ["SwizzleAxis"] },
//     { "action_path": "/Game/MyGame/Input/IA_Move", "key": "S", "modifiers": ["SwizzleAxis","Negate"] }
//   ]
// }
// ============================================================================

bool FNGGHttpServer::HandleInputConfigureIMC(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseError;
	TSharedPtr<FJsonObject> Body;

	if (!ParseJsonBody(Req, Body, ParseError))
	{
		OnComplete(JsonError(400, ParseError));
		return true;
	}

	FString IMCPath;
	Body->TryGetStringField(TEXT("imc_path"), IMCPath);

	const TArray<TSharedPtr<FJsonValue>>* MappingsArray = nullptr;
	if (IMCPath.IsEmpty() || !Body->TryGetArrayField(TEXT("mappings"), MappingsArray))
	{
		OnComplete(JsonError(400, TEXT("'imc_path' and 'mappings' array are required")));
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> MappingsCopy = *MappingsArray;
	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback, IMCPath, MappingsCopy]()
	{
		UInputMappingContext* IMC = LoadObject<UInputMappingContext>(nullptr, *IMCPath);
		if (!IMC)
		{
			Callback(JsonError(404, FString::Printf(TEXT("IMC not found: %s"), *IMCPath)));
			return;
		}

		IMC->UnmapAll();

		int32 MappingsAdded = 0;
		TArray<FString> Warnings;

		for (const TSharedPtr<FJsonValue>& Entry : MappingsCopy)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!Entry->TryGetObject(EntryObj)) continue;

			FString ActionPath, KeyName;
			(*EntryObj)->TryGetStringField(TEXT("action_path"), ActionPath);
			(*EntryObj)->TryGetStringField(TEXT("key"),         KeyName);

			if (ActionPath.IsEmpty() || KeyName.IsEmpty()) continue;

			UInputAction* Action = LoadObject<UInputAction>(nullptr, *ActionPath);
			if (!Action)
			{
				Warnings.Add(FString::Printf(TEXT("Action not found: %s"), *ActionPath));
				continue;
			}

			const FKey Key(*KeyName);
			if (!Key.IsValid())
			{
				Warnings.Add(FString::Printf(TEXT("Invalid key name: '%s'"), *KeyName));
				continue;
			}

			FEnhancedActionKeyMapping& Mapping = IMC->MapKey(Action, Key);

			const TArray<TSharedPtr<FJsonValue>>* ModArray = nullptr;
			if ((*EntryObj)->TryGetArrayField(TEXT("modifiers"), ModArray))
			{
				for (const TSharedPtr<FJsonValue>& ModVal : *ModArray)
				{
					FString ModName;
					if (!ModVal->TryGetString(ModName)) continue;

					UInputModifier* Modifier = nullptr;
					if      (ModName == TEXT("Negate"))      Modifier = NewObject<UInputModifierNegate>     (IMC);
					else if (ModName == TEXT("SwizzleAxis")) Modifier = NewObject<UInputModifierSwizzleAxis>(IMC);
					else if (ModName == TEXT("DeadZone"))    Modifier = NewObject<UInputModifierDeadZone>   (IMC);
					else if (ModName == TEXT("Scalar"))      Modifier = NewObject<UInputModifierScalar>     (IMC);
					else
						Warnings.Add(FString::Printf(TEXT("Unknown modifier '%s' — supported: Negate, SwizzleAxis, DeadZone, Scalar"), *ModName));

					if (Modifier) Mapping.Modifiers.Add(Modifier);
				}
			}

			++MappingsAdded;
		}

		IMC->MarkPackageDirty();

		TArray<TSharedPtr<FJsonValue>> WarnValues;
		for (const FString& W : Warnings)
			WarnValues.Add(MakeShared<FJsonValueString>(W));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),       true);
		Result->SetStringField(TEXT("imc_path"),      IMCPath);
		Result->SetNumberField(TEXT("mappings_added"), MappingsAdded);
		Result->SetArrayField (TEXT("warnings"),      WarnValues);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}
