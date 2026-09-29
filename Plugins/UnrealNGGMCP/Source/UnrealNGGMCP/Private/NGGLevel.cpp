// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGLevel.cpp
//
// Implementation of the level and actor endpoints:
//   /levels/{create,open}
//   /actors/{list,spawn,update,delete}
//   /levels/set_world_settings, /levels/set_environment
//   /levels/spawn_post_process_volume
//
// Everything here touches the editor world, so it runs on the Game Thread like
// the rest of the bridge, and create_level in particular has to wait out an
// in-flight GC before it may tear the current world down.

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
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
// ---- World / actors ---------------------------------------------------------
#include "EngineUtils.h"          // TActorIterator
#include "Selection.h"            // USelection — full definition for DeselectAll/Deselect
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"
#include "Engine/LevelStreaming.h"
#include "LevelEditor.h"
// ULevelEditorSubsystem::NewLevel — the supported create-a-level entry point
// used by /editor/create_level.
#include "LevelEditorSubsystem.h"
// IsGarbageCollecting() — create_level waits it out before tearing down a world.
#include "UObject/GarbageCollection.h"
#include "Engine/SCS_Node.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
// ---- Level environment actors ----------------------------------------------
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/PropertyIterator.h"

// ============================================================================
// Cross-TU helpers (defined in NGGBlueprintAssets.cpp)
// ============================================================================
bool TrySetMeshPropertyViaSetter(
	UObject* Target, const FName& PropFName, const FString& ValueStr,
	FString& OutError, bool& bOutSucceeded);


// ============================================================================
// Handler: POST /editor/set_world_settings
// ============================================================================

bool FNGGHttpServer::HandleSetWorldSettings(
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

	FString GameModeClassPath, PlayerControllerClassPath, DefaultPawnClassPath;
	Body->TryGetStringField(TEXT("game_mode_class"),         GameModeClassPath);
	Body->TryGetStringField(TEXT("player_controller_class"), PlayerControllerClassPath);
	Body->TryGetStringField(TEXT("default_pawn_class"),      DefaultPawnClassPath);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, GameModeClassPath, PlayerControllerClassPath, DefaultPawnClassPath]()
	{
		if (!GEditor)
		{
			Callback(JsonError(500, TEXT("GEditor is null — is this running inside the Editor?")));
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world context available")));
			return;
		}

		AWorldSettings* WS = World->GetWorldSettings();
		if (!WS)
		{
			Callback(JsonError(500, TEXT("World has no WorldSettings actor")));
			return;
		}

		// Helper: resolve a UClass from a /Script/ or /Game/ path.
		// For /Script/ paths the class is already compiled — use FindObject.
		// For /Game/ paths load the Blueprint and take its GeneratedClass.
		auto LoadClassFromPath = [](const FString& Path) -> UClass*
		{
			if (Path.IsEmpty()) return nullptr;

			if (Path.StartsWith(TEXT("/Script/")))
			{
				// e.g. /Script/YourProject.YourGameModeClass
				UClass* Cls = FindObject<UClass>(nullptr, *Path);
				if (!Cls)
				{
					// StaticLoadClass as fallback
					Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *Path);
				}
				return Cls;
			}

			// Blueprint class — try _C suffix first (already resolved class path)
			FString ClassPath = Path.EndsWith(TEXT("_C")) ? Path : Path + TEXT("_C");
			UClass* Cls = LoadObject<UClass>(nullptr, *ClassPath);
			if (Cls) return Cls;

			// Try loading the Blueprint asset and getting its GeneratedClass
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
			if (BP && BP->GeneratedClass)
			{
				return BP->GeneratedClass;
			}
			return nullptr;
		};

		FString AppliedGameMode, AppliedDefaultPawn, AppliedPlayerController;

		// ---- Game Mode -------------------------------------------------------
		if (!GameModeClassPath.IsEmpty())
		{
			UClass* GMClass = LoadClassFromPath(GameModeClassPath);
			if (GMClass)
			{
				WS->DefaultGameMode = TSubclassOf<AGameModeBase>(GMClass);
				AppliedGameMode = GMClass->GetName();
			}
			else
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("HandleSetWorldSettings: could not resolve game_mode_class '%s'"),
					*GameModeClassPath);
			}
		}

		// ---- Pawn Class (set on GameMode CDO) --------------------------------
		if (!DefaultPawnClassPath.IsEmpty())
		{
			UClass* PawnClass = LoadClassFromPath(DefaultPawnClassPath);
			if (PawnClass && WS->DefaultGameMode)
			{
				AGameModeBase* GMCDO = Cast<AGameModeBase>(WS->DefaultGameMode->GetDefaultObject());
				if (GMCDO)
				{
					GMCDO->DefaultPawnClass = PawnClass;
					AppliedDefaultPawn = PawnClass->GetName();
				}
			}
			else if (PawnClass)
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("HandleSetWorldSettings: default_pawn_class resolved but GameMode not set — skipped"));
			}
			else
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("HandleSetWorldSettings: could not resolve default_pawn_class '%s'"),
					*DefaultPawnClassPath);
			}
		}

		// ---- Player Controller Class (set on GameMode CDO) -------------------
		if (!PlayerControllerClassPath.IsEmpty())
		{
			UClass* PCClass = LoadClassFromPath(PlayerControllerClassPath);
			if (PCClass && WS->DefaultGameMode)
			{
				AGameModeBase* GMCDO = Cast<AGameModeBase>(WS->DefaultGameMode->GetDefaultObject());
				if (GMCDO)
				{
					GMCDO->PlayerControllerClass = PCClass;
					AppliedPlayerController = PCClass->GetName();
				}
			}
			else if (PCClass)
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("HandleSetWorldSettings: player_controller_class resolved but GameMode not set — skipped"));
			}
			else
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("HandleSetWorldSettings: could not resolve player_controller_class '%s'"),
					*PlayerControllerClassPath);
			}
		}

		World->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),           true);
		Result->SetStringField(TEXT("world"),             World->GetName());
		Result->SetStringField(TEXT("game_mode"),         AppliedGameMode);
		Result->SetStringField(TEXT("default_pawn"),      AppliedDefaultPawn);
		Result->SetStringField(TEXT("player_controller"), AppliedPlayerController);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/spawn_actor_in_level
// ============================================================================

bool FNGGHttpServer::HandleSpawnActorInLevel(
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

	FString ActorClassName; Body->TryGetStringField(TEXT("actor_class"), ActorClassName);
	if (ActorClassName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("actor_class is required")));
		return true;
	}

	// Parse location
	double X = 0.0, Y = 0.0, Z = 0.0;
	{
		const TSharedPtr<FJsonObject>* LocObj;
		if (Body->TryGetObjectField(TEXT("location"), LocObj))
		{
			(*LocObj)->TryGetNumberField(TEXT("x"), X);
			(*LocObj)->TryGetNumberField(TEXT("y"), Y);
			(*LocObj)->TryGetNumberField(TEXT("z"), Z);
		}
	}

	// Parse rotation
	double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
	{
		const TSharedPtr<FJsonObject>* RotObj;
		if (Body->TryGetObjectField(TEXT("rotation"), RotObj))
		{
			(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*RotObj)->TryGetNumberField(TEXT("yaw"),   Yaw);
			(*RotObj)->TryGetNumberField(TEXT("roll"),  Roll);
		}
	}

	FString Label;
	Body->TryGetStringField(TEXT("label"), Label);

	FString StaticMeshPath;
	Body->TryGetStringField(TEXT("static_mesh"), StaticMeshPath);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, ActorClassName, X, Y, Z, Pitch, Yaw, Roll, Label, StaticMeshPath]()
	{
		if (!GEditor)
		{
			Callback(JsonError(500, TEXT("GEditor is null")));
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world context")));
			return;
		}

		// ---------- 0. Duplicate label check ----------------------------------
		if (!Label.IsEmpty())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel() == Label)
				{
					Callback(JsonError(409, FString::Printf(
						TEXT("Actor with label '%s' already exists in the level"), *Label)));
					return;
				}
			}
		}

		// ---------- 1. Resolve actor UClass -----------------------------------
		UClass* ActorClass = nullptr;

		// UClass::GetName() returns names WITHOUT the A/U prefix.
		FString AStrippedA = ActorClassName;
		if (AStrippedA.StartsWith(TEXT("A"))) AStrippedA = AStrippedA.Mid(1);
		FString AStrippedU = ActorClassName;
		if (AStrippedU.StartsWith(TEXT("U"))) AStrippedU = AStrippedU.Mid(1);

		TArray<FString> CandidateNames = { AStrippedA, AStrippedU, ActorClassName };
		for (const FString& Candidate : CandidateNames)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == Candidate && It->IsChildOf(AActor::StaticClass()))
				{
					ActorClass = *It;
					break;
				}
			}
			if (ActorClass) break;
		}

		// Try loading as a Blueprint content path if class still not found
		if (!ActorClass && ActorClassName.StartsWith(TEXT("/")))
		{
			// Try _C (generated class) first
			FString ClassPath = ActorClassName.EndsWith(TEXT("_C"))
				? ActorClassName
				: ActorClassName + TEXT("_C");
			ActorClass = LoadObject<UClass>(nullptr, *ClassPath);
			if (!ActorClass)
			{
				UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ActorClassName);
				if (BP && BP->GeneratedClass && BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
				{
					ActorClass = BP->GeneratedClass;
				}
			}
		}

		if (!ActorClass)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Could not find actor UClass for '%s'"), *ActorClassName)));
			return;
		}

		// ---------- 2. Spawn --------------------------------------------------
		FTransform SpawnTransform(
			FRotator(Pitch, Yaw, Roll),
			FVector(X, Y, Z));

		AActor* Spawned = World->SpawnActor<AActor>(ActorClass, SpawnTransform);
		if (!Spawned)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("SpawnActor failed for class '%s'"), *ActorClass->GetName())));
			return;
		}

		// ---------- 3. Label & select in editor --------------------------------
		if (!Label.IsEmpty())
		{
			Spawned->SetActorLabel(*Label);
		}

		// ---------- 4. Assign static mesh if provided -------------------------
		if (!StaticMeshPath.IsEmpty())
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *StaticMeshPath);
			if (Mesh)
			{
				UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(
					Spawned->FindComponentByClass(UStaticMeshComponent::StaticClass()));
				if (SMC)
				{
					SMC->SetStaticMesh(Mesh);
					Spawned->ReregisterAllComponents();
				}
			}
			else
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("spawn_actor: static_mesh '%s' could not be loaded — actor spawned without mesh"),
					*StaticMeshPath);
			}
		}

		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Spawned, true, true);

		World->GetOutermost()->MarkPackageDirty();

		// ---------- 5. Respond ------------------------------------------------
		TSharedPtr<FJsonObject> LocResult = MakeShared<FJsonObject>();
		LocResult->SetNumberField(TEXT("x"), X);
		LocResult->SetNumberField(TEXT("y"), Y);
		LocResult->SetNumberField(TEXT("z"), Z);

		TSharedPtr<FJsonObject> RotResult = MakeShared<FJsonObject>();
		RotResult->SetNumberField(TEXT("pitch"), Pitch);
		RotResult->SetNumberField(TEXT("yaw"),   Yaw);
		RotResult->SetNumberField(TEXT("roll"),  Roll);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),    true);
		Result->SetStringField(TEXT("actor_name"), Spawned->GetActorLabel());
		Result->SetStringField(TEXT("class"),      ActorClass->GetName());
		Result->SetObjectField(TEXT("location"),   LocResult);
		Result->SetObjectField(TEXT("rotation"),   RotResult);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_level
// Creates a new empty persistent level, saves it, and opens it in the editor.
// Body: { "level_path": "/Game/Maps/L_Persistent_Exercise" }
// ============================================================================

bool FNGGHttpServer::HandleCreateLevel(
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

	FString LevelPath; Body->TryGetStringField(TEXT("level_path"), LevelPath);
	if (LevelPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("level_path is required")));
		return true;
	}

	bool bPartitioned = false;
	Body->TryGetBoolField(TEXT("partitioned"), bPartitioned);

	// Creating a level destroys the world that is currently open, and that is
	// only safe from a clean point in the frame.
	//
	// This used to run straight from AsyncTask(GameThread). Game-thread tasks
	// are drained from several places, including from inside FlushAsyncLoading,
	// so the world teardown could start while the loader was mid-flight. That
	// reliably killed the editor with
	//   Assertion failed: !LevelList.Contains(TickTaskLevel)  (TickTaskManager.cpp)
	// from ULevel::~ULevel -> FTickTaskManager::FreeTickTaskLevel — the level was
	// still registered for ticking while it was being destroyed. Reproduced from
	// a cold-started editor with `FlushAsyncLoading(477): 1 QueuedPackages`
	// logged immediately before the teardown.
	//
	// The core ticker fires from a known point in FEngineLoop::Tick instead, so
	// the work never runs nested inside another engine operation. Waiting for GC
	// to finish and flushing the loader close the remaining two windows.
	auto Attempts = MakeShared<int32, ESPMode::ThreadSafe>(0);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Callback, LevelPath, bPartitioned, Attempts](float) -> bool
	{
		// Destroying a world from inside garbage collection is never valid.
		// Roughly 10s at 60fps, after which we give up rather than hang the
		// request forever — the caller gets an error it can act on.
		if (IsGarbageCollecting())
		{
			if (++(*Attempts) < 600)
			{
				return true;   // keep ticking
			}
			Callback(JsonError(503,
				TEXT("create_level: the editor stayed in garbage collection for 10s; try again")));
			return false;
		}

		// Check if the map already exists on disk.
		FString PackageFilename;
		if (FPackageName::DoesPackageExist(LevelPath, &PackageFilename))
		{
			TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
			Resp->SetBoolField  (TEXT("success"),         true);
			Resp->SetBoolField  (TEXT("already_existed"), true);
			Resp->SetStringField(TEXT("level_path"),      LevelPath);
			Resp->SetStringField(TEXT("file_path"),       PackageFilename);

			FString RespBody;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
			FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
			Callback(JsonOk(RespBody));
			return false;
		}

		// Settle any in-flight package loads before the current world goes away.
		FlushAsyncLoading();

		ULevelEditorSubsystem* LevelEditorSubsystem =
			GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
		if (!LevelEditorSubsystem)
		{
			Callback(JsonError(500, TEXT("LevelEditorSubsystem unavailable")));
			return false;
		}

		// NewLevel() creates, names and saves the asset in one supported call,
		// refuses to run during PIE, and suppresses the modal dialogs that would
		// otherwise block a headless request. It also knows how to make a
		// World Partition map, which the previous hand-rolled
		// NewMap + Rename + SaveLevel sequence could not do at all.
		const bool bCreated = LevelEditorSubsystem->NewLevel(LevelPath, bPartitioned);

		const FString FilePath = FPackageName::LongPackageNameToFilename(
			LevelPath, FPackageName::GetMapPackageExtension());

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         bCreated);
		Resp->SetBoolField  (TEXT("already_existed"), false);
		Resp->SetBoolField  (TEXT("created"),         bCreated);
		Resp->SetBoolField  (TEXT("partitioned"),     bPartitioned);
		Resp->SetStringField(TEXT("level_path"),      LevelPath);
		Resp->SetStringField(TEXT("file_path"),       FilePath);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		bCreated ? Callback(JsonCreated(RespBody))
		         : Callback(JsonError(500,
		             TEXT("ULevelEditorSubsystem::NewLevel failed — see the editor log for the reason "
		                  "(invalid path, or a PIE session is running)")));
		return false;   // one-shot
	}), 0.0f);

	return true;
}

// ============================================================================
// Handler: POST /editor/open_level
// Opens an existing level in the editor by its content path.
// ============================================================================

bool FNGGHttpServer::HandleOpenLevel(
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

	FString LevelPath; Body->TryGetStringField(TEXT("level_path"), LevelPath);
	if (LevelPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("level_path is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, LevelPath]()
	{
		// Verify the map package exists on disk
		FString PackageFilename;
		if (!FPackageName::DoesPackageExist(LevelPath, &PackageFilename))
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Level not found: %s"), *LevelPath)));
			return;
		}

		// Convert to file path and load the map in the editor
		FString MapFilePath = FPackageName::LongPackageNameToFilename(
			LevelPath, FPackageName::GetMapPackageExtension());

		bool bLoaded = FEditorFileUtils::LoadMap(MapFilePath, /*bLoadAsTemplate=*/false, /*bShowProgress=*/true);

		if (!bLoaded)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("Failed to open level: %s"), *LevelPath)));
			return;
		}

		// Return info about the opened level
		UWorld* World = GEditor->GetEditorWorldContext().World();
		FString WorldName = World ? World->GetName() : TEXT("unknown");

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),    true);
		Resp->SetStringField(TEXT("level_path"), LevelPath);
		Resp->SetStringField(TEXT("world"),      WorldName);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: GET /editor/list_actors
// Lists all actors in the current editor world. Optional ?class_filter=ClassName
// ============================================================================

bool FNGGHttpServer::HandleListActors(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	FString ClassFilter = GetQueryParam(Req, TEXT("class_filter"));

	// Fallback: read from JSON body (needed when called from batch mode).
	if (ClassFilter.IsEmpty() && Req.Body.Num() > 0)
	{
		TSharedPtr<FJsonObject> BodyObj;
		FString Err;
		if (ParseJsonBody(Req, BodyObj, Err) && BodyObj.IsValid())
		{
			BodyObj->TryGetStringField(TEXT("class_filter"), ClassFilter);
		}
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, ClassFilter]()
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world available")));
			return;
		}

		TArray<TSharedPtr<FJsonValue>> ActorArray;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor)) continue;

			// Apply optional class filter
			if (!ClassFilter.IsEmpty())
			{
				FString ActorClassName = Actor->GetClass()->GetName();
				if (!ActorClassName.Contains(ClassFilter)) continue;
			}

			TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
			ActorObj->SetStringField(TEXT("name"),  Actor->GetActorLabel());
			ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

			FVector Loc = Actor->GetActorLocation();
			FRotator Rot = Actor->GetActorRotation();

			TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), Loc.X);
			LocObj->SetNumberField(TEXT("y"), Loc.Y);
			LocObj->SetNumberField(TEXT("z"), Loc.Z);

			TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
			RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
			RotObj->SetNumberField(TEXT("yaw"),   Rot.Yaw);
			RotObj->SetNumberField(TEXT("roll"),  Rot.Roll);

			ActorObj->SetObjectField(TEXT("location"), LocObj);
			ActorObj->SetObjectField(TEXT("rotation"), RotObj);
			ActorObj->SetStringField(TEXT("path"),     Actor->GetPathName());

			ActorArray.Add(MakeShared<FJsonValueObject>(ActorObj));
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"), true);
		Resp->SetStringField(TEXT("world"),   World->GetName());
		Resp->SetNumberField(TEXT("count"),   static_cast<double>(ActorArray.Num()));
		Resp->SetArrayField (TEXT("actors"),  ActorArray);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/update_actor
// Updates an existing actor's transform and/or label.
// Body: { "actor_label": "...", "location": {x,y,z}, "rotation": {pitch,yaw,roll}, "new_label": "..." }
// ============================================================================

bool FNGGHttpServer::HandleUpdateActor(
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

	FString ActorLabel; Body->TryGetStringField(TEXT("actor_label"), ActorLabel);
	if (ActorLabel.IsEmpty())
	{
		Callback(JsonError(400, TEXT("actor_label is required")));
		return true;
	}

	// Capture optional fields
	TSharedPtr<FJsonObject> LocationObj;
	if (Body->HasTypedField<EJson::Object>(TEXT("location")))
		LocationObj = Body->GetObjectField(TEXT("location"));

	TSharedPtr<FJsonObject> RotationObj;
	if (Body->HasTypedField<EJson::Object>(TEXT("rotation")))
		RotationObj = Body->GetObjectField(TEXT("rotation"));

	FString NewLabel;
	Body->TryGetStringField(TEXT("new_label"), NewLabel);

	TArray<TSharedPtr<FJsonValue>> CompPropsJson;
	if (Body->HasField(TEXT("component_properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* CompPropsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("component_properties"), CompPropsPtr) || !CompPropsPtr)
		{
			Callback(JsonError(400, TEXT("'component_properties' must be an array")));
			return true;
		}
		CompPropsJson = *CompPropsPtr;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, ActorLabel, LocationObj, RotationObj, NewLabel, CompPropsJson]()
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world available")));
			return;
		}

		// Find actor by label
		AActor* Found = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
			{
				Found = *It;
				break;
			}
		}

		if (!Found)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Actor with label '%s' not found"), *ActorLabel)));
			return;
		}

		// Apply location. TryGetNumberField tolerates missing/wrong-typed
		// sub-fields (defaulting to 0) instead of crashing in GetNumberField.
		if (LocationObj.IsValid())
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			LocationObj->TryGetNumberField(TEXT("x"), X);
			LocationObj->TryGetNumberField(TEXT("y"), Y);
			LocationObj->TryGetNumberField(TEXT("z"), Z);
			Found->SetActorLocation(FVector(X, Y, Z));
		}

		// Apply rotation
		if (RotationObj.IsValid())
		{
			double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
			RotationObj->TryGetNumberField(TEXT("pitch"), Pitch);
			RotationObj->TryGetNumberField(TEXT("yaw"),   Yaw);
			RotationObj->TryGetNumberField(TEXT("roll"),  Roll);
			Found->SetActorRotation(FRotator(Pitch, Yaw, Roll));
		}

		// Rename label
		if (!NewLabel.IsEmpty())
		{
			Found->SetActorLabel(NewLabel);
		}

		// Apply component properties (e.g. set StaticMesh on a StaticMeshActor)
		if (CompPropsJson.Num() > 0)
		{
			TArray<UActorComponent*> Components;
			Found->GetComponents(Components);

			for (const TSharedPtr<FJsonValue>& Entry : CompPropsJson)
			{
				const TSharedPtr<FJsonObject>* EntryObj;
				if (!Entry->TryGetObject(EntryObj)) continue;

				FString ComponentName, PropertyName, ValueStr;
				(*EntryObj)->TryGetStringField(TEXT("component_name"), ComponentName);
				(*EntryObj)->TryGetStringField(TEXT("property_name"),  PropertyName);
				(*EntryObj)->TryGetStringField(TEXT("value"),          ValueStr);
				if (PropertyName.IsEmpty()) continue;

				// Find component by name or class name; fall back to actor if empty
				UObject* Target = nullptr;
				if (!ComponentName.IsEmpty())
				{
					for (UActorComponent* Comp : Components)
					{
						if (Comp && (Comp->GetName() == ComponentName
							|| Comp->GetClass()->GetName() == ComponentName))
						{
							Target = Comp;
							break;
						}
					}
				}
				if (!Target) Target = Found;

				FProperty* Prop = FindFProperty<FProperty>(Target->GetClass(), *PropertyName);
				if (Prop)
				{
					FString MeshErr; bool bMeshOk = false;
					if (TrySetMeshPropertyViaSetter(Target, FName(*PropertyName), ValueStr, MeshErr, bMeshOk))
					{
						// handled (success or logged error); skip raw import
					}
					else
					{
						Prop->ImportText_Direct(
							*ValueStr,
							Prop->ContainerPtrToValuePtr<void>(Target),
							Target,
							PPF_None);
					}
				}
			}

			Found->ReregisterAllComponents();
		}

		Found->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"), true);
		Resp->SetStringField(TEXT("actor_label"), Found->GetActorLabel());
		Resp->SetStringField(TEXT("class"),       Found->GetClass()->GetName());

		FVector Loc = Found->GetActorLocation();
		TSharedPtr<FJsonObject> LocResp = MakeShared<FJsonObject>();
		LocResp->SetNumberField(TEXT("x"), Loc.X);
		LocResp->SetNumberField(TEXT("y"), Loc.Y);
		LocResp->SetNumberField(TEXT("z"), Loc.Z);
		Resp->SetObjectField(TEXT("location"), LocResp);

		FRotator Rot = Found->GetActorRotation();
		TSharedPtr<FJsonObject> RotResp = MakeShared<FJsonObject>();
		RotResp->SetNumberField(TEXT("pitch"), Rot.Pitch);
		RotResp->SetNumberField(TEXT("yaw"),   Rot.Yaw);
		RotResp->SetNumberField(TEXT("roll"),  Rot.Roll);
		Resp->SetObjectField(TEXT("rotation"), RotResp);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/delete_actor
// Removes an actor from the current level by label.
// Body: { "actor_label": "TeleportAnchor_Kitchen_Center" }
// ============================================================================

bool FNGGHttpServer::HandleDeleteActor(
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

	FString ActorLabel; Body->TryGetStringField(TEXT("actor_label"), ActorLabel);
	if (ActorLabel.IsEmpty())
	{
		Callback(JsonError(400, TEXT("actor_label is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, ActorLabel]()
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world available")));
			return;
		}

		AActor* Found = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
			{
				Found = *It;
				break;
			}
		}

		if (!Found)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Actor with label '%s' not found"), *ActorLabel)));
			return;
		}

		FString ClassName = Found->GetClass()->GetName();

		// Deselect before destroy. If the actor is in the editor selection
		// (e.g. spawn_actor selects the new actor), EditorDestroyActor leaves
		// a dangling TypedElementHandle in USelection → next viewport tick
		// crashes in GetSelectedComponentCount on a null element.
		if (GEditor)
		{
			if (USelection* Sel = GEditor->GetSelectedActors())
			{
				Sel->Deselect(Found);
			}
			GEditor->NoteSelectionChange(/*bNotify*/ false);
		}

		bool bDestroyed = World->EditorDestroyActor(Found, true);

		// Defensive: even if the actor wasn't selected above (e.g. deleted
		// by a different handler after spawn selected it), make absolutely
		// sure no tick-time code holds onto stale element handles.
		if (GEditor && bDestroyed)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true, /*WarnAboutTools*/ false);
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),   bDestroyed);
		Resp->SetStringField(TEXT("deleted"),   ActorLabel);
		Resp->SetStringField(TEXT("class"),     ClassName);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/spawn_post_process_volume
//
// Spawns (or reuses) an APostProcessVolume in the current editor world,
// configures it as unbounded, and adds the given UMaterialInterface to the
// PostProcessSettings.WeightedBlendables array.
//
// Body:
//   {
//     "material_path": "/Game/Materials/M_EnemyHighlight",
//     "actor_label": "PP_EnemyHighlight",   // optional, default "PP_<MaterialName>"
//     "unbounded": true,                     // optional, default true
//     "priority": 0                          // optional, default 0
//   }
//
// If an APostProcessVolume already exists with the supplied label, it is
// reused (its blendable array is updated in place rather than duplicated).
// ============================================================================

bool FNGGHttpServer::HandleSpawnPostProcessVolume(
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

	FString MaterialPath; Body->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("material_path is required")));
		return true;
	}

	FString ActorLabel;
	Body->TryGetStringField(TEXT("actor_label"), ActorLabel);

	bool bUnbounded = true;
	Body->TryGetBoolField(TEXT("unbounded"), bUnbounded);

	double PriorityDbl = 0.0;
	Body->TryGetNumberField(TEXT("priority"), PriorityDbl);
	const float Priority = (float)PriorityDbl;

	AsyncTask(ENamedThreads::GameThread, [Callback, MaterialPath, ActorLabel, bUnbounded, Priority]()
	{
		if (!GEditor)
		{
			Callback(JsonError(500, TEXT("GEditor is null")));
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world available")));
			return;
		}

		// Resolve the material — must be a UMaterialInterface (UMaterial or MIC).
		UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Mat)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Material not found at '%s'"), *MaterialPath)));
			return;
		}

		// Default label derived from the material name.
		FString FinalLabel = ActorLabel;
		if (FinalLabel.IsEmpty())
		{
			FinalLabel = FString::Printf(TEXT("PP_%s"), *Mat->GetName());
		}

		// Reuse an existing volume if one exists with the supplied label.
		APostProcessVolume* PPV = nullptr;
		bool bAlreadyExisted = false;
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == FinalLabel)
			{
				PPV = *It;
				bAlreadyExisted = true;
				break;
			}
		}

		if (!PPV)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			PPV = World->SpawnActor<APostProcessVolume>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			if (!PPV)
			{
				Callback(JsonError(500, TEXT("Failed to spawn APostProcessVolume")));
				return;
			}
			PPV->SetActorLabel(*FinalLabel);
		}

		PPV->bUnbound = bUnbounded;
		PPV->Priority = Priority;

		// Add the material to the WeightedBlendables array if it isn't already
		// present. Preserve any existing blendables the user has configured.
		bool bMaterialAlreadyInArray = false;
		for (const FWeightedBlendable& Existing : PPV->Settings.WeightedBlendables.Array)
		{
			if (Existing.Object == Mat)
			{
				bMaterialAlreadyInArray = true;
				break;
			}
		}
		if (!bMaterialAlreadyInArray)
		{
			FWeightedBlendable Blendable(1.0f, Mat);
			PPV->Settings.WeightedBlendables.Array.Add(Blendable);
		}

		PPV->MarkPackageDirty();
		if (UPackage* WorldPkg = World->GetOutermost())
		{
			WorldPkg->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),               true);
		Resp->SetStringField(TEXT("actor_label"),           PPV->GetActorLabel());
		Resp->SetBoolField  (TEXT("already_existed"),       bAlreadyExisted);
		Resp->SetStringField(TEXT("material_path"),         MaterialPath);
		Resp->SetBoolField  (TEXT("material_added"),        !bMaterialAlreadyInArray);
		Resp->SetBoolField  (TEXT("unbounded"),             bUnbounded);
		Resp->SetNumberField(TEXT("priority"),              Priority);
		Resp->SetNumberField(TEXT("blendables_count"),      PPV->Settings.WeightedBlendables.Array.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/set_level_environment
//
// Open a level, add/configure ExponentialHeightFog, DirectionalLight, and
// SkyLight actors for themed environments, then mark dirty for save.
// ============================================================================

bool FNGGHttpServer::HandleSetLevelEnvironment(
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

	FString LevelPath; Body->TryGetStringField(TEXT("level_path"), LevelPath);
	if (LevelPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("level_path is required")));
		return true;
	}

	// Parse fog params
	struct FFogParams
	{
		bool bEnabled = true;
		FLinearColor InscatteringColor = FLinearColor(0.01f, 0.02f, 0.08f, 1.0f);
		float Density = 0.02f;
		float HeightFalloff = 0.2f;
		bool bHasFog = false;
	} FogParams;
	{
		const TSharedPtr<FJsonObject>* FogObj;
		if (Body->TryGetObjectField(TEXT("fog"), FogObj))
		{
			FogParams.bHasFog = true;
			(*FogObj)->TryGetBoolField(TEXT("enabled"), FogParams.bEnabled);

			const TSharedPtr<FJsonObject>* ColorObj;
			if ((*FogObj)->TryGetObjectField(TEXT("inscattering_color"), ColorObj))
			{
				double R = 0, G = 0, B = 0, A = 1;
				(*ColorObj)->TryGetNumberField(TEXT("r"), R);
				(*ColorObj)->TryGetNumberField(TEXT("g"), G);
				(*ColorObj)->TryGetNumberField(TEXT("b"), B);
				(*ColorObj)->TryGetNumberField(TEXT("a"), A);
				FogParams.InscatteringColor = FLinearColor(R, G, B, A);
			}

			double DensityVal = 0;
			if ((*FogObj)->TryGetNumberField(TEXT("density"), DensityVal))
				FogParams.Density = (float)DensityVal;

			double FalloffVal = 0;
			if ((*FogObj)->TryGetNumberField(TEXT("height_falloff"), FalloffVal))
				FogParams.HeightFalloff = (float)FalloffVal;
		}
	}

	// Parse directional light params
	struct FDirLightParams
	{
		float Intensity = 1.0f;
		FLinearColor LightColor = FLinearColor::White;
		bool bHasLight = false;
	} DirLightParams;
	{
		const TSharedPtr<FJsonObject>* LightObj;
		if (Body->TryGetObjectField(TEXT("directional_light"), LightObj))
		{
			DirLightParams.bHasLight = true;

			double IntVal = 0;
			if ((*LightObj)->TryGetNumberField(TEXT("intensity"), IntVal))
				DirLightParams.Intensity = (float)IntVal;

			const TSharedPtr<FJsonObject>* ColorObj;
			if ((*LightObj)->TryGetObjectField(TEXT("light_color"), ColorObj))
			{
				double R = 1, G = 1, B = 1;
				(*ColorObj)->TryGetNumberField(TEXT("r"), R);
				(*ColorObj)->TryGetNumberField(TEXT("g"), G);
				(*ColorObj)->TryGetNumberField(TEXT("b"), B);
				DirLightParams.LightColor = FLinearColor(R, G, B, 1.0f);
			}
		}
	}

	// Parse sky light params
	struct FSkyLightParams
	{
		float Intensity = 1.0f;
		FLinearColor LightColor = FLinearColor::White;
		bool bHasSkyLight = false;
	} SkyLightParams;
	{
		const TSharedPtr<FJsonObject>* SkyObj;
		if (Body->TryGetObjectField(TEXT("sky_light"), SkyObj))
		{
			SkyLightParams.bHasSkyLight = true;

			double IntVal = 0;
			if ((*SkyObj)->TryGetNumberField(TEXT("intensity"), IntVal))
				SkyLightParams.Intensity = (float)IntVal;

			const TSharedPtr<FJsonObject>* ColorObj;
			if ((*SkyObj)->TryGetObjectField(TEXT("light_color"), ColorObj))
			{
				double R = 1, G = 1, B = 1;
				(*ColorObj)->TryGetNumberField(TEXT("r"), R);
				(*ColorObj)->TryGetNumberField(TEXT("g"), G);
				(*ColorObj)->TryGetNumberField(TEXT("b"), B);
				SkyLightParams.LightColor = FLinearColor(R, G, B, 1.0f);
			}
		}
	}

	AsyncTask(ENamedThreads::GameThread,
		[Callback, LevelPath, FogParams, DirLightParams, SkyLightParams]()
	{
		if (!GEditor)
		{
			Callback(JsonError(500, TEXT("GEditor is null")));
			return;
		}

		// 1. Open the level if it's not already the current one
		UWorld* World = GEditor->GetEditorWorldContext().World();
		FString CurrentMapName = World ? World->GetMapName() : TEXT("");
		FString DesiredMapName = FPaths::GetBaseFilename(LevelPath);

		if (CurrentMapName != DesiredMapName)
		{
			FString PackageFilename;
			if (!FPackageName::DoesPackageExist(LevelPath, &PackageFilename))
			{
				Callback(JsonError(404, FString::Printf(
					TEXT("Level not found: %s"), *LevelPath)));
				return;
			}

			FString MapFilePath = FPackageName::LongPackageNameToFilename(
				LevelPath, FPackageName::GetMapPackageExtension());
			bool bLoaded = FEditorFileUtils::LoadMap(MapFilePath, false, true);
			if (!bLoaded)
			{
				Callback(JsonError(500, FString::Printf(
					TEXT("Failed to open level: %s"), *LevelPath)));
				return;
			}

			World = GEditor->GetEditorWorldContext().World();
		}

		if (!World)
		{
			Callback(JsonError(500, TEXT("No editor world available after opening level")));
			return;
		}

		TArray<FString> ActionsLog;

		// 2. Configure ExponentialHeightFog
		if (FogParams.bHasFog)
		{
			AExponentialHeightFog* FogActor = nullptr;
			for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
			{
				FogActor = *It;
				break;
			}

			if (!FogActor)
			{
				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				FogActor = World->SpawnActor<AExponentialHeightFog>(
					AExponentialHeightFog::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
				FogActor->SetActorLabel(TEXT("EnvironmentFog"));
				ActionsLog.Add(TEXT("spawned ExponentialHeightFog"));
			}
			else
			{
				ActionsLog.Add(TEXT("updated existing ExponentialHeightFog"));
			}

			if (FogActor && FogActor->GetComponent())
			{
				UExponentialHeightFogComponent* FogComp = FogActor->GetComponent();
				FogComp->SetFogDensity(FogParams.Density);
				FogComp->SetFogHeightFalloff(FogParams.HeightFalloff);
				FogComp->SetFogInscatteringColor(FogParams.InscatteringColor);
				FogComp->SetVisibility(FogParams.bEnabled);
				FogComp->MarkRenderStateDirty();
			}
		}

		// 3. Configure DirectionalLight
		if (DirLightParams.bHasLight)
		{
			ADirectionalLight* DirLight = nullptr;
			for (TActorIterator<ADirectionalLight> It(World); It; ++It)
			{
				DirLight = *It;
				break;
			}

			if (!DirLight)
			{
				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				DirLight = World->SpawnActor<ADirectionalLight>(
					ADirectionalLight::StaticClass(), FVector::ZeroVector,
					FRotator(-45.0f, 0.0f, 0.0f), SpawnParams);
				DirLight->SetActorLabel(TEXT("EnvironmentDirectionalLight"));
				ActionsLog.Add(TEXT("spawned DirectionalLight"));
			}
			else
			{
				ActionsLog.Add(TEXT("updated existing DirectionalLight"));
			}

			if (DirLight)
			{
				UDirectionalLightComponent* LightComp = Cast<UDirectionalLightComponent>(DirLight->GetLightComponent());
				if (LightComp)
				{
					LightComp->SetIntensity(DirLightParams.Intensity);
					LightComp->SetLightColor(DirLightParams.LightColor);
					LightComp->MarkRenderStateDirty();
				}
			}
		}

		// 4. Configure SkyLight
		if (SkyLightParams.bHasSkyLight)
		{
			ASkyLight* SkyLight = nullptr;
			for (TActorIterator<ASkyLight> It(World); It; ++It)
			{
				SkyLight = *It;
				break;
			}

			if (!SkyLight)
			{
				FActorSpawnParameters SpawnParams;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				SkyLight = World->SpawnActor<ASkyLight>(
					ASkyLight::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
				SkyLight->SetActorLabel(TEXT("EnvironmentSkyLight"));
				ActionsLog.Add(TEXT("spawned SkyLight"));
			}
			else
			{
				ActionsLog.Add(TEXT("updated existing SkyLight"));
			}

			if (SkyLight && SkyLight->GetLightComponent())
			{
				USkyLightComponent* SkyComp = Cast<USkyLightComponent>(SkyLight->GetLightComponent());
				if (SkyComp)
				{
					SkyComp->Intensity = SkyLightParams.Intensity;
					SkyComp->LightColor = SkyLightParams.LightColor.ToFColor(true);
					SkyComp->MarkRenderStateDirty();
					SkyComp->SetCaptureIsDirty();
				}
			}
		}

		// 5. Mark the level dirty
		World->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log, TEXT("SetLevelEnvironment: configured %s — %d actions"),
			*LevelPath, ActionsLog.Num());

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),    true);
		Resp->SetStringField(TEXT("level_path"), LevelPath);

		TArray<TSharedPtr<FJsonValue>> LogArr;
		for (const FString& Action : ActionsLog)
		{
			LogArr.Add(MakeShared<FJsonValueString>(Action));
		}
		Resp->SetArrayField(TEXT("actions"), LogArr);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}
