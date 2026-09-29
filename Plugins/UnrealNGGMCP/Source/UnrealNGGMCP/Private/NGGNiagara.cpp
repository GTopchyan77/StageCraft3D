// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGNiagara.cpp
//
// Implementation of the Niagara endpoints:
//   /niagara/{create,configure,set_emitter_params}
//
// Niagara systems are created from a template system and then reconfigured, so
// the emitter handles the caller sees are the ones the template shipped with.

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
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
// ---- Niagara particle system -----------------------------------------------
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/PropertyIterator.h"


// ============================================================================
// Handler: POST /editor/batch
// Body: { "operations": [ {"method":"POST","path":"/editor/save_all","body":{}}, ... ] }
// Executes multiple sub-operations and returns an ordered result array.
// Each result entry: { "index": N, "status": 200, "body": {...} }
//
// ============================================================================
// HandleCreateNiagaraSystem
// POST /editor/create_niagara_system
// Body: { "asset_path": "/Game/VFX/NS_BoxDestroy", "template_path": "" (optional) }
//
// Creates a blank UNiagaraSystem asset at the given content path.
// If template_path is supplied, the existing system is duplicated instead.
// The caller must open the resulting asset in the Niagara editor to add and
// configure emitter modules (spawn rate, velocity, colour, lifetime, etc.).
// ============================================================================
bool FNGGHttpServer::HandleCreateNiagaraSystem(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	FString TemplatePath;
	Body->TryGetStringField(TEXT("template_path"), TemplatePath);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, TemplatePath]()
	{
		// --- Split asset_path into PackagePath + AssetName -------------------
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName,
			ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		if (AssetName.IsEmpty())
		{
			Callback(JsonError(400, TEXT("Could not derive asset name from asset_path — path must contain at least one '/'")));
			return;
		}

		// --- Check if asset already exists -----------------------------------
		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
		UNiagaraSystem* ExistingSystem = LoadObject<UNiagaraSystem>(nullptr, *FullObjectPath);
		if (ExistingSystem)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField  (TEXT("success"),        true);
			Result->SetBoolField  (TEXT("already_existed"),true);
			Result->SetStringField(TEXT("asset_path"),     AssetPath);

			FString BodyStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			Callback(JsonOk(BodyStr));
			return;
		}

		UNiagaraSystem* NewSystem = nullptr;

		// --- Option A: Clone from a template ---------------------------------
		if (!TemplatePath.IsEmpty())
		{
			FString TemplateAssetName;
			TemplatePath.Split(TEXT("/"), nullptr, &TemplateAssetName,
				ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			const FString TemplateObjectPath = TemplatePath + TEXT(".") + TemplateAssetName;

			UNiagaraSystem* Template = LoadObject<UNiagaraSystem>(nullptr, *TemplateObjectPath);
			if (Template)
			{
				UPackage* NewPackage = CreatePackage(*AssetPath);
				NewSystem = Cast<UNiagaraSystem>(
					StaticDuplicateObject(Template, NewPackage, *AssetName));
				if (NewSystem)
				{
					NewSystem->SetFlags(RF_Public | RF_Standalone);
				}
			}
			else
			{
				// Non-fatal: fall through to factory creation
				UE_LOG(LogTemp, Warning,
					TEXT("HandleCreateNiagaraSystem: template '%s' not found, creating blank system instead"),
					*TemplatePath);
			}
		}

		// --- Option B: Create via auto-discovered factory --------------------
		if (!NewSystem)
		{
			IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

			// Iterate all loaded UFactory subclasses to find the one registered
			// for UNiagaraSystem — avoids a hard compile-time dependency on
			// the NiagaraEditor module include paths.
			UFactory* NiagaraFactory = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->HasAnyClassFlags(CLASS_Abstract) || !It->IsChildOf(UFactory::StaticClass()))
					continue;

				UFactory* Candidate = Cast<UFactory>(It->GetDefaultObject());
				if (Candidate && Candidate->SupportedClass == UNiagaraSystem::StaticClass())
				{
					NiagaraFactory = NewObject<UFactory>(GetTransientPackage(), *It);
					break;
				}
			}

			if (!NiagaraFactory)
			{
				Callback(JsonError(500,
					TEXT("No UFactory found for UNiagaraSystem. "
					     "Ensure the Niagara editor plugin is enabled and loaded.")));
				return;
			}

			UObject* Asset = AT.CreateAsset(AssetName, PackagePath,
				UNiagaraSystem::StaticClass(), NiagaraFactory);
			NewSystem = Cast<UNiagaraSystem>(Asset);
		}

		if (!NewSystem)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("Failed to create Niagara system at '%s'"), *AssetPath)));
			return;
		}

		NewSystem->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),        true);
		Result->SetBoolField  (TEXT("already_existed"),false);
		Result->SetStringField(TEXT("asset_path"),     AssetPath);
		Result->SetStringField(TEXT("next_step"),
			TEXT("Niagara system created. Open it in the Niagara editor to add emitter "
			     "modules (spawn burst, initialize particle, add velocity, gravity, "
			     "sprite renderer). Then call ue5_save_all to persist."));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonCreated(BodyStr));
	});

	return true;
}

// ============================================================================
// HandleConfigureNiagaraSystem
// POST /editor/configure_niagara_system
//
// Body:
//   asset_path    (required) — content path to an existing UNiagaraSystem
//   spawn_count   — particles per burst  (default 10)
//   lifetime_min  — minimum particle lifetime in seconds  (default 0.6)
//   lifetime_max  — maximum particle lifetime in seconds  (default 0.8)
//   color_r/g/b   — linear-space emitter colour  (default: #8B5E3C wood brown)
//   sprite_size   — sprite size in UU  (default 3.0)
//   velocity      — outward launch speed in cm/s  (default 200)
//   gravity_z     — gravity acceleration Z  (default -980)
//   loop_behavior — "Once" | "Infinite" | "Multiple"  (default "Once")
//
// Steps performed:
//   1. If the system has no emitters, scan the Asset Registry for an engine
//      sprite-burst emitter template and add it via
//      AddEmitterHandleWithoutDuplicateOf.
//   2. Try to set user-exposed parameters on the system's parameter store
//      (only works if the template emitter exposes them as User.* variables).
//   3. RequestCompile + MarkPackageDirty.
//   4. Return a full report: emitter_added, params_set, params_not_exposed,
//      all_exposed_params (so the caller knows what can be driven at runtime).
// ============================================================================
bool FNGGHttpServer::HandleConfigureNiagaraSystem(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	// --- Parse optional config with sane defaults ----------------------------
	int32   SpawnCount   = 10;
	float   LifetimeMin  = 0.6f;
	float   LifetimeMax  = 0.8f;
	float   ColorR       = 0.212f;  // #8B5E3C linear
	float   ColorG       = 0.139f;
	float   ColorB       = 0.071f;
	float   SpriteSize   = 3.0f;
	float   Velocity     = 200.0f;
	float   GravityZ     = -980.0f;
	FString LoopBehavior = TEXT("Once");

	{
		double Tmp = 0.0;
		if (Body->TryGetNumberField(TEXT("spawn_count"),   Tmp)) SpawnCount  = (int32)Tmp;
		if (Body->TryGetNumberField(TEXT("lifetime_min"),  Tmp)) LifetimeMin = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("lifetime_max"),  Tmp)) LifetimeMax = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("color_r"),       Tmp)) ColorR      = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("color_g"),       Tmp)) ColorG      = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("color_b"),       Tmp)) ColorB      = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("sprite_size"),   Tmp)) SpriteSize  = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("velocity"),      Tmp)) Velocity    = (float)Tmp;
		if (Body->TryGetNumberField(TEXT("gravity_z"),     Tmp)) GravityZ    = (float)Tmp;
		Body->TryGetStringField(TEXT("loop_behavior"), LoopBehavior);
	}

	// reset_emitters: remove existing handles and re-add from template (default true so
	// the endpoint is always idempotent — broken handles from a previous call are cleared)
	bool bResetEmitters = true;
	Body->TryGetBoolField(TEXT("reset_emitters"), bResetEmitters);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, AssetPath, SpawnCount, LifetimeMin, LifetimeMax,
		 ColorR, ColorG, ColorB, SpriteSize, Velocity, GravityZ, LoopBehavior, bResetEmitters]()
	{
		// --- Load system ------------------------------------------------------
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName,
			ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(
			nullptr, *(AssetPath + TEXT(".") + AssetName));

		TArray<FString> Steps;
		bool bAddedEmitter = false;
		FString TemplateUsed;

		// --- Step 1: Clone from engine template system if needed ------------------
		// UE5.7's versioned emitter system makes AddEmitterHandle unreliable —
		// the only reliable way to get a working particle system is to DUPLICATE
		// an entire engine template system (which has all modules pre-configured).
		const bool bNeedsClone = !System || bResetEmitters;

		if (bNeedsClone)
		{
			Steps.Add(TEXT("Using system-clone approach for reliable emitter setup"));

			// Find the best engine template system to clone from
			IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")).Get();

			FARFilter Filter;
			Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths  = true;
			Filter.bRecursiveClasses = false;
			Filter.PackagePaths.Add(FName("/Niagara"));

			TArray<FAssetData> SystemAssets;
			AR.GetAssets(Filter, SystemAssets);

			// Priority: SimpleExplosion > RadialBurst > DirectionalBurst > any
			static const TArray<FString> Keywords = {
				TEXT("SimpleExplosion"), TEXT("RadialBurst"), TEXT("DirectionalBurst"),
				TEXT("Burst"), TEXT("Explosion")
			};

			UNiagaraSystem* TemplateSystem = nullptr;
			for (const FString& Kw : Keywords)
			{
				for (const FAssetData& AD : SystemAssets)
				{
					if (AD.AssetName.ToString().Contains(Kw, ESearchCase::IgnoreCase))
					{
						TemplateSystem = Cast<UNiagaraSystem>(AD.GetAsset());
						if (TemplateSystem)
						{
							TemplateUsed = AD.GetObjectPathString();
							break;
						}
					}
				}
				if (TemplateSystem) break;
			}

			if (TemplateSystem)
			{
				// Delete existing asset if present
				if (System)
				{
					// Rename old to temp so we can reuse the path
					System->Rename(*FString::Printf(TEXT("%s_OLD_%d"), *AssetName, FMath::Rand()),
						nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
					Steps.Add(TEXT("Renamed existing system to make room for clone"));
				}

				// Duplicate the template system to the target path
				FString DestPackageName = AssetPath;
				UPackage* DestPackage = CreatePackage(*DestPackageName);
				if (DestPackage)
				{
					System = Cast<UNiagaraSystem>(
						StaticDuplicateObject(TemplateSystem, DestPackage, *AssetName));

					if (System)
					{
						System->SetFlags(RF_Public | RF_Standalone);
						System->MarkPackageDirty();
						FAssetRegistryModule::AssetCreated(System);
						bAddedEmitter = true;

						// Force compile after clone
						System->RequestCompile(/*bForce=*/true);
						System->WaitForCompilationComplete();

						Steps.Add(FString::Printf(TEXT("Cloned engine template system: %s"), *TemplateUsed));
					}
					else
					{
						Steps.Add(TEXT("StaticDuplicateObject failed — could not clone template system"));
					}
				}
			}
			else
			{
				Steps.Add(TEXT("No matching engine Niagara system template found under /Niagara"));
			}
		}
		else
		{
			Steps.Add(FString::Printf(
				TEXT("System already has %d emitter(s) and reset_emitters=false — skipping clone"),
				System->GetEmitterHandles().Num()));
		}

		if (!System)
		{
			Callback(JsonError(500, TEXT("System is null after clone attempt — check engine template availability")));
			return;
		}

		// --- Step 2: Set user-exposed parameters ------------------------------
		FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

		TArray<FNiagaraVariable> AllExposed;
		Store.GetParameters(AllExposed);

		TArray<FString> ParamsSet;
		TArray<FString> ParamsNotExposed;

		// Helper: attempt to set a user parameter via the base-class template.
		// SetParameterValue(value, var, bAdd=false) returns true only if the
		// parameter already exists in the store — no new param is added.
		auto TryFloat = [&](const TCHAR* Name, float Value)
		{
			FNiagaraVariable Var(FNiagaraTypeDefinition::GetFloatDef(), Name);
			if (Store.SetParameterValue(Value, Var, /*bAdd=*/false))
				ParamsSet.Add(FString::Printf(TEXT("%s = %.3f"), Name, Value));
			else
				ParamsNotExposed.Add(Name);
		};

		auto TryInt = [&](const TCHAR* Name, int32 Value)
		{
			FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), Name);
			if (Store.SetParameterValue(Value, Var, /*bAdd=*/false))
				ParamsSet.Add(FString::Printf(TEXT("%s = %d"), Name, Value));
			else
				ParamsNotExposed.Add(Name);
		};

		auto TryColor = [&](const TCHAR* Name, FLinearColor Value)
		{
			FNiagaraVariable Var(FNiagaraTypeDefinition::GetColorDef(), Name);
			if (Store.SetParameterValue(Value, Var, /*bAdd=*/false))
				ParamsSet.Add(FString::Printf(TEXT("%s = (%.3f,%.3f,%.3f)"),
					Name, Value.R, Value.G, Value.B));
			else
				ParamsNotExposed.Add(Name);
		};

		// Try the common naming conventions used by Niagara built-in templates
		TryInt  (TEXT("User.SpawnCount"),      SpawnCount);
		TryInt  (TEXT("User.SpawnBurstCount"), SpawnCount);
		TryFloat(TEXT("User.LifetimeMin"),     LifetimeMin);
		TryFloat(TEXT("User.LifetimeMax"),     LifetimeMax);
		TryFloat(TEXT("User.Lifetime"),        (LifetimeMin + LifetimeMax) * 0.5f);
		TryFloat(TEXT("User.SpriteSize"),      SpriteSize);
		TryFloat(TEXT("User.SpriteSizeMin"),   SpriteSize * 0.75f);
		TryFloat(TEXT("User.SpriteSizeMax"),   SpriteSize * 1.25f);
		TryFloat(TEXT("User.Velocity"),        Velocity);
		TryFloat(TEXT("User.VelocityMin"),     Velocity * 0.5f);
		TryFloat(TEXT("User.VelocityMax"),     Velocity);
		TryFloat(TEXT("User.GravityZ"),        GravityZ);
		TryColor(TEXT("User.Color"),           FLinearColor(ColorR, ColorG, ColorB, 1.0f));
		TryColor(TEXT("User.StartColor"),      FLinearColor(ColorR, ColorG, ColorB, 1.0f));

		Steps.Add(FString::Printf(
			TEXT("Parameter store had %d exposed variables; set %d, skipped %d (not exposed)"),
			AllExposed.Num(), ParamsSet.Num(), ParamsNotExposed.Num()));

		// --- Step 3: Compile and save ----------------------------------------
		System->RequestCompile(/*bForce=*/true);
		System->WaitForCompilationComplete();
		System->MarkPackageDirty();
		Steps.Add(TEXT("RequestCompile (forced) + WaitForCompilationComplete"));

		// --- Build response ---------------------------------------------------
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),        true);
		Result->SetStringField(TEXT("asset_path"),     AssetPath);
		Result->SetBoolField  (TEXT("emitter_added"),  bAddedEmitter);
		Result->SetStringField(TEXT("template_used"),  TemplateUsed);

		// parameters
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();

			TArray<TSharedPtr<FJsonValue>> SetArr;
			for (const FString& S : ParamsSet)
				SetArr.Add(MakeShared<FJsonValueString>(S));
			P->SetArrayField(TEXT("set"), SetArr);

			TArray<TSharedPtr<FJsonValue>> NEArr;
			for (const FString& S : ParamsNotExposed)
				NEArr.Add(MakeShared<FJsonValueString>(S));
			P->SetArrayField(TEXT("not_exposed"), NEArr);

			TArray<TSharedPtr<FJsonValue>> ExposedArr;
			for (const FNiagaraVariable& V : AllExposed)
				ExposedArr.Add(MakeShared<FJsonValueString>(V.GetName().ToString()));
			P->SetArrayField(TEXT("all_exposed"), ExposedArr);

			Result->SetObjectField(TEXT("parameters"), P);
		}

		// steps log
		TArray<TSharedPtr<FJsonValue>> StepsArr;
		for (const FString& S : Steps)
			StepsArr.Add(MakeShared<FJsonValueString>(S));
		Result->SetArrayField(TEXT("steps"), StepsArr);

		// manual_config: properties that couldn't be set via user params
		if (ParamsNotExposed.Num() > 0)
		{
			FString ManualNote = FString::Printf(
				TEXT("The following could not be set via user-exposed parameters and must be "
				     "configured manually inside the Niagara editor emitter stack: %s"),
				*FString::Join(ParamsNotExposed, TEXT(", ")));
			Result->SetStringField(TEXT("manual_config_required"), ManualNote);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonCreated(BodyStr));
	});

	return true;
}

// ============================================================================
// HandleSetNiagaraEmitterParams
// POST /editor/set_niagara_emitter_params
//
// Body:
//   asset_path      (required) — content path to an existing UNiagaraSystem
//   emitter_index   (optional, default 0) — which emitter in the system
//   list_only       (optional, default false) — if true, just dump all RIP names/types
//   spawn_count     (optional) — particle burst count
//   lifetime_min    (optional) — minimum particle lifetime in seconds
//   lifetime_max    (optional) — maximum particle lifetime in seconds
//   color_r/g/b     (optional) — linear-space RGB colour
//   sprite_size     (optional) — sprite size in UU
//   velocity_min    (optional) — minimum outward speed cm/s
//   velocity_max    (optional) — maximum outward speed cm/s
//   gravity_z       (optional) — gravity on Z axis cm/s²
//   raw_params      (optional) — [{ "name": "exact.RIP.Name", "value": 1.0 }]
//
// Unlike configure_niagara_system which only sets User.* exposed parameters,
// this endpoint walks every script's RapidIterationParameters store — the baked
// values that live inside the emitter module stack — and sets them directly.
// Use list_only=true first to discover the exact RIP names on an unfamiliar emitter.
// ============================================================================
bool FNGGHttpServer::HandleSetNiagaraEmitterParams(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath; Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}

	int32 EmitterIndex = 0;
	bool  bListOnly    = false;
	bool  bRemoveEmitter = false;
	{
		double Tmp = 0;
		if (Body->TryGetNumberField(TEXT("emitter_index"), Tmp)) EmitterIndex = (int32)Tmp;
		Body->TryGetBoolField(TEXT("list_only"), bListOnly);
		Body->TryGetBoolField(TEXT("remove_emitter"), bRemoveEmitter);
	}

	// Named shorthand params — stored in a map so the lambda only captures one object.
	// We use a sentinel of -1e30f to mean "not provided" for every field.
	TMap<FString, float> NamedParams;
	{
		static const FString Keys[] = {
			TEXT("spawn_count"), TEXT("lifetime_min"), TEXT("lifetime_max"),
			TEXT("color_r"), TEXT("color_g"), TEXT("color_b"),
			TEXT("sprite_size"), TEXT("velocity_min"), TEXT("velocity_max"), TEXT("gravity_z")
		};
		for (const FString& K : Keys)
		{
			double Tmp = 0;
			if (Body->TryGetNumberField(K, Tmp))
				NamedParams.Add(K, (float)Tmp);
		}
	}

	// Raw params: exact name-value pairs for precise RIP targeting.
	struct FRawParam { FString Name; float Value; };
	TArray<FRawParam> RawParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("raw_params"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!V->TryGetObject(Obj) || !Obj) continue;
				FString Name; double Val = 0;
				(*Obj)->TryGetStringField(TEXT("name"),  Name);
				(*Obj)->TryGetNumberField(TEXT("value"), Val);
				if (!Name.IsEmpty()) RawParams.Add({ Name, (float)Val });
			}
		}
	}

	AsyncTask(ENamedThreads::GameThread,
		[Callback, AssetPath, EmitterIndex, bListOnly, bRemoveEmitter, NamedParams, RawParams]()
	{
		// --- Load system -------------------------------------------------------
		FString _, AssetName;
		AssetPath.Split(TEXT("/"), &_, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(
			nullptr, *(AssetPath + TEXT(".") + AssetName));
		if (!System)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Niagara system not found at '%s'"), *AssetPath)));
			return;
		}

		// --- Remove emitter if requested ---------------------------------------
		if (bRemoveEmitter)
		{
			const TArray<FNiagaraEmitterHandle>& RemHandles = System->GetEmitterHandles();
			if (!RemHandles.IsValidIndex(EmitterIndex))
			{
				Callback(JsonError(404, FString::Printf(
					TEXT("Cannot remove emitter_index %d — system has %d emitter(s)."),
					EmitterIndex, RemHandles.Num())));
				return;
			}

			const FNiagaraEmitterHandle& HandleToRemove = RemHandles[EmitterIndex];
			const FName EmitterName = HandleToRemove.GetName();
			System->RemoveEmitterHandle(HandleToRemove);
			System->RequestCompile(true);
			System->WaitForCompilationComplete();
			System->MarkPackageDirty();

			TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
			Resp->SetBoolField  (TEXT("success"),       true);
			Resp->SetStringField(TEXT("asset_path"),    AssetPath);
			Resp->SetStringField(TEXT("removed_emitter"), EmitterName.ToString());
			Resp->SetNumberField(TEXT("emitter_index"), EmitterIndex);
			Resp->SetNumberField(TEXT("remaining_emitters"), System->GetEmitterHandles().Num());

			FString RespBody;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
			FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
			Callback(JsonOk(RespBody));
			return;
		}

		// --- Get emitter at requested index ------------------------------------
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		if (!Handles.IsValidIndex(EmitterIndex))
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("emitter_index %d is out of range (system has %d emitter(s)). "
				     "Use list_only=true on emitter_index 0 to inspect the system first."),
				EmitterIndex, Handles.Num())));
			return;
		}

		const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];

		// UE5.3+ versioned emitter API: GetInstance() → FVersionedNiagaraEmitter
		FVersionedNiagaraEmitter VI = Handle.GetInstance();
		UNiagaraEmitter* Emitter = VI.Emitter.Get();
		if (!Emitter)
		{
			Callback(JsonError(500,
				TEXT("Emitter instance is null — the handle may be stale. "
				     "Try configure_niagara_system with reset_emitters=true first.")));
			return;
		}

		FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
		if (!Data)
		{
			Callback(JsonError(500, TEXT("GetLatestEmitterData() returned null")));
			return;
		}

		// --- Collect all scripts from the emitter ------------------------------
		// Each emitter has up to 4 script slots: Spawn, Update, EmitterSpawn, EmitterUpdate.
		// Rapid Iteration Parameters (RIPs) are stored per-script.
		TArray<TPair<FString, UNiagaraScript*>> Scripts;
		auto AddScript = [&](const TCHAR* Label, TObjectPtr<UNiagaraScript> ScriptProp)
		{
			if (ScriptProp.Get())
				Scripts.Add({ Label, ScriptProp.Get() });
		};
		AddScript(TEXT("SpawnScript"),        Data->SpawnScriptProps.Script);
		AddScript(TEXT("UpdateScript"),       Data->UpdateScriptProps.Script);
		AddScript(TEXT("EmitterSpawnScript"), Data->EmitterSpawnScriptProps.Script);
		AddScript(TEXT("EmitterUpdateScript"),Data->EmitterUpdateScriptProps.Script);

		// --- Enumerate all RIPs ------------------------------------------------
		struct FRIPEntry
		{
			FString          FullName;
			FString          TypeName;
			FString          ScriptLabel;
			UNiagaraScript*  Script  = nullptr;
			FNiagaraVariable Var;
		};
		TArray<FRIPEntry> AllRIPs;

		for (auto& [Label, Script] : Scripts)
		{
			TArray<FNiagaraVariable> Params;
			Script->RapidIterationParameters.GetParameters(Params);
			for (const FNiagaraVariable& Var : Params)
			{
				FRIPEntry E;
				E.FullName    = Var.GetName().ToString();
				E.TypeName    = Var.GetType().GetName();
				E.ScriptLabel = Label;
				E.Script      = Script;
				E.Var         = Var;
				AllRIPs.Add(MoveTemp(E));
			}
		}

		// --- List-only mode: return all RIPs without touching anything ---------
		if (bListOnly)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset_path"),    AssetPath);
			Result->SetNumberField(TEXT("emitter_index"), EmitterIndex);
			Result->SetNumberField(TEXT("rip_count"),     AllRIPs.Num());
			Result->SetStringField(TEXT("hint"),
				TEXT("Pass these names in raw_params to set them precisely. "
				     "Named shorthand params (spawn_count, lifetime_min, etc.) use "
				     "keyword substring matching against these names."));

			TArray<TSharedPtr<FJsonValue>> RIPArr;
			for (const FRIPEntry& E : AllRIPs)
			{
				TSharedPtr<FJsonObject> RIPObj = MakeShared<FJsonObject>();
				RIPObj->SetStringField(TEXT("name"),   E.FullName);
				RIPObj->SetStringField(TEXT("type"),   E.TypeName);
				RIPObj->SetStringField(TEXT("script"), E.ScriptLabel);
				RIPArr.Add(MakeShared<FJsonValueObject>(RIPObj));
			}
			Result->SetArrayField(TEXT("rapid_iteration_params"), RIPArr);

			FString BodyStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
			FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
			Callback(JsonOk(BodyStr));
			return;
		}

		// --- Set mode: apply parameters ----------------------------------------
		TArray<FString> ParamsSet;
		TArray<FString> ParamsNotFound;

		// Type-safe setters — return true on success
		auto SetFloat = [](const FRIPEntry& E, float Value) -> bool
		{
			if (E.Var.GetType() == FNiagaraTypeDefinition::GetFloatDef())
				return E.Script->RapidIterationParameters.SetParameterValue(Value, E.Var, false);
			if (E.Var.GetType() == FNiagaraTypeDefinition::GetIntDef())
				return E.Script->RapidIterationParameters.SetParameterValue((int32)Value, E.Var, false);
			return false;
		};

		auto SetColor = [](const FRIPEntry& E, FLinearColor Color) -> bool
		{
			if (E.Var.GetType() == FNiagaraTypeDefinition::GetColorDef())
				return E.Script->RapidIterationParameters.SetParameterValue(Color, E.Var, false);
			return false;
		};

		// Keyword table: each named shorthand param → ordered substring keywords to match against.
		// The first RIP whose full name (lowercased) contains any keyword wins.
		struct FRule
		{
			FString          SemanticName;  // key into NamedParams
			TArray<FString>  Keywords;
			bool             bIsColor  = false;
		};

		const TArray<FRule> Rules =
		{
			{ TEXT("spawn_count"),  { TEXT("spawncount"), TEXT("spawnburstcount"), TEXT("burstcount") } },
			{ TEXT("lifetime_min"), { TEXT("lifetimemin"), TEXT("lifetime.min"), TEXT("lifetime.minimum"), TEXT("minlifetime") } },
			{ TEXT("lifetime_max"), { TEXT("lifetimemax"), TEXT("lifetime.max"), TEXT("lifetime.maximum"), TEXT("maxlifetime") } },
			{ TEXT("sprite_size"),  { TEXT("spritesizemin"), TEXT("spritesizemax"), TEXT("spritesize"), TEXT("uniformsprite") } },
			{ TEXT("velocity_min"), { TEXT("speedmin"), TEXT("velocitymin"), TEXT("speed.minimum"), TEXT("minspeed") } },
			{ TEXT("velocity_max"), { TEXT("speedmax"), TEXT("velocitymax"), TEXT("speed.maximum"), TEXT("maxspeed"), TEXT("speed") } },
			{ TEXT("gravity_z"),    { TEXT("gravityz"), TEXT("gravity.z"), TEXT("gravityacceleration") } },
			{ TEXT("color"),        { TEXT("color"), TEXT("colour"), TEXT("startcolor"), TEXT("particlecolor") }, true },
		};

		const float* PCR = NamedParams.Find(TEXT("color_r"));
		const float* PCG = NamedParams.Find(TEXT("color_g"));
		const float* PCB = NamedParams.Find(TEXT("color_b"));
		const bool   bHasColor = PCR && PCG && PCB;
		const FLinearColor TargetColor = bHasColor
			? FLinearColor(*PCR, *PCG, *PCB, 1.0f) : FLinearColor::White;

		for (const FRule& Rule : Rules)
		{
			// Color is special: only apply if all three channels were provided
			if (Rule.bIsColor && !bHasColor) continue;

			const float* pVal = Rule.bIsColor ? nullptr : NamedParams.Find(Rule.SemanticName);
			if (!Rule.bIsColor && !pVal) continue;  // not provided — skip
			const float FloatVal = pVal ? *pVal : 0.f;

			bool bMatched = false;
			for (const FRIPEntry& E : AllRIPs)
			{
				const FString Lower = E.FullName.ToLower();
				for (const FString& Kw : Rule.Keywords)
				{
					if (!Lower.Contains(Kw)) continue;

					bool bSet = Rule.bIsColor ? SetColor(E, TargetColor) : SetFloat(E, FloatVal);
					if (bSet)
					{
						if (Rule.bIsColor)
							ParamsSet.Add(FString::Printf(TEXT("%s = (%.3f,%.3f,%.3f,1)"),
								*E.FullName, TargetColor.R, TargetColor.G, TargetColor.B));
						else
							ParamsSet.Add(FString::Printf(TEXT("%s = %.3f"), *E.FullName, FloatVal));
						bMatched = true;
					}
					break;
				}
				if (bMatched) break;
			}
			if (!bMatched)
				ParamsNotFound.Add(FString::Printf(TEXT("%s (keywords: %s)"),
					*Rule.SemanticName, *FString::Join(Rule.Keywords, TEXT("|"))));
		}

		// --- Raw params: exact name match (case-insensitive) -------------------
		for (const FRawParam& RP : RawParams)
		{
			bool bFound = false;
			for (const FRIPEntry& E : AllRIPs)
			{
				if (!E.FullName.Equals(RP.Name, ESearchCase::IgnoreCase)) continue;
				if (SetFloat(E, RP.Value))
				{
					ParamsSet.Add(FString::Printf(TEXT("%s = %.3f (raw)"), *E.FullName, RP.Value));
					bFound = true;
				}
				else
				{
					ParamsNotFound.Add(FString::Printf(
						TEXT("%s (raw, found but type '%s' is not float/int)"), *RP.Name, *E.TypeName));
					bFound = true; // found but can't set
				}
				break;
			}
			if (!bFound)
				ParamsNotFound.Add(FString::Printf(TEXT("%s (raw, no matching RIP found)"), *RP.Name));
		}

		// --- Compile if anything changed ---------------------------------------
		if (ParamsSet.Num() > 0)
		{
			System->RequestCompile(false);
			System->MarkPackageDirty();
		}

		// --- Build response ---------------------------------------------------
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),       true);
		Result->SetStringField(TEXT("asset_path"),    AssetPath);
		Result->SetNumberField(TEXT("emitter_index"), EmitterIndex);
		Result->SetNumberField(TEXT("rip_count"),     AllRIPs.Num());
		Result->SetBoolField  (TEXT("compiled"),      ParamsSet.Num() > 0);

		auto StringArray = [](const TArray<FString>& In)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FString& S : In) Out.Add(MakeShared<FJsonValueString>(S));
			return Out;
		};
		Result->SetArrayField(TEXT("params_set"),       StringArray(ParamsSet));
		Result->SetArrayField(TEXT("params_not_found"), StringArray(ParamsNotFound));

		// Always include all RIP names so the caller can diagnose mismatches
		TArray<TSharedPtr<FJsonValue>> AllArr;
		for (const FRIPEntry& E : AllRIPs)
			AllArr.Add(MakeShared<FJsonValueString>(
				E.FullName + TEXT(" [") + E.TypeName + TEXT("] (") + E.ScriptLabel + TEXT(")")));
		Result->SetArrayField(TEXT("all_rips"), AllArr);

		if (ParamsNotFound.Num() > 0)
			Result->SetStringField(TEXT("hint"),
				TEXT("Use list_only=true to see all RIP names, or use raw_params with exact names."));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}
