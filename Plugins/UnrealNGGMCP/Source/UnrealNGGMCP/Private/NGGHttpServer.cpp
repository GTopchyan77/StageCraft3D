// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGHttpServer.cpp
//
// The bridge itself: socket lifecycle, route table, authentication, and the
// JSON <-> UObject reflection helpers every handler is built on. The handlers
// themselves live in subject files next to this one — NGGAssets.cpp,
// NGGBlueprintAssets.cpp, NGGBlueprintGraph.cpp, NGGLevel.cpp, NGGWidgets.cpp,
// NGGMaterials.cpp, NGGNiagara.cpp, NGGAnimation.cpp, NGGInput.cpp,
// NGGEditorControl.cpp, NGGBehaviorTree.cpp, NGGStateTree.cpp,
// NGGGameplayAbilitySystem.cpp and NGGMeshGeometryScript.cpp — all of them
// members of FNGGHttpServer declared in NGGHttpServer.h.
//
// Threading contract:
//   - UE5's HttpServerModule calls handlers on an internal worker thread.
//   - All UnrealEd / Asset Registry / UObject APIs MUST run on the Game Thread.
//   - Every handler therefore wraps its work in AsyncTask(ENamedThreads::GameThread).
//   - The FHttpResultCallback is thread-safe and can be called from any thread,
//     so we call it from inside the GameThread task after the work is done.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- UE5 HTTP Server -------------------------------------------------------
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
// FInternetAddr::GetRawIp — peer-address inspection for the loopback gate.
#include "IPAddress.h"
// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/UnrealType.h"
#include "UObject/PropertyIterator.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
// ---- Engine ----------------------------------------------------------------
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Editor.h"
// ---- First-run auth-token provisioning -------------------------------------
#include "Misc/App.h"          // FApp::HasProjectName — never provision in a commandlet
#include "Misc/FileHelper.h"   // line-edit read/write of the ini, comments intact
#include "Misc/Guid.h"         // FGuid::NewGuid — the token's entropy
#include "HAL/FileManager.h"   // IFileManager::IsReadOnly — source-controlled ini
// ---- Types the generic property bridge special-cases -----------------------
// PropertyToJson renders an FGameplayTag as its tag name rather than as a
// struct, and SetPropertyFromJson routes a material's colour through a real
// expression node instead of writing the property raw.
#include "GameplayTagContainer.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MaterialEditingLibrary.h"


// ============================================================================
// Utility macros
// ============================================================================

// Maximum accepted request-body size (16 MiB). Bodies larger than this are
// rejected with 413 at the route boundary (and again in ParseJsonBody) before
// being copied into an FString, to bound memory use.
static constexpr int32 NGG_MaxRequestBodyBytes = 16 * 1024 * 1024;

#define REHAB_ROUTE(Router, Verb, Path, Handler) \
	RouteHandles.Add( \
		(Router)->BindRoute( \
			FHttpPath(TEXT(Path)), \
			EHttpServerRequestVerbs::VERB_##Verb, \
			FHttpRequestHandler::CreateLambda( \
				[this](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete) -> bool \
				{ \
					if (Req.Body.Num() > NGG_MaxRequestBodyBytes) \
					{ \
						OnComplete(JsonError(413, TEXT("Request body too large (max 16 MiB)"))); \
						return true; \
					} \
					if (!bAllowRemoteClients && !IsLoopbackPeer(Req)) \
					{ \
						OnComplete(JsonError(403, TEXT("Forbidden: this bridge serves loopback clients only. Set [UnrealNGGMCP] AllowRemoteClients=true in DefaultEngine.ini to permit remote access."))); \
						return true; \
					} \
					if (!AuthToken.IsEmpty()) \
					{ \
						const TArray<FString>* AuthValues = Req.Headers.Find(TEXT("authorization")); \
						const FString Expected = TEXT("Bearer ") + AuthToken; \
						if (!AuthValues || AuthValues->Num() == 0 || !ConstantTimeEquals((*AuthValues)[0], Expected)) \
						{ \
							OnComplete(JsonError(401, TEXT("Unauthorized: missing or invalid Authorization header"))); \
							return true; \
						} \
					} \
					return this->Handler(Req, OnComplete); \
				} \
			) \
		) \
	)


// ============================================================================
// First-run auth-token provisioning
// ============================================================================
//
// A project that has never been configured has no [UnrealNGGMCP] AuthToken, so
// exec_python and the editor lifecycle routes refuse every call — which is the
// first thing a developer hits after dropping the plugin into a new project.
// Instead of leaving them to hand-write a token, the bridge mints one on first
// run and writes it into that project's own Config/UserEngine.ini — one of the
// two layers the MCP sidecar falls back to when building its Authorization
// header, so both ends agree with no further setup.
//
// UserEngine.ini and not DefaultEngine.ini: UserEngine.ini is GConfig's
// `GameDirUser` layer, the LAST entry in GConfigLayers
// (Core/Public/Misc/ConfigHierarchy.h), so it overrides everything below it and
// the editor never rewrites it — and, the reason that matters here, it is
// gitignored. DefaultEngine.ini is tracked, so a token written there would be
// published in the repository history. That has already happened once in this
// repo and cost a token rotation.
//
// The token is random per project rather than a fixed constant — a shipped
// default would be public knowledge and would gate nothing.
//
// Two opt-outs are honoured, both meaning "this project runs unauthenticated":
//   AuthToken=                      (key present, value empty)
//   AutoProvisionAuthToken=false
// ============================================================================

namespace
{
	/**
	 * <Project>/Config/UserEngine.ini — the gitignored config layer the token
	 * belongs in, and the first of the two layers the sidecar reads.
	 */
	FString ProjectUserEngineIniPath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("UserEngine.ini")));
	}

	/**
	 * 64 hex characters (256 bits) from two platform GUIDs. FGuid::NewGuid goes
	 * through the OS UUID generator, so the value does not follow from anything
	 * else the editor exposes.
	 */
	FString GenerateAuthToken()
	{
		return (FGuid::NewGuid().ToString(EGuidFormats::Digits)
			  + FGuid::NewGuid().ToString(EGuidFormats::Digits)).ToLower();
	}

	/**
	 * Reads AuthToken straight out of raw ini text, using the same rule as the
	 * sidecar's ue5client.js: the first AuthToken= line inside [UnrealNGGMCP]
	 * wins. Returns true if the key is present at all — an empty value is
	 * meaningful ("auth deliberately off") and is not the same as no key.
	 */
	bool FindAuthTokenInIniText(const FString& Text, FString& OutValue)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

		bool bInSection = false;
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.StartsWith(TEXT("[")))
			{
				bInSection = Line.Equals(TEXT("[UnrealNGGMCP]"), ESearchCase::IgnoreCase);
				continue;
			}
			if (!bInSection || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
			{
				continue;
			}

			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value)
				&& Key.TrimStartAndEnd().Equals(TEXT("AuthToken"), ESearchCase::IgnoreCase))
			{
				OutValue = Value.TrimStartAndEnd();
				return true;
			}
		}
		return false;
	}

	enum class EAuthTokenProvision : uint8
	{
		FoundExisting,   // the ini already named a token — adopt it
		Written,         // minted a token and persisted it
		DisabledByUser,  // AuthToken= present but empty: deliberate, leave alone
		Failed           // could not read or write the ini
	};

	/**
	 * Guarantees the project's UserEngine.ini carries an [UnrealNGGMCP]
	 * AuthToken, minting one if it does not, and hands back the value the
	 * sidecar will send.
	 *
	 * The token is inserted as a line edit rather than through
	 * GConfig->SetString + Flush on purpose. Two reasons: FConfigFile rewriting
	 * drops the developer's comments and reflows the whole file, and GConfig
	 * only flushes files already in its cache — SetString + Flush against a
	 * UserEngine.ini that does not exist yet is silently a no-op. Every byte
	 * outside the inserted line is preserved.
	 *
	 * Re-reading the file (rather than trusting GConfig) also keeps this
	 * idempotent: a token added while the editor was running, or a second call
	 * after a module reload, is adopted instead of producing a duplicate entry
	 * that the two ends would resolve differently.
	 */
	EAuthTokenProvision EnsureProjectAuthToken(FString& OutToken, FString& OutError)
	{
		const FString IniPath     = ProjectUserEngineIniPath();
		const bool    bFileExists = FPaths::FileExists(IniPath);

		FString Text;
		if (bFileExists && !FFileHelper::LoadFileToString(Text, *IniPath))
		{
			OutError = FString::Printf(TEXT("could not read %s"), *IniPath);
			return EAuthTokenProvision::Failed;
		}

		if (FindAuthTokenInIniText(Text, OutToken))
		{
			return OutToken.IsEmpty()
				? EAuthTokenProvision::DisabledByUser
				: EAuthTokenProvision::FoundExisting;
		}

		if (bFileExists && IFileManager::Get().IsReadOnly(*IniPath))
		{
			OutError = FString::Printf(
				TEXT("%s is read-only — check it out from source control first"), *IniPath);
			return EAuthTokenProvision::Failed;
		}

		const FString Token = GenerateAuthToken();
		const FString Entry = FString::Printf(TEXT("AuthToken=%s"), *Token);

		// Match whatever the file already uses; a file we are creating gets the
		// platform's own terminator.
		const FString Eol = Text.IsEmpty()
			? FString(LINE_TERMINATOR)
			: FString(Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n"));

		// Locate [UnrealNGGMCP] as a line of its own, so a longer section name
		// that merely contains it cannot be mistaken for a match.
		int32 SectionStart = INDEX_NONE;
		for (int32 Found = Text.Find(TEXT("[UnrealNGGMCP]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 0);
			 Found != INDEX_NONE;
			 Found = Text.Find(TEXT("[UnrealNGGMCP]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Found + 1))
		{
			if (Found == 0 || Text[Found - 1] == TEXT('\n'))
			{
				SectionStart = Found;
				break;
			}
		}

		if (SectionStart != INDEX_NONE)
		{
			const int32 HeaderEnd =
				Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SectionStart);
			if (HeaderEnd == INDEX_NONE)
			{
				// The header is the last line in the file and has no terminator.
				Text += Eol;
				Text += Entry;
				Text += Eol;
			}
			else
			{
				Text.InsertAt(HeaderEnd + 1, Entry + Eol);
			}
		}
		else
		{
			if (!Text.IsEmpty())
			{
				if (!Text.EndsWith(TEXT("\n")))
				{
					Text += Eol;
				}
				Text += Eol;                 // blank line before the new section
			}
			Text += TEXT("[UnrealNGGMCP]");
			Text += Eol;
			Text += Entry;
			Text += Eol;
		}

		// Hand the token back either way: on failure the caller prints it as the
		// line to paste in by hand.
		OutToken = Token;

		if (!FFileHelper::SaveStringToFile(Text, *IniPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *IniPath);
			return EAuthTokenProvision::Failed;
		}

		return EAuthTokenProvision::Written;
	}
}

// ============================================================================
// Construction / Destruction
// ============================================================================

FNGGHttpServer::FNGGHttpServer(uint32 InPort)
	: Port(InPort)
{
}

FNGGHttpServer::~FNGGHttpServer()
{
	Stop();
}

// ============================================================================
// Start / Stop
// ============================================================================

bool FNGGHttpServer::Start()
{
	if (bRunning)
	{
		return true;
	}

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	Router = HttpServerModule.GetHttpRouter(Port);

	if (!Router.IsValid())
	{
		UE_LOG(LogNGGBridge, Error, TEXT("NGGHttpServer: Could not obtain router for port %d"), Port);
		return false;
	}

	// Read auth token from [UnrealNGGMCP] AuthToken=... Reading GEngineIni picks
	// up whichever config layer supplies it — Config/UserEngine.ini (the
	// gitignored GameDirUser layer, where it belongs) or Config/DefaultEngine.ini
	// (tracked; older installs still have it there).
	//
	// Empty string means auth is disabled. A key that is present but empty is a
	// deliberate "run unauthenticated"; a key that is absent means this project
	// has never been set up, and ProvisionAuthToken mints one for it.
	const bool bTokenKeyPresent =
		GConfig->GetString(TEXT("UnrealNGGMCP"), TEXT("AuthToken"), AuthToken, GEngineIni);
	AuthToken.TrimStartAndEndInline();

	if (!bTokenKeyPresent)
	{
		ProvisionAuthToken();
	}
	else if (AuthToken.IsEmpty())
	{
		UE_LOG(LogNGGBridge, Log,
			TEXT("NGGHttpServer: [UnrealNGGMCP] AuthToken is present but empty — auth stays off by request. ")
			TEXT("Remove the line entirely if you want the bridge to generate a token for this project."));
	}
	else
	{
		WarnIfProjectIniTokenDiffers();
	}

	// UE binds this listener to localhost unless the project overrides
	// [HTTPServer.Listeners] BindAddress, so the socket is normally unreachable
	// from off-machine already. Peer filtering is the second layer: it keeps a
	// BindAddress=any override from silently exposing the editor. Defaults on;
	// opting out is deliberate.
	bAllowRemoteClients = false;
	GConfig->GetBool(TEXT("UnrealNGGMCP"), TEXT("AllowRemoteClients"), bAllowRemoteClients, GEngineIni);

	if (!bAllowRemoteClients)
	{
		UE_LOG(LogNGGBridge, Log,
			TEXT("NGGHttpServer: Loopback-only — requests from non-local peers are refused with 403. ")
			TEXT("Set [UnrealNGGMCP] AllowRemoteClients=true in DefaultEngine.ini to serve other machines."));
	}
	else if (!AuthToken.IsEmpty())
	{
		UE_LOG(LogNGGBridge, Warning,
			TEXT("NGGHttpServer: Peer filtering DISABLED (AllowRemoteClients=true) — requests are accepted from any peer ")
			TEXT("that can reach the socket, and must carry 'Authorization: Bearer <token>'. ")
			TEXT("Note this only widens access if [HTTPServer.Listeners] also binds beyond localhost."));
	}
	else
	{
		UE_LOG(LogNGGBridge, Error,
			TEXT("NGGHttpServer: UNAUTHENTICATED — AllowRemoteClients=true with no AuthToken set. ")
			TEXT("If [HTTPServer.Listeners] binds port %u beyond localhost, anyone who can reach it can drive this editor: ")
			TEXT("create, modify and delete assets. Set [UnrealNGGMCP] AuthToken=<token>, or remove AllowRemoteClients."), Port);
	}

	if (AuthToken.IsEmpty())
	{
		// exec_python and the lifecycle routes stay gated on a token regardless of
		// peer filtering — a token is the only thing that distinguishes the MCP
		// client from any other local process that can open a socket.
		//
		// Reaching here means provisioning failed (it has already logged why);
		// name the blast radius so a dead pcg_* tool isn't a separate mystery.
		UE_LOG(LogNGGBridge, Warning,
			TEXT("NGGHttpServer: No AuthToken — exec_python, every pcg_* tool, and the lifecycle routes ")
			TEXT("(shutdown / build_and_run / kill_and_restart) are DISABLED. See the provisioning warning above."));
	}

	RegisterRoutes();

	HttpServerModule.StartAllListeners();
	bRunning = true;

	UE_LOG(LogNGGBridge, Log, TEXT("NGGHttpServer: Started on port %d"), Port);
	return true;
}

void FNGGHttpServer::Stop()
{
	if (!bRunning)
	{
		return;
	}

	UnregisterRoutes();

	if (FModuleManager::Get().IsModuleLoaded("HTTPServer"))
	{
		FHttpServerModule::Get().StopAllListeners();
	}

	Router.Reset();
	bRunning = false;

	UE_LOG(LogNGGBridge, Log, TEXT("NGGHttpServer: Stopped"));
}

void FNGGHttpServer::ProvisionAuthToken()
{
	bool bAutoProvision = true;
	GConfig->GetBool(TEXT("UnrealNGGMCP"), TEXT("AutoProvisionAuthToken"), bAutoProvision, GEngineIni);
	if (!bAutoProvision)
	{
		UE_LOG(LogNGGBridge, Log,
			TEXT("NGGHttpServer: No AuthToken and AutoProvisionAuthToken=false — leaving this project unauthenticated."));
		return;
	}

	// Commandlets (cook, package, resave) load this module too and must never
	// mutate the project's config as a side effect of a build.
	if (IsRunningCommandlet() || !FApp::HasProjectName())
	{
		return;
	}

	FString Token;
	FString Error;
	switch (EnsureProjectAuthToken(Token, Error))
	{
	case EAuthTokenProvision::Written:
		AuthToken = Token;
		UE_LOG(LogNGGBridge, Log,
			TEXT("NGGHttpServer: This project had no AuthToken — generated one and wrote it to %s under [UnrealNGGMCP]. ")
			TEXT("The MCP sidecar reads the same file, so exec_python and the lifecycle routes work with no further setup. ")
			TEXT("The token is stored in plaintext: keep it out of public repositories. To run unauthenticated instead, ")
			TEXT("replace the value with an empty 'AuthToken=' or set AutoProvisionAuthToken=false."),
			*ProjectUserEngineIniPath());
		break;

	case EAuthTokenProvision::FoundExisting:
		// The ini named a token that the loaded config hierarchy did not carry —
		// somebody edited the file after the editor read it. The sidecar reads
		// the file, so the file wins.
		AuthToken = Token;
		UE_LOG(LogNGGBridge, Log,
			TEXT("NGGHttpServer: Adopted the AuthToken already present in %s."),
			*ProjectUserEngineIniPath());
		break;

	case EAuthTokenProvision::DisabledByUser:
		UE_LOG(LogNGGBridge, Log,
			TEXT("NGGHttpServer: %s carries an empty AuthToken — auth stays off by request."),
			*ProjectUserEngineIniPath());
		break;

	case EAuthTokenProvision::Failed:
	default:
	{
		// Adopting a token the sidecar cannot read would 401 every request —
		// strictly worse than the unauthenticated state we started in. Stay put
		// and print the line the developer can paste in by hand.
		const FString Suggested = Token.IsEmpty() ? GenerateAuthToken() : Token;
		UE_LOG(LogNGGBridge, Warning,
			TEXT("NGGHttpServer: Could not provision an AuthToken (%s). exec_python and the lifecycle routes stay disabled. ")
			TEXT("Add this under [UnrealNGGMCP] in Config/UserEngine.ini by hand to enable them ")
			TEXT("(UserEngine.ini, not DefaultEngine.ini — the latter is tracked in git):\n")
			TEXT("    AuthToken=%s"),
			*Error, *Suggested);
		break;
	}
	}
}

void FNGGHttpServer::WarnIfProjectIniTokenDiffers() const
{
	// The bridge takes its token from the whole config hierarchy, while the
	// sidecar reads only two files: Config/UserEngine.ini then
	// Config/DefaultEngine.ini. When a token is set somewhere else in the
	// hierarchy (a Saved/ override, a platform ini) the two disagree and every
	// request comes back 401 with nothing in the log to explain it. Say so once
	// at startup instead.
	//
	// Both layers are checked, not just the one provisioning writes: an older
	// install that keeps its token in DefaultEngine.ini is still perfectly
	// readable by the sidecar and must not be warned about.
	const FString SidecarIniPaths[] =
	{
		ProjectUserEngineIniPath(),
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultEngine.ini"))),
	};

	for (const FString& IniPath : SidecarIniPaths)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *IniPath))
		{
			continue;
		}

		FString FileToken;
		if (FindAuthTokenInIniText(Text, FileToken) && FileToken == AuthToken)
		{
			return;
		}
	}

	UE_LOG(LogNGGBridge, Warning,
		TEXT("NGGHttpServer: The active AuthToken comes from neither %s nor Config/DefaultEngine.ini ")
		TEXT("(it is set elsewhere in the config hierarchy). The MCP sidecar reads only those two files, ")
		TEXT("so requests will be rejected with 401 unless the same value is put in one of them ")
		TEXT("(UserEngine.ini is the gitignored one) or exported as NGG_BRIDGE_TOKEN."),
		*SidecarIniPaths[0]);
}

// ============================================================================
// Route registration
// ============================================================================

void FNGGHttpServer::RegisterRoutes()
{
	if (!Router.IsValid()) return;

	// ---- Infrastructure ----------------------------------------------------
	REHAB_ROUTE(Router, GET,  "/health",                  HandleHealth);
	REHAB_ROUTE(Router, GET,  "/project_info",            HandleProjectInfo);

	// ---- Generic asset CRUD ------------------------------------------------
	REHAB_ROUTE(Router, GET,  "/assets/list",             HandleAssetsList);
	REHAB_ROUTE(Router, GET,  "/assets/get",              HandleAssetsGet);
	REHAB_ROUTE(Router, POST, "/assets/create",           HandleAssetsCreate);
	REHAB_ROUTE(Router, POST, "/assets/duplicate",        HandleAssetsDuplicate);
	REHAB_ROUTE(Router, POST, "/assets/set_property",     HandleAssetsSetProp);
	REHAB_ROUTE(Router, POST, "/assets/set_map_entries", HandleAssetsSetMapEntries);

	// ---- Editor utilities --------------------------------------------------
	REHAB_ROUTE(Router, POST, "/editor/save_all",         HandleEditorSaveAll);
	REHAB_ROUTE(Router, POST, "/editor/reimport",         HandleEditorReimport);
	REHAB_ROUTE(Router, POST, "/editor/open_level",       HandleOpenLevel);

	// ---- Gameplay Tags -----------------------------------------------------
	REHAB_ROUTE(Router, GET,  "/gameplay_tags/list",      HandleGameplayTagsList);

	// ---- Blueprint / Widget Blueprint / World Settings / Actor Spawning ----
	REHAB_ROUTE(Router, POST, "/editor/create_blueprint",        HandleCreateBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/create_widget_blueprint", HandleCreateWidgetBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/set_blueprint_defaults",  HandleSetBlueprintDefaults);
	REHAB_ROUTE(Router, POST, "/editor/reparent_blueprint",      HandleReparentBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/set_world_settings",      HandleSetWorldSettings);
	REHAB_ROUTE(Router, POST, "/editor/spawn_actor_in_level",    HandleSpawnActorInLevel);
	REHAB_ROUTE(Router, POST, "/editor/set_component_defaults",  HandleSetComponentDefaults);
	REHAB_ROUTE(Router, GET,  "/editor/get_component_defaults",  HandleGetComponentDefaults);
	REHAB_ROUTE(Router, POST, "/editor/add_component_to_blueprint", HandleAddComponentToBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/remove_component_from_blueprint", HandleRemoveComponentFromBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/create_level",            HandleCreateLevel);
	REHAB_ROUTE(Router, GET,  "/editor/list_actors",             HandleListActors);
	REHAB_ROUTE(Router, POST, "/editor/update_actor",            HandleUpdateActor);
	REHAB_ROUTE(Router, POST, "/editor/delete_actor",            HandleDeleteActor);
	REHAB_ROUTE(Router, POST, "/editor/style_widgets",           HandleStyleWidgets);
	REHAB_ROUTE(Router, POST, "/editor/get_widget_tree",         HandleGetWidgetTree);
	REHAB_ROUTE(Router, POST, "/editor/remove_widget_from_blueprint", HandleRemoveWidgetFromBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/reparent_widget",         HandleReparentWidget);
	REHAB_ROUTE(Router, POST, "/editor/rename_widget",           HandleRenameWidget);
	REHAB_ROUTE(Router, POST, "/editor/compile_widget_blueprint",HandleCompileWidgetBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/create_material",         HandleCreateMaterial);
	REHAB_ROUTE(Router, POST, "/editor/create_color_curve",      HandleCreateColorCurve);
	REHAB_ROUTE(Router, POST, "/editor/create_float_curve",      HandleCreateFloatCurve);
	REHAB_ROUTE(Router, POST, "/editor/read_curve",              HandleReadCurve);
	REHAB_ROUTE(Router, POST, "/editor/read_material",           HandleReadMaterial);

	// ---- Asset deletion ----------------------------------------------------
	REHAB_ROUTE(Router, POST, "/assets/delete",                  HandleAssetsDelete);

	// ---- Batch execution ---------------------------------------------------
	REHAB_ROUTE(Router, POST, "/editor/batch",                   HandleBatch);

	// ---- Enhanced Input ----------------------------------------------------
	REHAB_ROUTE(Router, POST, "/input/configure_imc",            HandleInputConfigureIMC);

	// ---- Editor lifecycle --------------------------------------------------
	REHAB_ROUTE(Router, POST, "/editor/shutdown",                HandleEditorShutdown);
	REHAB_ROUTE(Router, POST, "/editor/build_and_run",           HandleEditorBuildAndRun);
	REHAB_ROUTE(Router, POST, "/editor/kill_and_restart",        HandleEditorKillAndRestart);
	REHAB_ROUTE(Router, POST, "/editor/create_niagara_system",        HandleCreateNiagaraSystem);
	REHAB_ROUTE(Router, POST, "/editor/configure_niagara_system",     HandleConfigureNiagaraSystem);
	REHAB_ROUTE(Router, POST, "/editor/set_niagara_emitter_params",   HandleSetNiagaraEmitterParams);
	REHAB_ROUTE(Router, POST, "/editor/add_widget_to_blueprint",      HandleAddWidgetToBlueprint);
	REHAB_ROUTE(Router, POST, "/assets/import",                       HandleImportAsset);

	// ---- Animation Blueprint / Level Environment ---------------------------
	REHAB_ROUTE(Router, POST, "/editor/configure_anim_blueprint",      HandleConfigureAnimBlueprint);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_two_bone_ik",          HandleAnimAddTwoBoneIK);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_copy_bone",            HandleAnimAddCopyBone);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_hand_ik_retargeting",  HandleAnimAddHandIKRetargeting);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_layered_bone_blend",   HandleAnimAddLayeredBoneBlend);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_sequence_player",      HandleAnimAddSequencePlayer);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_modify_bone",          HandleAnimAddModifyBone);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_look_at",              HandleAnimAddLookAt);
	REHAB_ROUTE(Router, POST, "/editor/anim/add_aim_offset_blend_space", HandleAnimAddAimOffsetBlendSpace);
	REHAB_ROUTE(Router, POST, "/editor/skeleton/add_virtual_bone",     HandleSkeletonAddVirtualBone);
	REHAB_ROUTE(Router, POST, "/editor/anim/delete_node",              HandleAnimDeleteNode);
	REHAB_ROUTE(Router, POST, "/editor/set_level_environment",         HandleSetLevelEnvironment);
	REHAB_ROUTE(Router, POST, "/editor/create_material_instance",     HandleCreateMaterialInstance);
	REHAB_ROUTE(Router, POST, "/editor/create_post_process_material", HandleCreatePostProcessMaterial);
	REHAB_ROUTE(Router, POST, "/editor/spawn_post_process_volume",    HandleSpawnPostProcessVolume);

	// ---- Python scripting ---------------------------------------------------
	REHAB_ROUTE(Router, POST, "/editor/exec_python",                   HandleExecPython);

	// ---- Geometry Script mesh composition -----------------------------------
	REHAB_ROUTE(Router, POST, "/mesh/create",                          HandleMeshCreate);
	REHAB_ROUTE(Router, POST, "/mesh/append_primitive",                HandleMeshAppendPrimitive);
	REHAB_ROUTE(Router, POST, "/mesh/boolean",                         HandleMeshBoolean);
	REHAB_ROUTE(Router, POST, "/mesh/transform",                       HandleMeshTransform);
	REHAB_ROUTE(Router, POST, "/mesh/deform",                          HandleMeshDeform);
	REHAB_ROUTE(Router, POST, "/mesh/remesh",                          HandleMeshRemesh);
	REHAB_ROUTE(Router, POST, "/mesh/bake_static",                     HandleMeshBakeStatic);
	REHAB_ROUTE(Router, POST, "/mesh/delete_handle",                   HandleMeshDeleteHandle);
	REHAB_ROUTE(Router, POST, "/mesh/add_socket",                      HandleMeshAddSocket);

	// ---- Blueprint graph authoring (NGGBlueprintGraph.cpp) -----------------
	REHAB_ROUTE(Router, POST, "/bp/add_node",                          HandleBpAddNode);
	REHAB_ROUTE(Router, POST, "/bp/connect_pins",                      HandleBpConnectPins);
	REHAB_ROUTE(Router, POST, "/bp/compile",                           HandleBpCompile);
	REHAB_ROUTE(Router, POST, "/bp/add_logic",                         HandleBpAddLogic);
	REHAB_ROUTE(Router, GET,  "/bp/read_graph",                        HandleBpReadGraph);
	REHAB_ROUTE(Router, GET,  "/bp/get_selection",                     HandleBpGetSelection);
	REHAB_ROUTE(Router, POST, "/bp/delete_node",                       HandleBpDeleteNode);
	REHAB_ROUTE(Router, POST, "/bp/refresh_all_nodes",                 HandleBpRefreshAllNodes);
	REHAB_ROUTE(Router, POST, "/bp/add_interface",                     HandleBpAddInterface);
	REHAB_ROUTE(Router, POST, "/bp/implement_interface_function",      HandleBpImplementInterfaceFunction);
	REHAB_ROUTE(Router, POST, "/bp/lint",                              HandleBpLint);
	REHAB_ROUTE(Router, POST, "/bp/lint_project",                      HandleBpLintProject);
	REHAB_ROUTE(Router, POST, "/bp/create_variable",                   HandleBpCreateVariable);
	REHAB_ROUTE(Router, GET,  "/bp/list_variables",                    HandleBpListVariables);
	REHAB_ROUTE(Router, POST, "/bp/create_function",                   HandleBpCreateFunction);
	REHAB_ROUTE(Router, POST, "/bp/create_macro",                      HandleBpCreateMacro);
	REHAB_ROUTE(Router, POST, "/bp/delete_function",                   HandleBpDeleteFunction);
	REHAB_ROUTE(Router, POST, "/bp/delete_variable",                   HandleBpDeleteVariable);
	REHAB_ROUTE(Router, POST, "/bp/heal_world_context",                HandleBpHealWorldContext);

	// ---- Asset creation: user-defined structs / enums / interfaces / anim BPs ----
	REHAB_ROUTE(Router, POST, "/asset/create_blueprint_struct",        HandleCreateBlueprintStruct);
	REHAB_ROUTE(Router, POST, "/asset/create_blueprint_enum",          HandleCreateBlueprintEnum);
	REHAB_ROUTE(Router, POST, "/asset/create_blueprint_interface",     HandleCreateBlueprintInterface);
	REHAB_ROUTE(Router, POST, "/asset/create_anim_blueprint",          HandleCreateAnimBlueprint);

	// ---- Behavior Tree authoring (NGGBehaviorTree.cpp) — Phase 1: asset creation ----
	REHAB_ROUTE(Router, POST, "/bt/create_tree",                       HandleBtCreateTree);
	REHAB_ROUTE(Router, POST, "/bt/create_blackboard",                 HandleBtCreateBlackboard);
	REHAB_ROUTE(Router, POST, "/bt/add_blackboard_keys",               HandleBtAddBlackboardKeys);
	// ---- Behavior Tree authoring (NGGBehaviorTree.cpp) — Phase 2: graph wiring ----
	REHAB_ROUTE(Router, POST, "/bt/add_logic",                         HandleBtAddLogic);
	REHAB_ROUTE(Router, GET,  "/bt/read_tree",                         HandleBtReadTree);

	// ---- StateTree authoring (NGGStateTree.cpp) ----
	REHAB_ROUTE(Router, GET,  "/statetree/read_tree",                  HandleStateTreeReadTree);
	REHAB_ROUTE(Router, POST, "/statetree/repoint_node",              HandleStateTreeRepointNode);

	// ---- Gameplay Ability System (NGGGameplayAbilitySystem.cpp) ----
	REHAB_ROUTE(Router, POST, "/gas/setup_actor",         HandleGasSetupActor);
	REHAB_ROUTE(Router, POST, "/gas/create_attribute_set",HandleGasCreateAttributeSet);
	REHAB_ROUTE(Router, POST, "/gas/create_ability",      HandleGasCreateAbility);
	REHAB_ROUTE(Router, POST, "/gas/create_effect",       HandleGasCreateEffect);
	REHAB_ROUTE(Router, POST, "/gas/configure_asc",       HandleGasConfigureAsc);
	REHAB_ROUTE(Router, GET,  "/gas/read_setup",          HandleGasReadSetup);
}

void FNGGHttpServer::UnregisterRoutes()
{
	if (Router.IsValid())
	{
		for (FHttpRouteHandle& Handle : RouteHandles)
		{
			if (Handle.IsValid())
			{
				Router->UnbindRoute(Handle);
			}
		}
	}
	RouteHandles.Empty();
}

// ============================================================================
// Dynamic route registration (extension API)
// ============================================================================

void FNGGHttpServer::RegisterDynamicRoute(
	const FString& Path,
	EHttpServerRequestVerbs Verb,
	FHttpRequestHandler Handler)
{
	if (!Router.IsValid())
	{
		UE_LOG(LogNGGBridge, Warning,
			TEXT("RegisterDynamicRoute('%s'): server not running — route ignored."), *Path);
		return;
	}

	// Wrap with the same auth check as REHAB_ROUTE so extension handlers are
	// protected identically to built-in routes.
	FString CapturedToken = AuthToken;
	const bool bCapturedAllowRemote = bAllowRemoteClients;
	FHttpRequestHandler WrappedHandler = FHttpRequestHandler::CreateLambda(
		[CapturedToken, bCapturedAllowRemote, Handler](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete) -> bool
		{
			if (Req.Body.Num() > NGG_MaxRequestBodyBytes)
			{
				OnComplete(JsonError(413, TEXT("Request body too large (max 16 MiB)")));
				return true;
			}
			if (!bCapturedAllowRemote && !IsLoopbackPeer(Req))
			{
				OnComplete(JsonError(403, TEXT("Forbidden: this bridge serves loopback clients only. Set [UnrealNGGMCP] AllowRemoteClients=true in DefaultEngine.ini to permit remote access.")));
				return true;
			}
			if (!CapturedToken.IsEmpty())
			{
				const TArray<FString>* AuthValues = Req.Headers.Find(TEXT("authorization"));
				const FString Expected = TEXT("Bearer ") + CapturedToken;
				if (!AuthValues || AuthValues->Num() == 0 || !ConstantTimeEquals((*AuthValues)[0], Expected))
				{
					OnComplete(JsonError(401, TEXT("Unauthorized: missing or invalid Authorization header")));
					return true;
				}
			}
			return Handler.Execute(Req, OnComplete);
		}
	);

	FHttpRouteHandle Handle = Router->BindRoute(FHttpPath(Path), Verb, WrappedHandler);
	if (Handle.IsValid())
	{
		RouteHandles.Add(Handle);
	}
}

// ============================================================================
// Response helpers
// ============================================================================

TUniquePtr<FHttpServerResponse> FNGGHttpServer::JsonOk(const FString& JsonBody)
{
	auto Response = FHttpServerResponse::Create(JsonBody, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::Ok;
	return Response;
}

TUniquePtr<FHttpServerResponse> FNGGHttpServer::JsonCreated(const FString& JsonBody)
{
	auto Response = FHttpServerResponse::Create(JsonBody, TEXT("application/json"));
	Response->Code = EHttpServerResponseCodes::Created;
	return Response;
}

TUniquePtr<FHttpServerResponse> FNGGHttpServer::JsonError(int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("error"), Message);

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = static_cast<EHttpServerResponseCodes>(Code);
	return Response;
}

// ============================================================================
// Auth helpers
// ============================================================================

bool FNGGHttpServer::IsLoopbackPeer(const FHttpServerRequest& Req)
{
	if (!Req.PeerAddress.IsValid() || !Req.PeerAddress->IsValid())
	{
		// No resolvable peer: the request cannot be proven local, so it is not.
		return false;
	}

	// Network byte order, so the leading byte is the first octet of the address.
	const TArray<uint8> Raw = Req.PeerAddress->GetRawIp();

	// IPv4 — the whole 127.0.0.0/8 block is loopback, not just 127.0.0.1.
	if (Raw.Num() == 4)
	{
		return Raw[0] == 127;
	}

	if (Raw.Num() == 16)
	{
		// IPv4-mapped IPv6 (::ffff:a.b.c.d). A dual-stack listener reports IPv4
		// peers in this form, so the embedded IPv4 address is what must be tested
		// — the outer address is not ::1 and would otherwise be read as remote.
		static const uint8 V4MappedPrefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
		if (FMemory::Memcmp(Raw.GetData(), V4MappedPrefix, sizeof(V4MappedPrefix)) == 0)
		{
			return Raw[12] == 127;
		}

		// ::1 — fifteen zero bytes followed by 1.
		for (int32 Idx = 0; Idx < 15; ++Idx)
		{
			if (Raw[Idx] != 0)
			{
				return false;
			}
		}
		return Raw[15] == 1;
	}

	// Unknown address family — refuse rather than guess.
	return false;
}

bool FNGGHttpServer::ConstantTimeEquals(const FString& A, const FString& B)
{
	// Compare lengths first. Differing lengths are not a secret (and a mismatch
	// here cannot be masked), so an early-out is acceptable.
	if (A.Len() != B.Len())
	{
		return false;
	}

	// XOR-accumulate over every TCHAR without short-circuiting so the loop's
	// duration does not depend on where the first mismatch occurs.
	uint32 Diff = 0;
	const TCHAR* APtr = *A;
	const TCHAR* BPtr = *B;
	for (int32 Idx = 0; Idx < A.Len(); ++Idx)
	{
		Diff |= static_cast<uint32>(APtr[Idx] ^ BPtr[Idx]);
	}
	return Diff == 0;
}

// ============================================================================
// Request parsing helpers
// ============================================================================

bool FNGGHttpServer::ParseJsonBody(const FHttpServerRequest& Req,
	TSharedPtr<FJsonObject>& OutObj, FString& OutError)
{
	// UE5 stores the raw body as TArray<uint8>
	if (Req.Body.Num() == 0)
	{
		OutError = TEXT("Request body is empty");
		return false;
	}

	// Reject oversized bodies before allocating the FString copy, to bound memory.
	if (Req.Body.Num() > NGG_MaxRequestBodyBytes)
	{
		OutError = FString::Printf(
			TEXT("Request body too large: %d bytes (max %d)"),
			Req.Body.Num(), NGG_MaxRequestBodyBytes);
		return false;
	}

	// Null-terminate before converting: Req.Body has no trailing null,
	// so UTF8_TO_TCHAR without length would read past the buffer end and
	// produce "Unexpected additional input" parse errors.
	const FUTF8ToTCHAR Converter(
		reinterpret_cast<const ANSICHAR*>(Req.Body.GetData()), Req.Body.Num());
	const FString BodyStr(Converter.Length(), Converter.Get());

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to parse JSON body: %s"), *Reader->GetErrorMessage());
		return false;
	}
	return true;
}

FString FNGGHttpServer::GetQueryParam(const FHttpServerRequest& Req, const FString& Key)
{
	const FString* Value = Req.QueryParams.Find(Key);
	return Value ? *Value : FString();
}

// ============================================================================
// UObject reflection helpers
// ============================================================================

/**
 * Inner SEH-guarded resolver — no C++ objects with destructors allowed here.
 * Returns a raw UObject* or nullptr.  On Windows, SEH catches access violations
 * from corrupt FObjectPtr handles.
 */
#if PLATFORM_WINDOWS
#pragma warning(push)
#pragma warning(disable: 4611) // interaction between '_setjmp' and C++ object destruction
static UObject* SafeResolveObjectPtr_Inner(void* ValuePtr, const FObjectProperty* ObjProp)
{
	UObject* Result = nullptr;
	__try
	{
		Result = ObjProp->GetObjectPropertyValue(ValuePtr);
	}
	__except(1) // EXCEPTION_EXECUTE_HANDLER == 1
	{
		Result = nullptr;
	}
	return Result;
}
#pragma warning(pop)
#endif

/**
 * Safely resolve an FObjectProperty value to a UObject*.
 * Returns nullptr if the handle is corrupt or unresolvable.
 */
static UObject* SafeGetObjectPropertyValue(
	const FObjectProperty* ObjProp, const void* ContainerPtr)
{
	// ContainerPtrToValuePtr requires non-const; safe because we only read.
	void* ValuePtr = const_cast<FObjectProperty*>(ObjProp)->ContainerPtrToValuePtr<void>(
		const_cast<void*>(ContainerPtr));
	if (!ValuePtr) return nullptr;

#if PLATFORM_WINDOWS
	UObject* Obj = SafeResolveObjectPtr_Inner(ValuePtr, ObjProp);
#else
	UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
#endif

	return Obj;
}

TSharedPtr<FJsonValue> FNGGHttpServer::PropertyToJson(
	const FProperty* Prop, const void* ContainerPtr)
{
	if (!Prop || !ContainerPtr)
	{
		return MakeShared<FJsonValueNull>();
	}

	if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FInt8Property* Int8Prop = CastField<FInt8Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(Int8Prop->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FInt16Property* Int16Prop = CastField<FInt16Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(Int16Prop->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		return MakeShared<FJsonValueNumber>(IntProp->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(
			Int64Prop->GetPropertyValue_InContainer(ContainerPtr)));
	}
	if (const FUInt16Property* UInt16Prop = CastField<FUInt16Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(UInt16Prop->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FUInt32Property* UInt32Prop = CastField<FUInt32Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(UInt32Prop->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FUInt64Property* UInt64Prop = CastField<FUInt64Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(
			UInt64Prop->GetPropertyValue_InContainer(ContainerPtr)));
	}
	if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		return MakeShared<FJsonValueNumber>(FloatProp->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(StrProp->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(
			NameProp->GetPropertyValue_InContainer(ContainerPtr).ToString());
	}
	if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(
			TextProp->GetPropertyValue_InContainer(ContainerPtr).ToString());
	}
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		// Could be an enum or a raw byte
		if (ByteProp->Enum)
		{
			const int64 EnumVal = ByteProp->GetPropertyValue_InContainer(ContainerPtr);
			return MakeShared<FJsonValueString>(
				ByteProp->Enum->GetNameStringByValue(EnumVal));
		}
		return MakeShared<FJsonValueNumber>(ByteProp->GetPropertyValue_InContainer(ContainerPtr));
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		const int64 EnumVal = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(
			EnumProp->ContainerPtrToValuePtr<void>(ContainerPtr));
		return MakeShared<FJsonValueString>(EnumProp->GetEnum()->GetNameStringByValue(EnumVal));
	}
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		// Use the SEH-guarded wrapper for safety against corrupt object handles.
		const UObject* RefObj = SafeGetObjectPropertyValue(ObjProp, ContainerPtr);
		if (RefObj)
		{
			return MakeShared<FJsonValueString>(RefObj->GetPathName());
		}
		return MakeShared<FJsonValueNull>();
	}
	if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		const FSoftObjectPtr SoftPtr = SoftProp->GetPropertyValue_InContainer(ContainerPtr);
		return MakeShared<FJsonValueString>(SoftPtr.ToString());
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		// FGameplayTag → emit tag string
		// TBaseStructure<FGameplayTag>::Get() works because FGameplayTag is a USTRUCT.
		static const UScriptStruct* GameplayTagStruct =
			TBaseStructure<FGameplayTag>::Get();
		if (StructProp->Struct == GameplayTagStruct)
		{
			const FGameplayTag& Tag = *StructProp->ContainerPtrToValuePtr<FGameplayTag>(ContainerPtr);
			return MakeShared<FJsonValueString>(Tag.ToString());
		}

		// Generic struct — recurse
		TSharedPtr<FJsonObject> StructObj = MakeShared<FJsonObject>();
		const void* StructDataPtr = StructProp->ContainerPtrToValuePtr<void>(ContainerPtr);
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			StructObj->SetField(It->GetName(), PropertyToJson(*It, StructDataPtr));
		}
		return MakeShared<FJsonValueObject>(StructObj);
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ContainerPtr));
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		JsonArray.Reserve(ArrayHelper.Num());
		for (int32 i = 0; i < ArrayHelper.Num(); ++i)
		{
			JsonArray.Add(PropertyToJson(ArrayProp->Inner, ArrayHelper.GetRawPtr(i)));
		}
		return MakeShared<FJsonValueArray>(JsonArray);
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		// Emit as array of {key, value} objects for simplicity
		FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(ContainerPtr));
		TArray<TSharedPtr<FJsonValue>> Pairs;
		for (int32 i = 0; i < MapHelper.Num(); ++i)
		{
			if (!MapHelper.IsValidIndex(i)) continue;
			TSharedPtr<FJsonObject> Pair = MakeShared<FJsonObject>();
			// Pass the pair pointer — PropertyToJson calls ContainerPtrToValuePtr
			// which adds the property offset.  GetKeyPtr/GetValuePtr already add
			// the offset, which would double-offset and read garbage.
			const uint8* PairPtr = MapHelper.GetPairPtr(i);
			Pair->SetField(TEXT("key"),   PropertyToJson(MapProp->KeyProp,   PairPtr));
			Pair->SetField(TEXT("value"), PropertyToJson(MapProp->ValueProp, PairPtr));
			Pairs.Add(MakeShared<FJsonValueObject>(Pair));
		}
		return MakeShared<FJsonValueArray>(Pairs);
	}

	// Fallback: export to string via UE's built-in text export
	FString ExportedStr;
	Prop->ExportTextItem_Direct(ExportedStr, Prop->ContainerPtrToValuePtr<void>(ContainerPtr),
		nullptr, nullptr, PPF_None);
	return MakeShared<FJsonValueString>(ExportedStr);
}

TSharedPtr<FJsonObject> FNGGHttpServer::UObjectToJson(UObject* Obj)
{
	if (!IsValid(Obj))
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("_class"),    Obj->GetClass()->GetName());
	Result->SetStringField(TEXT("_path"),     Obj->GetPathName());
	Result->SetStringField(TEXT("_outermost"),Obj->GetOutermost()->GetName());

	for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
	{
		const FProperty* Prop = *It;
		// Only export properties that are exposed to Blueprint/editor
		if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}
		Result->SetField(Prop->GetName(), PropertyToJson(Prop, Obj));
	}

	return Result;
}

FString FNGGHttpServer::SetPropertyFromJson(
	UObject* Obj, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value)
{
	if (!IsValid(Obj))
	{
		return TEXT("Target object is null or invalid");
	}

	// Special case: setting BaseColor on a UMaterial via expression graph.
	// UMaterial has no UPROPERTY named "BaseColor" — it lives in the node graph.
	if (UMaterial* Mat = Cast<UMaterial>(Obj))
	{
		if (PropertyName.Equals(TEXT("BaseColor"), ESearchCase::IgnoreCase))
		{
			FLinearColor Color = FLinearColor::Red;
			if (Value->Type == EJson::Object && Value->AsObject().IsValid())
			{
				const TSharedPtr<FJsonObject>& ColorObj = Value->AsObject();
				double R = 0, G = 0, B = 0, A = 1;
				ColorObj->TryGetNumberField(TEXT("R"), R);
				ColorObj->TryGetNumberField(TEXT("G"), G);
				ColorObj->TryGetNumberField(TEXT("B"), B);
				ColorObj->TryGetNumberField(TEXT("A"), A);
				Color = FLinearColor((float)R, (float)G, (float)B, (float)A);
			}
			else if (Value->Type == EJson::String)
			{
				FLinearColor Parsed;
				if (Parsed.InitFromString(Value->AsString()))
				{
					Color = Parsed;
				}
			}

			UMaterialEditorOnlyData* EdData = Mat->GetEditorOnlyData();

			// If a VectorParameter is already wired to BaseColor, update its default.
			if (UMaterialExpressionVectorParameter* VecParam =
				Cast<UMaterialExpressionVectorParameter>(EdData->BaseColor.Expression))
			{
				VecParam->DefaultValue = Color;
				UMaterialEditingLibrary::RecompileMaterial(Mat);
				Mat->MarkPackageDirty();
				return FString();
			}

			// If a Constant3Vector is already wired, update it in-place.
			if (UMaterialExpressionConstant3Vector* Existing =
				Cast<UMaterialExpressionConstant3Vector>(EdData->BaseColor.Expression))
			{
				Existing->Constant = Color;
				UMaterialEditingLibrary::RecompileMaterial(Mat);
				Mat->MarkPackageDirty();
				return FString();
			}

			// Nothing wired — create a new Constant3Vector and connect it.
			UMaterialExpressionConstant3Vector* ColorExpr =
				NewObject<UMaterialExpressionConstant3Vector>(Mat);
			ColorExpr->Constant = Color;
			Mat->GetExpressionCollection().AddExpression(ColorExpr);
			EdData->BaseColor.Expression = ColorExpr;
			UMaterialEditingLibrary::RecompileMaterial(Mat);
			Mat->MarkPackageDirty();
			return FString();
		}
	}

	FProperty* Prop = Obj->GetClass()->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		return FString::Printf(TEXT("Property '%s' not found on class '%s'"),
			*PropertyName, *Obj->GetClass()->GetName());
	}

	// Use ExportText round-trip for simple cases; for arrays/structs fall back to
	// FJsonObjectConverter if the project ADL types are available.
	void* PropPtr = Prop->ContainerPtrToValuePtr<void>(Obj);

	if (Value->Type == EJson::String)
	{
		const FString StrVal = Value->AsString();
		Prop->ImportText_Direct(*StrVal, PropPtr, Obj, PPF_None);
		return FString();
	}
	if (Value->Type == EJson::Number)
	{
		const FString StrVal = FString::SanitizeFloat(Value->AsNumber());
		Prop->ImportText_Direct(*StrVal, PropPtr, Obj, PPF_None);
		return FString();
	}
	if (Value->Type == EJson::Boolean)
	{
		const FString StrVal = Value->AsBool() ? TEXT("true") : TEXT("false");
		Prop->ImportText_Direct(*StrVal, PropPtr, Obj, PPF_None);
		return FString();
	}

	// Complex value — convert to JSON string and import via text.
	// In UE5.7, FJsonSerializer::Serialize requires a typed dispatch: the
	// two-argument (FJsonValue, Writer) overload was removed.  Branch on type.
	FString JsonStr;
	if (Value->Type == EJson::Object && Value->AsObject().IsValid())
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
		FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
	}
	else if (Value->Type == EJson::Array)
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
		FJsonSerializer::Serialize(Value->AsArray(), Writer);
	}
	else
	{
		// Null or any other unexpected type — use the string representation
		JsonStr = Value->AsString();
	}
	Prop->ImportText_Direct(*JsonStr, PropPtr, Obj, PPF_None);
	return FString();
}
