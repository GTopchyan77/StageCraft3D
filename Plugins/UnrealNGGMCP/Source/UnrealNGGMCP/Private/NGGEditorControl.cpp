// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGEditorControl.cpp
//
// Implementation of the endpoints that drive the editor process itself rather
// than any one asset:
//   /batch                      — run several bridge operations in one request
//   /editor/exec_python         — run Python in the editor's interpreter
//   /editor/shutdown, /editor/build_and_run, /editor/kill_and_restart
//
// The lifecycle routes shell out to the engine's batch files, so the target and
// configuration names a caller supplies are validated against an allowlist
// before they reach a command line.

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
// ---- Engine ----------------------------------------------------------------
#include "Containers/Ticker.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
// ---- Python scripting ------------------------------------------------------
#include "IPythonScriptPlugin.h"


// Auth note: the REHAB_ROUTE macro handles the auth check before dispatching
// to this handler, so sub-operations bypass the outer check intentionally --
// they are internal calls that never leave the process.
// ============================================================================

bool FNGGHttpServer::HandleBatch(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseError;
	TSharedPtr<FJsonObject> RequestObj;
	if (!ParseJsonBody(Req, RequestObj, ParseError))
	{
		OnComplete(JsonError(400, FString::Printf(TEXT("Invalid JSON: %s"), *ParseError)));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
	if (!RequestObj->TryGetArrayField(TEXT("operations"), OpsArray) || !OpsArray || OpsArray->IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'operations' array is required and must be non-empty")));
		return true;
	}

	// Snapshot each operation into a plain struct to avoid dangling pointers
	// into the JSON tree once sub-handlers dispatch onto the Game Thread.
	struct FBatchOp
	{
		FString Method;
		FString Path;
		FString BodyStr;
	};

	TArray<FBatchOp> Ops;
	for (const TSharedPtr<FJsonValue>& Val : *OpsArray)
	{
		const TSharedPtr<FJsonObject>* OpObjPtr = nullptr;
		if (!Val->TryGetObject(OpObjPtr) || !OpObjPtr)
		{
			OnComplete(JsonError(400, TEXT("Each operation must be a JSON object")));
			return true;
		}
		const TSharedPtr<FJsonObject>& OpObj = *OpObjPtr;

		FBatchOp Op;
		OpObj->TryGetStringField(TEXT("method"), Op.Method);
		OpObj->TryGetStringField(TEXT("path"),   Op.Path);

		// Serialize the nested "body" object back to a JSON string.
		const TSharedPtr<FJsonValue>* BodyVal = OpObj->Values.Find(TEXT("body"));
		if (BodyVal && BodyVal->IsValid() && (*BodyVal)->Type == EJson::Object)
		{
			TSharedRef<TJsonWriter<>> BodyWriter = TJsonWriterFactory<>::Create(&Op.BodyStr);
			FJsonSerializer::Serialize((*BodyVal)->AsObject().ToSharedRef(), BodyWriter);
		}
		else
		{
			Op.BodyStr = TEXT("{}");
		}
		Ops.Add(MoveTemp(Op));
	}

	const int32 Total = Ops.Num();

	// Shared result accumulator. TSharedPtr copies in each sub-callback keep it alive
	// until the last async Game Thread task fires its callback.
	TSharedPtr<TArray<TPair<int32, FString>>> Results =
		MakeShared<TArray<TPair<int32, FString>>>();

	// Build dispatch map: path -> bound member function.
	// Note: /editor/reimport and /editor/create_material are intentionally omitted —
	// they are rarely useful in batch context.
	using HandlerFn = TFunction<bool(const FHttpServerRequest&, const FHttpResultCallback&)>;

	// Capture a strong self-ref (mirrors the mesh handlers' AsShared() pattern) so
	// the dispatch lambdas — and the sub-handler AsyncTasks they spawn — never
	// dereference a raw `this` that could be freed if the server is torn down
	// mid-batch. Each dispatch lambda calls through Self-> instead of this->.
	TSharedRef<FNGGHttpServer> Self = AsShared();

	TMap<FString, HandlerFn> Dispatch;
	Dispatch.Add(TEXT("/health"),                        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleHealth(R, C); });
	Dispatch.Add(TEXT("/project_info"),                  [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleProjectInfo(R, C); });
	Dispatch.Add(TEXT("/assets/list"),                   [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAssetsList(R, C); });
	Dispatch.Add(TEXT("/assets/get"),                    [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAssetsGet(R, C); });
	Dispatch.Add(TEXT("/assets/create"),                 [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAssetsCreate(R, C); });
	Dispatch.Add(TEXT("/assets/set_property"),           [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAssetsSetProp(R, C); });
	Dispatch.Add(TEXT("/assets/set_map_entries"),        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAssetsSetMapEntries(R, C); });
	Dispatch.Add(TEXT("/assets/delete"),                 [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAssetsDelete(R, C); });
	Dispatch.Add(TEXT("/editor/save_all"),               [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleEditorSaveAll(R, C); });
	Dispatch.Add(TEXT("/editor/open_level"),             [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleOpenLevel(R, C); });
	Dispatch.Add(TEXT("/editor/create_blueprint"),       [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/create_widget_blueprint"),[Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateWidgetBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/set_blueprint_defaults"), [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSetBlueprintDefaults(R, C); });
	Dispatch.Add(TEXT("/editor/reparent_blueprint"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleReparentBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/set_world_settings"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSetWorldSettings(R, C); });
	Dispatch.Add(TEXT("/editor/spawn_actor_in_level"),   [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSpawnActorInLevel(R, C); });
	Dispatch.Add(TEXT("/editor/set_component_defaults"), [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSetComponentDefaults(R, C); });
	Dispatch.Add(TEXT("/editor/add_component_to_blueprint"), [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAddComponentToBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/remove_component_from_blueprint"), [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleRemoveComponentFromBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/create_level"),           [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateLevel(R, C); });
	Dispatch.Add(TEXT("/editor/list_actors"),            [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleListActors(R, C); });
	Dispatch.Add(TEXT("/editor/update_actor"),           [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleUpdateActor(R, C); });
	Dispatch.Add(TEXT("/editor/delete_actor"),           [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleDeleteActor(R, C); });
	Dispatch.Add(TEXT("/editor/style_widgets"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleStyleWidgets(R, C); });
	Dispatch.Add(TEXT("/editor/get_widget_tree"),        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleGetWidgetTree(R, C); });
	Dispatch.Add(TEXT("/editor/remove_widget_from_blueprint"), [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleRemoveWidgetFromBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/reparent_widget"),        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleReparentWidget(R, C); });
	Dispatch.Add(TEXT("/editor/rename_widget"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleRenameWidget(R, C); });
	Dispatch.Add(TEXT("/editor/compile_widget_blueprint"),[Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCompileWidgetBlueprint(R, C); });
	Dispatch.Add(TEXT("/gameplay_tags/list"),            [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleGameplayTagsList(R, C); });
	Dispatch.Add(TEXT("/input/configure_imc"),           [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleInputConfigureIMC(R, C); });
	Dispatch.Add(TEXT("/editor/shutdown"),               [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleEditorShutdown(R, C); });
	Dispatch.Add(TEXT("/editor/build_and_run"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleEditorBuildAndRun(R, C); });
	Dispatch.Add(TEXT("/editor/kill_and_restart"),       [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleEditorKillAndRestart(R, C); });
	Dispatch.Add(TEXT("/editor/create_material"),        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateMaterial(R, C); });
	Dispatch.Add(TEXT("/editor/create_color_curve"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateColorCurve(R, C); });
	Dispatch.Add(TEXT("/editor/create_float_curve"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateFloatCurve(R, C); });
	Dispatch.Add(TEXT("/editor/read_curve"),             [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleReadCurve(R, C); });
	Dispatch.Add(TEXT("/editor/read_material"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleReadMaterial(R, C); });
	Dispatch.Add(TEXT("/editor/create_niagara_system"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateNiagaraSystem(R, C); });
	Dispatch.Add(TEXT("/editor/configure_niagara_system"),       [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleConfigureNiagaraSystem(R, C); });
	Dispatch.Add(TEXT("/editor/set_niagara_emitter_params"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSetNiagaraEmitterParams(R, C); });
	Dispatch.Add(TEXT("/editor/add_widget_to_blueprint"),        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleAddWidgetToBlueprint(R, C); });
	Dispatch.Add(TEXT("/assets/import"),                         [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleImportAsset(R, C); });
	Dispatch.Add(TEXT("/editor/configure_anim_blueprint"),       [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleConfigureAnimBlueprint(R, C); });
	Dispatch.Add(TEXT("/editor/set_level_environment"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSetLevelEnvironment(R, C); });
	Dispatch.Add(TEXT("/editor/create_material_instance"),      [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateMaterialInstance(R, C); });
	Dispatch.Add(TEXT("/editor/create_post_process_material"),  [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreatePostProcessMaterial(R, C); });
	Dispatch.Add(TEXT("/editor/spawn_post_process_volume"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleSpawnPostProcessVolume(R, C); });
	Dispatch.Add(TEXT("/editor/exec_python"),                   [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleExecPython(R, C); });
	Dispatch.Add(TEXT("/asset/create_blueprint_struct"),        [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateBlueprintStruct(R, C); });
	Dispatch.Add(TEXT("/asset/create_blueprint_enum"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateBlueprintEnum(R, C); });
	Dispatch.Add(TEXT("/asset/create_blueprint_interface"),     [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateBlueprintInterface(R, C); });
	Dispatch.Add(TEXT("/asset/create_anim_blueprint"),          [Self](const FHttpServerRequest& R, const FHttpResultCallback& C){ return Self->HandleCreateAnimBlueprint(R, C); });

	// TryFinalize is captured by value (TSharedPtr copy) into each sub-callback.
	// It fires the final batch response once all operations have reported.
	auto TryFinalize = [Results, Total, OnComplete](int32 Idx, FString&& ResultJson) mutable
	{
		Results->Add(TPair<int32, FString>(Idx, MoveTemp(ResultJson)));

		if (Results->Num() == Total)
		{
			Results->Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B)
			{
				return A.Key < B.Key;
			});

			FString FinalBody = TEXT("{\"results\":[");
			for (int32 R = 0; R < Results->Num(); ++R)
			{
				if (R > 0) FinalBody += TEXT(",");
				FinalBody += (*Results)[R].Value;
			}
			FinalBody += TEXT("]}");
			OnComplete(JsonOk(FinalBody));
		}
	};

	for (int32 Idx = 0; Idx < Total; ++Idx)
	{
		const FBatchOp& Op = Ops[Idx];

		HandlerFn* FoundHandler = Dispatch.Find(Op.Path);
		if (!FoundHandler)
		{
			// Unknown path -- record a 404 without touching the Game Thread.
			FString ErrJson = FString::Printf(
				TEXT("{\"index\":%d,\"status\":404,\"body\":{\"error\":\"Unknown batch path: %s\"}}"),
				Idx, *Op.Path);
			TryFinalize(Idx, MoveTemp(ErrJson));
			continue;
		}

		// Construct a minimal FHttpServerRequest for this sub-operation.
		// FHttpServerRequest is a plain struct with no private members or
		// non-default-constructible fields, so default-construction is safe.
		FHttpServerRequest SubReq;
		SubReq.RelativePath = FHttpPath(Op.Path);
		SubReq.Verb = (Op.Method.ToUpper() == TEXT("GET"))
			? EHttpServerRequestVerbs::VERB_GET
			: EHttpServerRequestVerbs::VERB_POST;

		// UTF-8 encode the body string into the byte array.
		const FTCHARToUTF8 Utf8(*Op.BodyStr);
		SubReq.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		SubReq.Headers.Add(TEXT("content-type"), { TEXT("application/json") });

		int32 CapturedIdx = Idx;
		auto CapturedFinalize = TryFinalize;  // copy -- captures TSharedPtr Results by value

		FHttpResultCallback SubCallback =
			[CapturedIdx, CapturedFinalize](TUniquePtr<FHttpServerResponse> SubResponse) mutable
		{
			int32 StatusCode = 500;
			FString SubBodyStr = TEXT("{}");

			if (SubResponse.IsValid())
			{
				StatusCode = static_cast<int32>(SubResponse->Code);

				if (SubResponse->Body.Num() > 0)
				{
					// Mirror the safe UTF-8 decoding pattern used in ParseJsonBody.
					const FUTF8ToTCHAR Converter(
						reinterpret_cast<const ANSICHAR*>(SubResponse->Body.GetData()),
						SubResponse->Body.Num());
					SubBodyStr = FString(Converter.Length(), Converter.Get());
				}
			}

			FString ResultJson = FString::Printf(
				TEXT("{\"index\":%d,\"status\":%d,\"body\":%s}"),
				CapturedIdx,
				StatusCode,
				*SubBodyStr);

			CapturedFinalize(CapturedIdx, MoveTemp(ResultJson));
		};

		(*FoundHandler)(SubReq, SubCallback);
	}

	return true;
}

// ============================================================================
// Handler: POST /editor/shutdown
// ============================================================================
bool FNGGHttpServer::HandleEditorShutdown(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// Destructive lifecycle route — gate on auth like exec_python (see HandleExecPython).
	if (AuthToken.IsEmpty())
	{
		OnComplete(JsonError(403,
			TEXT("editor/shutdown is disabled: no AuthToken configured. Token provisioning failed at editor startup — check the log for the UserEngine.ini path, or run `npm run set-token` in Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/ and restart the editor.")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetBoolField(TEXT("success"), true);
	Resp->SetStringField(TEXT("message"), TEXT("Editor shutdown initiated"));
	FString RespStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&RespStr);
	FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
	Callback(JsonOk(RespStr));

	AsyncTask(ENamedThreads::GameThread, []()
	{
		UEditorLoadingAndSavingUtils::SaveDirtyPackages(false, true);
		FTimerHandle Handle;
		GEditor->GetTimerManager()->SetTimer(Handle, []() { FPlatformMisc::RequestExit(false); }, 0.5f, false);
	});
	return true;
}

// ============================================================================
// Build/restart routes interpolate `target` and `config` into a PowerShell
// command line. Both values are therefore restricted to the exact shapes
// UnrealBuildTool accepts, so request data can never carry shell syntax —
// the only executables these routes can start are the engine's own Build.bat
// and UnrealEditor.exe at paths derived from FPaths::EngineDir().
// ============================================================================
static bool IsSafeBuildTargetName(const FString& Target)
{
	if (Target.IsEmpty() || Target.Len() > 128)
	{
		return false;
	}
	for (const TCHAR C : Target)
	{
		if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-'))
		{
			return false;
		}
	}
	return true;
}

static bool NormalizeBuildConfig(FString& InOutConfig)
{
	static const TCHAR* Allowed[] = {
		TEXT("Debug"), TEXT("DebugGame"), TEXT("Development"), TEXT("Test"), TEXT("Shipping")
	};
	for (const TCHAR* Candidate : Allowed)
	{
		if (InOutConfig.Equals(Candidate, ESearchCase::IgnoreCase))
		{
			InOutConfig = Candidate;
			return true;
		}
	}
	return false;
}

// ============================================================================
// Handler: POST /editor/build_and_run
// Body (optional): "target", "config", "shutdown_first" (default true)
// ============================================================================
bool FNGGHttpServer::HandleEditorBuildAndRun(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// Destructive lifecycle route (spawns processes, may shut the editor down) —
	// gate on auth like exec_python (see HandleExecPython).
	if (AuthToken.IsEmpty())
	{
		OnComplete(JsonError(403,
			TEXT("editor/build_and_run is disabled: no AuthToken configured. Token provisioning failed at editor startup — check the log for the UserEngine.ini path, or run `npm run set-token` in Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/ and restart the editor.")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseError;
	ParseJsonBody(Req, Body, ParseError);

	const FString UProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString DefaultTarget = FString::Printf(TEXT("%sEditor"), FApp::GetProjectName());
	// Use TryGet* so a wrong-typed field falls back to the default instead of
	// tripping a check() inside GetStringField/GetBoolField.
	FString Target = DefaultTarget;
	if (Body) Body->TryGetStringField(TEXT("target"), Target);
	FString Config = TEXT("Development");
	if (Body) Body->TryGetStringField(TEXT("config"), Config);
	bool bShutdown = true;
	if (Body) Body->TryGetBoolField(TEXT("shutdown_first"), bShutdown);

	if (!IsSafeBuildTargetName(Target))
	{
		Callback(JsonError(400, TEXT("Invalid 'target': only letters, digits, '_' and '-' are allowed (a UBT target name).")));
		return true;
	}
	if (!NormalizeBuildConfig(Config))
	{
		Callback(JsonError(400, TEXT("Invalid 'config': must be one of Debug, DebugGame, Development, Test, Shipping.")));
		return true;
	}

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetBoolField(TEXT("success"), true);
	Resp->SetStringField(TEXT("message"), TEXT("Build+run initiated"));
	FString RespStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&RespStr);
	FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
	Callback(JsonOk(RespStr));

	const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	const FString BuildBat  = FPaths::Combine(EngineDir, TEXT("Build"), TEXT("BatchFiles"), TEXT("Build.bat"));
	const FString EditorExe = FPaths::Combine(EngineDir, TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor.exe"));

	AsyncTask(ENamedThreads::GameThread, [UProjectPath, Target, Config, bShutdown, BuildBat, EditorExe]()
	{
		UEditorLoadingAndSavingUtils::SaveDirtyPackages(false, true);
		FString SafeProject = UProjectPath; SafeProject.ReplaceInline(TEXT("'"), TEXT("''"));
		FString SafeBuild = BuildBat;    SafeBuild.ReplaceInline(TEXT("'"), TEXT("''"));
		FString SafeEditor = EditorExe;  SafeEditor.ReplaceInline(TEXT("'"), TEXT("''"));
		FString PSScript = FString::Printf(
			TEXT("%s& '%s' %s Win64 %s '%s'; if ($LASTEXITCODE -eq 0) { & '%s' '%s' }"),
			bShutdown ? TEXT("Start-Sleep -Seconds 3; ") : TEXT(""),
			*SafeBuild, *Target, *Config, *SafeProject, *SafeEditor, *SafeProject);
		FString Args = FString::Printf(TEXT("-NonInteractive -Command \"%s\""), *PSScript);
		FProcHandle Proc = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Args, true, true, true, nullptr, 0, nullptr, nullptr);
		if (Proc.IsValid()) FPlatformProcess::CloseProc(Proc);
		if (bShutdown)
		{
			FTimerHandle Handle;
			GEditor->GetTimerManager()->SetTimer(Handle, []() { FPlatformMisc::RequestExit(false); }, 1.0f, false);
		}
	});
	return true;
}

// ============================================================================
// Handler: POST /editor/kill_and_restart
// Body (optional): "build" (default true), "target", "config"
// Force-kills UnrealEditor.exe — bypasses Live Coding lock.
// Waits for process to exit, optionally builds, then relaunches.
// ============================================================================
bool FNGGHttpServer::HandleEditorKillAndRestart(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// Destructive lifecycle route (force-kills and relaunches the editor) —
	// gate on auth like exec_python (see HandleExecPython).
	if (AuthToken.IsEmpty())
	{
		OnComplete(JsonError(403,
			TEXT("editor/kill_and_restart is disabled: no AuthToken configured. Token provisioning failed at editor startup — check the log for the UserEngine.ini path, or run `npm run set-token` in Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/ and restart the editor.")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseError;
	ParseJsonBody(Req, Body, ParseError);

	const FString UProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString DefaultTarget = FString::Printf(TEXT("%sEditor"), FApp::GetProjectName());
	// Use TryGet* so a wrong-typed field falls back to the default instead of
	// tripping a check() inside GetBoolField/GetStringField.
	bool bBuild = true;
	if (Body) Body->TryGetBoolField(TEXT("build"), bBuild);
	FString Target = DefaultTarget;
	if (Body) Body->TryGetStringField(TEXT("target"), Target);
	FString Config = TEXT("Development");
	if (Body) Body->TryGetStringField(TEXT("config"), Config);

	if (!IsSafeBuildTargetName(Target))
	{
		Callback(JsonError(400, TEXT("Invalid 'target': only letters, digits, '_' and '-' are allowed (a UBT target name).")));
		return true;
	}
	if (!NormalizeBuildConfig(Config))
	{
		Callback(JsonError(400, TEXT("Invalid 'config': must be one of Debug, DebugGame, Development, Test, Shipping.")));
		return true;
	}

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetBoolField(TEXT("success"), true);
	Resp->SetStringField(TEXT("message"), TEXT("Kill+restart initiated"));
	FString RespStr;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&RespStr);
	FJsonSerializer::Serialize(Resp.ToSharedRef(), W);
	Callback(JsonOk(RespStr));

	const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	const FString BuildBat  = FPaths::Combine(EngineDir, TEXT("Build"), TEXT("BatchFiles"), TEXT("Build.bat"));
	const FString EditorExe = FPaths::Combine(EngineDir, TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor.exe"));

	AsyncTask(ENamedThreads::GameThread, [UProjectPath, bBuild, Target, Config, BuildBat, EditorExe]()
	{
		UEditorLoadingAndSavingUtils::SaveDirtyPackages(false, true);
		FString SafeProject = UProjectPath; SafeProject.ReplaceInline(TEXT("'"), TEXT("''"));
		FString SafeBuild = BuildBat;       SafeBuild.ReplaceInline(TEXT("'"), TEXT("''"));
		FString SafeEditor = EditorExe;     SafeEditor.ReplaceInline(TEXT("'"), TEXT("''"));

		// Build step (optional)
		FString BuildStep = bBuild
			? FString::Printf(TEXT("& '%s' %s Win64 %s '%s'; if ($LASTEXITCODE -ne 0) { exit 1 }; "), *SafeBuild, *Target, *Config, *SafeProject)
			: TEXT("");

		// Wait for editor to exit (max 30s), build, relaunch
		FString PSScript = FString::Printf(
			TEXT("$t=(Get-Date).AddSeconds(30); while((Get-Process -Name UnrealEditor -EA SilentlyContinue) -and (Get-Date)-lt $t){Start-Sleep 1}; ")
			TEXT("%s& '%s' '%s'"),
			*BuildStep, *SafeEditor, *SafeProject);

		FString Args = FString::Printf(TEXT("-NonInteractive -Command \"%s\""), *PSScript);
		FProcHandle Proc = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Args, true, true, true, nullptr, 0, nullptr, nullptr);
		if (Proc.IsValid()) FPlatformProcess::CloseProc(Proc);

		// Force-kill this process — bypasses Live Coding mutex
		FTimerHandle Handle;
		GEditor->GetTimerManager()->SetTimer(Handle, []() { FPlatformMisc::RequestExit(true); }, 0.5f, false);
	});
	return true;
}

// ============================================================================
// POST /editor/exec_python — execute Python code inside the editor
// ============================================================================

bool FNGGHttpServer::HandleExecPython(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	// Arbitrary code execution must never be reachable unauthenticated. The peer
	// gate already limits callers to this machine, but a token is the only thing
	// that distinguishes the MCP client from any other local process able to open
	// a socket, so refuse without one rather than handing out a Python REPL into
	// the editor. (Read/asset routes stay open; only this destructive route gates.)
	if (AuthToken.IsEmpty())
	{
		Callback(JsonError(403,
			TEXT("exec_python is disabled: no AuthToken configured. Token provisioning failed at editor startup — check the log for the UserEngine.ini path, or run `npm run set-token` in Plugins/UnrealNGGMCP/Source/ThirdParty/unrealngg-mcp/ and restart the editor. Note this also disables every pcg_* tool.")));
		return true;
	}

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError))
	{
		Callback(JsonError(400, ParseError));
		return true;
	}

	FString Command; Body->TryGetStringField(TEXT("command"), Command);
	if (Command.IsEmpty())
	{
		Callback(JsonError(400, TEXT("command is required")));
		return true;
	}

	FString Mode;
	Body->TryGetStringField(TEXT("mode"), Mode);

	// Dispatch via FTSTicker so the Python command runs on the game thread OUTSIDE
	// any TaskGraph task. Arbitrary Python (e.g. asset_tools.import_asset_tasks)
	// can call into Interchange, whose WaitUntilDone pumps the GameThread task
	// queue synchronously; running this handler as an AsyncTask(GameThread) lambda
	// re-enters FNamedTaskThread::ProcessTasksUntilIdle and trips the recursion
	// guard. Ticker delegates fire during engine tick, not inside a task.
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Callback, Command, Mode](float /*DeltaTime*/) -> bool
	{
		IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
		if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
		{
			Callback(JsonError(500, TEXT("PythonScriptPlugin is not available. Enable it in Edit > Plugins.")));
			return false;
		}

		FPythonCommandEx PythonCommand;
		PythonCommand.Command = Command;
		PythonCommand.Flags = EPythonCommandFlags::Unattended;

		// Set execution mode
		if (Mode == TEXT("execute_statement"))
		{
			PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteStatement;
		}
		else if (Mode == TEXT("evaluate_statement"))
		{
			PythonCommand.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
		}
		else
		{
			PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		}

		PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Public;

		const bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);

		// Build response
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField(TEXT("success"), bSuccess);
		Resp->SetStringField(TEXT("result"), PythonCommand.CommandResult);

		// Collect log output
		TArray<TSharedPtr<FJsonValue>> LogArray;
		for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
		{
			TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
			LogEntry->SetStringField(TEXT("type"), LexToString(Entry.Type));
			LogEntry->SetStringField(TEXT("output"), Entry.Output);
			LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
		}
		Resp->SetArrayField(TEXT("log"), LogArray);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);

		if (bSuccess)
		{
			Callback(JsonOk(RespBody));
		}
		else
		{
			Callback(JsonError(500, RespBody));
		}
		return false;
	}));

	return true;
}
