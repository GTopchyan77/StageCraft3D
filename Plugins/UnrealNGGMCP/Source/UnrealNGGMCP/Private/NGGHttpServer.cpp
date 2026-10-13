// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGHttpServer.cpp
//
// Full implementation of the REST bridge between Claude Code (via MCP) and the
// UE5 Editor asset system.  Every public endpoint is implemented here.
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
// ---- JSON ------------------------------------------------------------------
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
// ---- Asset tools -----------------------------------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/DataAssetFactory.h"
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
#include "EditorReimportHandler.h"// FReimportManager
// ---- UObject reflection ----------------------------------------------------
#include "UObject/UnrealType.h"
#include "UObject/PropertyIterator.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
// ---- Gameplay Tags ---------------------------------------------------------
#include "GameplayTagsManager.h"
// ---- Engine ----------------------------------------------------------------
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "ObjectTools.h"
#include "Selection.h"  // USelection — full definition for DeselectAll/Deselect
#include "EngineUtils.h"          // TActorIterator
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
// ---- Enhanced Input --------------------------------------------------------
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "EnhancedActionKeyMapping.h"
// ---- Niagara particle system -----------------------------------------------
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
// ---- Texture import (legacy factory, avoids Interchange crash) -------------
#include "Factories/TextureFactory.h"
#include "Engine/Texture2D.h"
// ---- Material creation -----------------------------------------------------
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionRound.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionIf.h"
#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveFloat.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
// ---- Blueprint component CDO helpers ---------------------------------------
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// ---- Animation Blueprint editor APIs ---------------------------------------
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Factories/AnimBlueprintFactory.h"
// ---- User-defined struct / enum / interface authoring ----------------------
// (UUserDefinedStruct itself is already pulled in via StructUtils/UserDefinedStruct.h above)
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
// FStructVariableDescription full definition (StructureEditorUtils.h forward-declares it)
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/Interface.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateGraph.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimationStateMachineSchema.h"
// ---- Two Bone IK authoring (NGGHttpServer.cpp HandleAnimAddTwoBoneIK) ----
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_Root.h"
#include "BoneControllers/AnimNode_TwoBoneIK.h"
#include "Animation/AnimTypes.h"
// ---- Copy Bone + Hand IK Retargeting (HandleAnimAddCopyBone / HandleAnimAddHandIKRetargeting) ----
#include "AnimGraphNode_CopyBone.h"
#include "AnimGraphNode_HandIKRetargeting.h"
#include "BoneControllers/AnimNode_CopyBone.h"
#include "BoneControllers/AnimNode_HandIKRetargeting.h"
// ---- Layered Bone Blend + Sequence Player splice (HandleAnimAddLayeredBoneBlend / HandleAnimAddSequencePlayer) ----
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimData/BoneMaskFilter.h"
#include "Animation/AnimNode_SequencePlayer.h"
// ---- Modify Bone (HandleAnimAddModifyBone) ----
#include "AnimGraphNode_ModifyBone.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
// ---- Look At (HandleAnimAddLookAt) ----
#include "AnimGraphNode_LookAt.h"
#include "BoneControllers/AnimNode_LookAt.h"
// ---- Aim Offset / Rotation Offset Blend Space (HandleAnimAddAimOffsetBlendSpace) ----
#include "AnimGraphNode_RotationOffsetBlendSpace.h"
#include "AnimNodes/AnimNode_RotationOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
// ---- Python scripting ------------------------------------------------------
#include "IPythonScriptPlugin.h"
// ---- Level environment actors ----------------------------------------------
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
// ============================================================================
// Cross-TU helpers (defined in NGGBlueprintGraph.cpp, used by handlers below)
// ============================================================================
namespace NGGBpPriv
{
	bool ResolveVariablePinType(const FString& TypeStr, FEdGraphPinType& OutType, FString& OutError);
}

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
// Property-set helper: routes Static/SkeletalMesh assignments through the
// component's setter so internal caches stay in sync. Raw ImportText on these
// pointer properties bypasses UStaticMeshComponent::SetStaticMesh() and trips
// UStaticMeshComponent::OutdatedKnownStaticMeshDetected on the next thumbnail
// refresh. Returns true if the property was handled specially; out-params
// indicate success vs failure. Callers fall back to ImportText_Direct if the
// function returns false.
// ============================================================================
static bool TrySetMeshPropertyViaSetter(
	UObject* Target, const FName& PropFName, const FString& ValueStr,
	FString& OutError, bool& bOutSucceeded)
{
	bOutSucceeded = false;
	if (auto* SMC = Cast<UStaticMeshComponent>(Target); SMC && PropFName == FName(TEXT("StaticMesh")))
	{
		UStaticMesh* Mesh = ValueStr.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *ValueStr);
		if (!Mesh && !ValueStr.IsEmpty())
		{
			OutError = FString::Printf(TEXT("StaticMesh not found at '%s'"), *ValueStr);
			return true;
		}
		SMC->SetStaticMesh(Mesh);
		bOutSucceeded = true;
		return true;
	}
	if (auto* SkMC = Cast<USkeletalMeshComponent>(Target);
		SkMC && (PropFName == FName(TEXT("SkeletalMesh")) || PropFName == FName(TEXT("SkeletalMeshAsset"))))
	{
		USkeletalMesh* Mesh = ValueStr.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *ValueStr);
		if (!Mesh && !ValueStr.IsEmpty())
		{
			OutError = FString::Printf(TEXT("SkeletalMesh not found at '%s'"), *ValueStr);
			return true;
		}
		SkMC->SetSkeletalMeshAsset(Mesh);
		bOutSucceeded = true;
		return true;
	}
	return false;
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

	// Read auth token from DefaultEngine.ini [UnrealNGGMCP] AuthToken=...
	// Empty string means auth is disabled.
	GConfig->GetString(TEXT("UnrealNGGMCP"), TEXT("AuthToken"), AuthToken, GEngineIni);
	if (!AuthToken.IsEmpty())
	{
		UE_LOG(LogNGGBridge, Log, TEXT("NGGHttpServer: Auth token configured — requests must include 'Authorization: Bearer <token>'"));
	}
	else
	{
		// FHttpServerModule has no bind-address API, so the listener is reachable
		// on ALL network interfaces — not just loopback. With no token, anyone who
		// can reach this port can drive the editor. Warn loudly and disable the
		// arbitrary-code / lifecycle routes (see HandleExecPython) until a token
		// is set.
		UE_LOG(LogNGGBridge, Warning,
			TEXT("NGGHttpServer: UNAUTHENTICATED — no AuthToken is set, and the bridge is reachable on ALL network interfaces (FHttpServerModule cannot bind loopback-only). ")
			TEXT("exec_python and other arbitrary-code / lifecycle routes are DISABLED until you set a token in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token>."));
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
	FHttpRequestHandler WrappedHandler = FHttpRequestHandler::CreateLambda(
		[CapturedToken, Handler](const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete) -> bool
		{
			if (Req.Body.Num() > NGG_MaxRequestBodyBytes)
			{
				OnComplete(JsonError(413, TEXT("Request body too large (max 16 MiB)")));
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

	// Resolve PropertyName as a dot-separated path with optional array indices,
	// e.g. "Entries[0].DolphinCompanion.bSpawnCompanion". A plain name with no
	// dots/brackets resolves exactly as before (single FindFProperty on Obj).
	FProperty* Prop = nullptr;
	void* PropPtr = nullptr;
	{
		TArray<FString> PathTokens;
		PropertyName.ParseIntoArray(PathTokens, TEXT("."));
		if (PathTokens.Num() == 0)
		{
			return FString::Printf(TEXT("Empty property path"));
		}

		void* CurrentContainer = Obj;
		UStruct* CurrentStruct = Obj->GetClass();

		for (int32 TokenIdx = 0; TokenIdx < PathTokens.Num(); ++TokenIdx)
		{
			const FString& Token = PathTokens[TokenIdx];
			const bool bIsLast = (TokenIdx == PathTokens.Num() - 1);

			FString FieldName = Token;
			int32 ArrayIndex = INDEX_NONE;
			const int32 OpenBracket = Token.Find(TEXT("["));
			if (OpenBracket != INDEX_NONE)
			{
				const int32 CloseBracket = Token.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, OpenBracket);
				if (CloseBracket != INDEX_NONE && CloseBracket > OpenBracket)
				{
					FieldName = Token.Left(OpenBracket);
					ArrayIndex = FCString::Atoi(*Token.Mid(OpenBracket + 1, CloseBracket - OpenBracket - 1));
				}
			}

			FProperty* FieldProp = FindFProperty<FProperty>(CurrentStruct, *FieldName);
			if (!FieldProp)
			{
				return FString::Printf(TEXT("Property '%s' not found on '%s' (full path '%s')"),
					*FieldName, *CurrentStruct->GetName(), *PropertyName);
			}

			if (ArrayIndex != INDEX_NONE)
			{
				FArrayProperty* ArrProp = CastField<FArrayProperty>(FieldProp);
				if (!ArrProp)
				{
					return FString::Printf(TEXT("Property '%s' is not an array (full path '%s')"),
						*FieldName, *PropertyName);
				}

				void* ArrayValuePtr = ArrProp->ContainerPtrToValuePtr<void>(CurrentContainer);
				FScriptArrayHelper ArrayHelper(ArrProp, ArrayValuePtr);
				// Removal, symmetric to the grow-by-one case below: targeting the trailing element
				// directly (e.g. "Entries[4]", no further ".Field") with this sentinel string removes
				// it. A JSON null would be the more natural signal, but it doesn't reliably survive
				// the MCP tool-call transport (observed: a null "value" field arrives as if absent),
				// so a magic string is used instead. Only the last element can be removed this way,
				// so this can't create sparse arrays.
				if (bIsLast && Value->Type == EJson::String
					&& Value->AsString() == TEXT("__NGG_REMOVE_ARRAY_ELEMENT__")
					&& ArrayIndex == ArrayHelper.Num() - 1)
				{
					ArrayHelper.RemoveValues(ArrayIndex, 1);
					return FString();
				}
				// Auto-grow by exactly one element when the index is the natural next append slot
				// (ArrayIndex == Num()) — lets callers append a new struct-array entry via e.g.
				// "Entries[4].DisplayName" without a separate resize call. Indices further out of
				// range still error, so this can't create sparse/garbage elements.
				if (ArrayIndex == ArrayHelper.Num())
				{
					ArrayHelper.AddValue();
				}
				if (!ArrayHelper.IsValidIndex(ArrayIndex))
				{
					return FString::Printf(TEXT("Index %d out of range for '%s' (num=%d, full path '%s')"),
						ArrayIndex, *FieldName, ArrayHelper.Num(), *PropertyName);
				}
				void* ElementPtr = ArrayHelper.GetRawPtr(ArrayIndex);

				if (bIsLast)
				{
					Prop = ArrProp->Inner;
					PropPtr = ElementPtr;
					break;
				}

				FStructProperty* InnerStruct = CastField<FStructProperty>(ArrProp->Inner);
				if (!InnerStruct)
				{
					return FString::Printf(TEXT("Element of array '%s' is not a struct, cannot navigate further (full path '%s')"),
						*FieldName, *PropertyName);
				}
				CurrentContainer = ElementPtr;
				CurrentStruct = InnerStruct->Struct;
				continue;
			}

			if (bIsLast)
			{
				Prop = FieldProp;
				PropPtr = FieldProp->ContainerPtrToValuePtr<void>(CurrentContainer);
				break;
			}

			FStructProperty* StructProp = CastField<FStructProperty>(FieldProp);
			if (!StructProp)
			{
				return FString::Printf(TEXT("Property '%s' is not a struct, cannot navigate into it (full path '%s')"),
					*FieldName, *PropertyName);
			}
			CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			CurrentStruct = StructProp->Struct;
		}
	}

	if (!Prop || !PropPtr)
	{
		return FString::Printf(TEXT("Could not resolve property path '%s' on class '%s'"),
			*PropertyName, *Obj->GetClass()->GetName());
	}

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

// ============================================================================
// Handler: GET /health
// ============================================================================

bool FNGGHttpServer::HandleHealth(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("status"),  TEXT("ok"));
	Body->SetStringField(TEXT("project"), FApp::GetProjectName());
	Body->SetNumberField(TEXT("port"),    static_cast<double>(Port));

	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	OnComplete(JsonOk(BodyStr));
	return true;
}

// ============================================================================
// Handler: GET /project_info
// Returns ground-truth project + engine paths from inside the running editor,
// so the sidecar can stop caching them at startup and "follow" whichever
// editor is currently up on this port.
// ============================================================================

bool FNGGHttpServer::HandleProjectInfo(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	// FApp/FPaths and the IFileManager directory scan below touch engine globals
	// and the filesystem, so marshal onto the Game Thread like every other handler
	// rather than running this on the HTTP worker thread.
	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback]()
	{
		const FString ProjectName  = FApp::GetProjectName();
		const FString UProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		// FPaths::EngineDir() returns "<EngineRoot>/Engine/" — strip the trailing separator
		// and the "Engine" leaf so we hand the sidecar the install root it expects.
		FString EngineRoot = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		if (EngineRoot.EndsWith(TEXT("/")) || EngineRoot.EndsWith(TEXT("\\")))
		{
			EngineRoot = EngineRoot.LeftChop(1);
		}
		EngineRoot = FPaths::GetPath(EngineRoot);

		// Default targets — same convention used by HandleEditorBuildAndRun.
		FString EditorTarget = FString::Printf(TEXT("%sEditor"), *ProjectName);
		FString GameTarget   = ProjectName;
		FString ServerTarget;
		FString ClientTarget;

		// Scan Source/*.Target.cs for actual target names so we don't guess wrong on renamed targets.
		const FString SourceDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
		TArray<FString> TargetFiles;
		IFileManager::Get().FindFiles(TargetFiles, *FPaths::Combine(SourceDir, TEXT("*.Target.cs")), true, false);
		for (const FString& File : TargetFiles)
		{
			FString Stem = File;
			Stem.RemoveFromEnd(TEXT(".Target.cs"));
			const FString Lower = Stem.ToLower();
			if      (Lower.EndsWith(TEXT("editor"))) EditorTarget = Stem;
			else if (Lower.EndsWith(TEXT("server"))) ServerTarget = Stem;
			else if (Lower.EndsWith(TEXT("client"))) ClientTarget = Stem;
			else                                     GameTarget   = Stem;
		}

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("project_name"),  ProjectName);
		Body->SetStringField(TEXT("uproject_path"), UProjectPath);
		Body->SetStringField(TEXT("engine_dir"),    EngineRoot);
		Body->SetStringField(TEXT("editor_target"), EditorTarget);
		Body->SetStringField(TEXT("game_target"),   GameTarget);
		if (!ServerTarget.IsEmpty()) Body->SetStringField(TEXT("server_target"), ServerTarget);
		if (!ClientTarget.IsEmpty()) Body->SetStringField(TEXT("client_target"), ClientTarget);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: GET /assets/list?path=/Game/Data
// ============================================================================

bool FNGGHttpServer::HandleAssetsList(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ContentPath = GetQueryParam(Req, TEXT("path"));

	// Fallback: read "path" from JSON body (needed when called from batch mode).
	if (ContentPath.IsEmpty() && Req.Body.Num() > 0)
	{
		TSharedPtr<FJsonObject> BodyObj;
		FString Err;
		if (ParseJsonBody(Req, BodyObj, Err) && BodyObj.IsValid())
		{
			BodyObj->TryGetStringField(TEXT("path"), ContentPath);
		}
	}

	if (ContentPath.IsEmpty())
	{
		ContentPath = TEXT("/Game");
	}

	// Capture for lambda
	FHttpResultCallback Callback = OnComplete;
	FString SearchPath = ContentPath;

	AsyncTask(ENamedThreads::GameThread, [Callback, SearchPath]()
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();

		TArray<FAssetData> Assets;
		AR.GetAssetsByPath(FName(*SearchPath), Assets, /*bRecursive=*/false);

		TArray<TSharedPtr<FJsonValue>> AssetArray;
		AssetArray.Reserve(Assets.Num());

		for (const FAssetData& AD : Assets)
		{
			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("path"),       AD.GetObjectPathString());
			AssetObj->SetStringField(TEXT("name"),       AD.AssetName.ToString());
			AssetObj->SetStringField(TEXT("class"),      AD.AssetClassPath.GetAssetName().ToString());
			AssetObj->SetStringField(TEXT("package"),    AD.PackageName.ToString());
			AssetArray.Add(MakeShared<FJsonValueObject>(AssetObj));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("path"),  SearchPath);
		Root->SetNumberField(TEXT("count"), static_cast<double>(AssetArray.Num()));
		Root->SetArrayField(TEXT("assets"), AssetArray);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: GET /assets/get?path=/Game/Data/Exercises/DA_ADL01
// ============================================================================

bool FNGGHttpServer::HandleAssetsGet(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString AssetPath = GetQueryParam(Req, TEXT("path"));

	// Fallback: read "path" from JSON body (needed when called from batch mode).
	if (AssetPath.IsEmpty() && Req.Body.Num() > 0)
	{
		TSharedPtr<FJsonObject> BodyObj;
		FString Err;
		if (ParseJsonBody(Req, BodyObj, Err) && BodyObj.IsValid())
		{
			BodyObj->TryGetStringField(TEXT("path"), AssetPath);
		}
	}

	if (AssetPath.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'path' is required (query param or JSON body)")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;
	FString Path = AssetPath;

	AsyncTask(ENamedThreads::GameThread, [Callback, Path]()
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *Path);
		if (!Asset)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Asset not found: %s"), *Path)));
			return;
		}

		TSharedPtr<FJsonObject> Root = UObjectToJson(Asset);

		// --- Data-type introspection for codegen (UENUM / USTRUCT mirrors) ----
		if (UUserDefinedEnum* UDE = Cast<UUserDefinedEnum>(Asset))
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			// UserDefinedEnum always appends a hidden _MAX sentinel as the last
			// entry — exclude it by stopping one short of NumEnums().
			for (int32 i = 0; i + 1 < UDE->NumEnums(); ++i)
			{
				TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetStringField(TEXT("name"),    UDE->GetNameStringByIndex(i));
				E->SetStringField(TEXT("display"), UDE->GetDisplayNameTextByIndex(i).ToString());
				E->SetNumberField(TEXT("value"),   static_cast<double>(UDE->GetValueByIndex(i)));
				Entries.Add(MakeShared<FJsonValueObject>(E));
			}
			Root->SetArrayField(TEXT("enum_entries"), Entries);
		}
		else if (UUserDefinedStruct* UDS = Cast<UUserDefinedStruct>(Asset))
		{
			TArray<TSharedPtr<FJsonValue>> Fields;
			for (TFieldIterator<FProperty> It(UDS); It; ++It)
			{
				FProperty* P = *It;
				TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("name"),     P->GetAuthoredName());
				F->SetStringField(TEXT("raw_name"), P->GetName());
				// GetCPPType returns the container element type via ExtendedTypeText
				// (e.g. base "TArray" + extended "<int32>") — concatenate for the full type.
				FString Extended;
				const FString CppBase = P->GetCPPType(&Extended);
				F->SetStringField(TEXT("cpp_type"), CppBase + Extended);

				// Referenced struct/enum/class path — lets codegen remap to the C++ mirror.
				if (const FStructProperty* SP = CastField<FStructProperty>(P))
				{
					if (SP->Struct) F->SetStringField(TEXT("type_object"), SP->Struct->GetPathName());
				}
				else if (const FEnumProperty* EP = CastField<FEnumProperty>(P))
				{
					if (EP->GetEnum()) F->SetStringField(TEXT("type_object"), EP->GetEnum()->GetPathName());
				}
				else if (const FByteProperty* BP2 = CastField<FByteProperty>(P))
				{
					if (BP2->Enum) F->SetStringField(TEXT("type_object"), BP2->Enum->GetPathName());
				}
				else if (const FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(P))
				{
					if (OP->PropertyClass) F->SetStringField(TEXT("type_object"), OP->PropertyClass->GetPathName());
				}
				Fields.Add(MakeShared<FJsonValueObject>(F));
			}
			Root->SetArrayField(TEXT("struct_fields"), Fields);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /assets/duplicate
// Body: { "source": "/Game/.../BP_Foo", "dest": "/Game/.../BP_Foo_CPP" }
// Duplicates an asset (original untouched) and saves the copy. Used by the
// duplicate-pattern BP→C++ conversion to make the *_CPP working copy.
// ============================================================================
bool FNGGHttpServer::HandleAssetsDuplicate(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError) || !Body.IsValid())
	{
		OnComplete(JsonError(400, ParseError));
		return true;
	}

	FString Source, Dest;
	Body->TryGetStringField(TEXT("source"), Source);
	Body->TryGetStringField(TEXT("dest"),   Dest);
	if (Source.IsEmpty() || Dest.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'source' and 'dest' are required")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;
	AsyncTask(ENamedThreads::GameThread, [Callback, Source, Dest]()
	{
		UObject* SrcAsset = LoadObject<UObject>(nullptr, *Source);
		if (!SrcAsset)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Source asset not found: %s"), *Source)));
			return;
		}

		const FString DestName = FPackageName::GetLongPackageAssetName(Dest);
		const FString DestPath = FPackageName::GetLongPackagePath(Dest);
		if (DestName.IsEmpty() || DestPath.IsEmpty())
		{
			Callback(JsonError(400, FString::Printf(TEXT("Invalid dest path: %s"), *Dest)));
			return;
		}

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		UObject* NewAsset = AssetToolsModule.Get().DuplicateAsset(DestName, DestPath, SrcAsset);
		if (!NewAsset)
		{
			Callback(JsonError(500, FString::Printf(TEXT("DuplicateAsset failed: %s -> %s"), *Source, *Dest)));
			return;
		}

		UEditorLoadingAndSavingUtils::SavePackages({ NewAsset->GetOutermost() }, /*bOnlyDirty=*/false);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"), true);
		Out->SetStringField(TEXT("source"),  Source);
		Out->SetStringField(TEXT("dest"),    NewAsset->GetPathName());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Out, Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /assets/create
// Body: { "class": "AdlExerciseDefinition", "path": "/Game/Data/Exercises/DA_ADL01" }
// ============================================================================

bool FNGGHttpServer::HandleAssetsCreate(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseError;
	TSharedPtr<FJsonObject> Body;

	if (!ParseJsonBody(Req, Body, ParseError))
	{
		OnComplete(JsonError(400, ParseError));
		return true;
	}

	FString ClassName, AssetPath;
	Body->TryGetStringField(TEXT("class"), ClassName);
	Body->TryGetStringField(TEXT("path"),  AssetPath);

	if (ClassName.IsEmpty() || AssetPath.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("Both 'class' and 'path' fields are required")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback, ClassName, AssetPath]()
	{
		// Split path into package dir + asset name
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		if (AssetName.IsEmpty())
		{
			Callback(JsonError(400, TEXT("Could not derive asset name from path")));
			return;
		}

		// Find the UClass by name (searches all loaded classes)
		// UClass::GetName() returns names WITHOUT U/A prefix, so strip it.
		FString StrippedClassName = ClassName;
		if (StrippedClassName.StartsWith(TEXT("U")) || StrippedClassName.StartsWith(TEXT("A")))
			StrippedClassName = StrippedClassName.Mid(1);

		UClass* TargetClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == StrippedClassName || It->GetName() == ClassName)
			{
				TargetClass = *It;
				break;
			}
		}

		if (!TargetClass)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Class '%s' not found in the current editor session"), *ClassName)));
			return;
		}

		// Check it's a UDataAsset subclass
		if (!TargetClass->IsChildOf(UDataAsset::StaticClass()))
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Class '%s' is not a UDataAsset subclass"), *ClassName)));
			return;
		}

		// Refuse abstract / deprecated / hidden classes — UDataAssetFactory will
		// otherwise call NewObject on an abstract class and trip the
		// StaticAllocateObjectErrorTests check, crashing the editor.
		if (TargetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Class '%s' is abstract/deprecated and cannot be instantiated directly"), *ClassName)));
			return;
		}

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
			TEXT("AssetTools")).Get();

		// Use the DataAsset factory.
		// GetTransientPackage() is the correct outer for editor-only transient UObjects.
		UDataAssetFactory* Factory = NewObject<UDataAssetFactory>(GetTransientPackage());
		Factory->DataAssetClass = TargetClass;

		UObject* NewAsset = AT.CreateAsset(AssetName, PackagePath, TargetClass, Factory);
		if (!NewAsset)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateAsset failed for path '%s/%s'"), *PackagePath, *AssetName)));
			return;
		}

		// Mark dirty and save immediately
		NewAsset->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("created"),  NewAsset->GetPathName());
		Result->SetStringField(TEXT("class"),    NewAsset->GetClass()->GetName());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		Callback(JsonCreated(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /assets/set_property
// Body: { "path": "...", "property": "ExerciseID", "value": "ADL_01" }
// ============================================================================

bool FNGGHttpServer::HandleAssetsSetProp(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseError;
	TSharedPtr<FJsonObject> Body;

	if (!ParseJsonBody(Req, Body, ParseError))
	{
		OnComplete(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, PropertyName;
	Body->TryGetStringField(TEXT("path"),     AssetPath);
	Body->TryGetStringField(TEXT("property"), PropertyName);

	const TSharedPtr<FJsonValue>* ValueField = Body->Values.Find(TEXT("value"));
	if (AssetPath.IsEmpty() || PropertyName.IsEmpty() || !ValueField)
	{
		OnComplete(JsonError(400, TEXT("'path', 'property', and 'value' are all required")));
		return true;
	}

	TSharedPtr<FJsonValue> JsonVal = *ValueField;
	FHttpResultCallback Callback   = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, PropertyName, JsonVal]()
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Asset)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Asset not found: %s"), *AssetPath)));
			return;
		}

		const FString SetError = SetPropertyFromJson(Asset, PropertyName, JsonVal);
		if (!SetError.IsEmpty())
		{
			Callback(JsonError(422, SetError));
			return;
		}

		Asset->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"),     AssetPath);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetBoolField  (TEXT("success"),  true);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /assets/set_map_entries
// Body: {
//   "path": "/Game/MyGame/Data/DA_BombRegistry",
//   "property": "BombClasses",
//   "entries": [
//     { "key": "Standard", "value": "/Game/MyGame/Blueprints/BP_MyGameStandardBomb.BP_MyGameStandardBomb_C" },
//     ...
//   ]
// }
// Directly manipulates a TMap<Enum, TSubclassOf<>> using LoadObject to resolve
// class references.  This avoids ImportText issues with FObjectPtr handles.
// ============================================================================

bool FNGGHttpServer::HandleAssetsSetMapEntries(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString ParseError;
	TSharedPtr<FJsonObject> Body;

	if (!ParseJsonBody(Req, Body, ParseError))
	{
		OnComplete(JsonError(400, ParseError));
		return true;
	}

	FString AssetPath, PropertyName;
	Body->TryGetStringField(TEXT("path"),     AssetPath);
	Body->TryGetStringField(TEXT("property"), PropertyName);

	const TArray<TSharedPtr<FJsonValue>>* EntriesPtr = nullptr;
	if (!Body->TryGetArrayField(TEXT("entries"), EntriesPtr) || !EntriesPtr)
	{
		OnComplete(JsonError(400, TEXT("'path', 'property', and 'entries' array are all required")));
		return true;
	}

	// Copy entries to capture for the async lambda
	TArray<TSharedPtr<FJsonValue>> Entries = *EntriesPtr;
	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, PropertyName, Entries]()
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Asset)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Asset not found: %s"), *AssetPath)));
			return;
		}

		FProperty* Prop = Asset->GetClass()->FindPropertyByName(*PropertyName);
		if (!Prop)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Property '%s' not found"), *PropertyName)));
			return;
		}

		const FMapProperty* MapProp = CastField<FMapProperty>(Prop);
		if (!MapProp)
		{
			Callback(JsonError(400, FString::Printf(TEXT("Property '%s' is not a TMap"), *PropertyName)));
			return;
		}

		void* MapPtr = MapProp->ContainerPtrToValuePtr<void>(Asset);
		FScriptMapHelper MapHelper(MapProp, MapPtr);

		// Clear existing entries
		MapHelper.EmptyValues();

		int32 SetCount = 0;
		TArray<FString> Warnings;

		for (const TSharedPtr<FJsonValue>& EntryVal : Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!EntryVal.IsValid() || !EntryVal->TryGetObject(EntryObj) || !EntryObj)
			{
				Warnings.Add(TEXT("Skipped non-object entry"));
				continue;
			}

			FString KeyStr, ValueStr;
			(*EntryObj)->TryGetStringField(TEXT("key"), KeyStr);
			(*EntryObj)->TryGetStringField(TEXT("value"), ValueStr);

			if (KeyStr.IsEmpty())
			{
				Warnings.Add(TEXT("Skipped entry with empty key"));
				continue;
			}

			// Add a new map entry
			const int32 NewIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();

			// Import the key (enum name)
			uint8* KeyPtr = MapHelper.GetKeyPtr(NewIndex);
			MapProp->KeyProp->ImportText_Direct(*KeyStr, KeyPtr, Asset, PPF_None);

			// Import the value (class path) — use LoadObject to resolve
			uint8* ValPtr = MapHelper.GetValuePtr(NewIndex);

			if (ValueStr.IsEmpty() || ValueStr == TEXT("None"))
			{
				// Leave as null
			}
			else
			{
				// Try to load the class
				UClass* LoadedClass = LoadObject<UClass>(nullptr, *ValueStr);
				if (!LoadedClass)
				{
					// Try with _C suffix for Blueprint generated classes
					FString WithSuffix = ValueStr;
					if (!WithSuffix.EndsWith(TEXT("_C")))
					{
						WithSuffix += TEXT("_C");
					}
					LoadedClass = LoadObject<UClass>(nullptr, *WithSuffix);
				}

				if (LoadedClass)
				{
					// Use ImportText with the class path to properly initialize
					// the FObjectPtr handle in the map value slot.
					FString ClassPath = LoadedClass->GetPathName();
					const TCHAR* ImportResult = MapProp->ValueProp->ImportText_Direct(
						*ClassPath, ValPtr, Asset, PPF_None);
					if (ImportResult)
					{
						SetCount++;
					}
					else
					{
						Warnings.Add(FString::Printf(TEXT("ImportText failed for value '%s' (key '%s')"), *ClassPath, *KeyStr));
					}
				}
				else
				{
					Warnings.Add(FString::Printf(TEXT("Failed to load class '%s' for key '%s'"), *ValueStr, *KeyStr));
				}
			}
		}

		MapHelper.Rehash();
		Asset->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), AssetPath);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetNumberField(TEXT("entries_set"), SetCount);
		Result->SetBoolField(TEXT("success"), true);

		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarnArray;
			for (const FString& W : Warnings)
			{
				WarnArray.Add(MakeShared<FJsonValueString>(W));
			}
			Result->SetArrayField(TEXT("warnings"), WarnArray);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/save_all
// ============================================================================

bool FNGGHttpServer::HandleEditorSaveAll(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback]()
	{
		// Save all dirty packages including map/level files
		const bool bSaved = UEditorLoadingAndSavingUtils::SaveDirtyPackages(
			/*bSaveMapPackages=*/true,
			/*bSaveContentPackages=*/true);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), bSaved);
		Result->SetStringField(TEXT("note"),
			bSaved ? TEXT("All dirty content packages saved") : TEXT("Save returned false — check Output Log"));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/reimport?path=/Game/...
// ============================================================================

bool FNGGHttpServer::HandleEditorReimport(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FString AssetPath = GetQueryParam(Req, TEXT("path"));
	if (AssetPath.IsEmpty())
	{
		// Also accept from body
		FString ParseError;
		TSharedPtr<FJsonObject> Body;
		if (ParseJsonBody(Req, Body, ParseError))
		{
			Body->TryGetStringField(TEXT("path"), AssetPath);
		}
	}

	if (AssetPath.IsEmpty())
	{
		OnComplete(JsonError(400, TEXT("'path' query param or body field is required")));
		return true;
	}

	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath]()
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!Asset)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Asset not found: %s"), *AssetPath)));
			return;
		}

		TArray<UObject*> Assets = { Asset };
		const bool bResult = FReimportManager::Instance()->ReimportMultiple(Assets);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),    bResult);
		Result->SetStringField(TEXT("asset_path"), AssetPath);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: GET /gameplay_tags/list
// ============================================================================

bool FNGGHttpServer::HandleGameplayTagsList(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	AsyncTask(ENamedThreads::GameThread, [Callback]()
	{
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

		TArray<TSharedPtr<FJsonValue>> TagArray;

		// RequestAllGameplayTags returns every registered leaf and parent tag.
		// FindTagNode(EmptyTag) always returns nullptr in UE5 and cannot be used.
		FGameplayTagContainer AllTags;
		TagManager.RequestAllGameplayTags(AllTags, /*bOnlyIncludeDictionaryTags=*/false);
		for (const FGameplayTag& Tag : AllTags)
		{
			if (!Tag.IsValid()) continue;
			TSharedPtr<FJsonObject> TagObj = MakeShared<FJsonObject>();
			TagObj->SetStringField(TEXT("tag"),        Tag.ToString());
			TagObj->SetStringField(TEXT("short_name"), Tag.GetTagName().ToString());
			TagArray.Add(MakeShared<FJsonValueObject>(TagObj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), static_cast<double>(TagArray.Num()));
		Result->SetArrayField (TEXT("tags"),  TagArray);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Includes for Blueprint / Widget Blueprint / World Settings / Actor Spawning
// These are placed after the existing implementation to avoid reordering the
// translation unit and triggering unnecessary rebuild of earlier handlers.
// ============================================================================

// ---- Blueprint creation -----------------------------------------------------
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Factories/BlueprintFactory.h"
// ---- Blueprint reparenting --------------------------------------------------
#include "BlueprintEditorLibrary.h"
// ---- Widget Blueprint creation ----------------------------------------------
#include "WidgetBlueprintFactory.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ProgressBar.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/Slider.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/SafeZone.h"
#include "Components/SafeZoneSlot.h"
#include "Components/Spacer.h"
#include "Components/WidgetSwitcher.h"
#include "Components/NamedSlot.h"
#include "Components/CheckBox.h"
#include "Components/RichTextBlock.h"
#include "Components/CircularThrobber.h"
#include "Components/Throbber.h"
#include "Components/BackgroundBlur.h"
#include "Components/RetainerBox.h"
#include "Components/InvalidationBox.h"
#include "Components/MenuAnchor.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/EditableText.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"
#include "AssetImportTask.h"
// ---- World settings / level editor ------------------------------------------
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"
#include "Engine/LevelStreaming.h"
#include "LevelEditor.h"
#include "Editor.h"

// ============================================================================
// Shared widget helpers (used by HandleCreateWidgetBlueprint, HandleStyleWidgets,
// HandleGetWidgetTree, HandleRemoveWidgetFromBlueprint, HandleReparentWidget,
// HandleRenameWidget, HandleCompileWidgetBlueprint).
// Call qualified as NGGWidgetPriv:: from handlers — adaptive non-unity builds
// fail with C2668 on unqualified static helpers shared across handler blocks.
// ============================================================================
namespace NGGWidgetPriv
{
	static UClass* ResolveWidgetClass(const FString& TypeName, const FString& UserWidgetClassPath)
	{
		if (TypeName == TEXT("UserWidget") || TypeName == TEXT("UserWidgetInstance"))
		{
			if (UserWidgetClassPath.IsEmpty()) return nullptr;
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *UserWidgetClassPath))
			{
				return BP->GeneratedClass ? BP->GeneratedClass : nullptr;
			}
			return nullptr;
		}
		if (TypeName == TEXT("Button"))                 return UButton::StaticClass();
		if (TypeName == TEXT("TextBlock"))              return UTextBlock::StaticClass();
		if (TypeName == TEXT("ComboBoxString"))         return UComboBoxString::StaticClass();
		if (TypeName == TEXT("EditableTextBox"))        return UEditableTextBox::StaticClass();
		if (TypeName == TEXT("EditableText"))           return UEditableText::StaticClass();
		if (TypeName == TEXT("ProgressBar"))            return UProgressBar::StaticClass();
		if (TypeName == TEXT("Image"))                  return UImage::StaticClass();
		if (TypeName == TEXT("Slider"))                 return USlider::StaticClass();
		if (TypeName == TEXT("CheckBox"))               return UCheckBox::StaticClass();
		if (TypeName == TEXT("VerticalBox"))            return UVerticalBox::StaticClass();
		if (TypeName == TEXT("HorizontalBox"))          return UHorizontalBox::StaticClass();
		if (TypeName == TEXT("CanvasPanel"))            return UCanvasPanel::StaticClass();
		if (TypeName == TEXT("Overlay"))                return UOverlay::StaticClass();
		if (TypeName == TEXT("Border"))                 return UBorder::StaticClass();
		if (TypeName == TEXT("ScrollBox"))              return UScrollBox::StaticClass();
		if (TypeName == TEXT("SizeBox"))                return USizeBox::StaticClass();
		if (TypeName == TEXT("GridPanel"))              return UGridPanel::StaticClass();
		if (TypeName == TEXT("UniformGridPanel"))       return UUniformGridPanel::StaticClass();
		if (TypeName == TEXT("WrapBox"))                return UWrapBox::StaticClass();
		if (TypeName == TEXT("Spacer"))                 return USpacer::StaticClass();
		if (TypeName == TEXT("ScaleBox"))               return UScaleBox::StaticClass();
		if (TypeName == TEXT("WidgetSwitcher"))         return UWidgetSwitcher::StaticClass();
		if (TypeName == TEXT("NamedSlot"))              return UNamedSlot::StaticClass();
		if (TypeName == TEXT("RichTextBlock"))          return URichTextBlock::StaticClass();
		if (TypeName == TEXT("SafeZone"))               return USafeZone::StaticClass();
		if (TypeName == TEXT("Throbber"))               return UThrobber::StaticClass();
		if (TypeName == TEXT("CircularThrobber"))       return UCircularThrobber::StaticClass();
		if (TypeName == TEXT("BackgroundBlur"))         return UBackgroundBlur::StaticClass();
		if (TypeName == TEXT("RetainerBox"))            return URetainerBox::StaticClass();
		if (TypeName == TEXT("InvalidationBox"))        return UInvalidationBox::StaticClass();
		if (TypeName == TEXT("MenuAnchor"))             return UMenuAnchor::StaticClass();
		if (TypeName == TEXT("MultiLineEditableText"))  return UMultiLineEditableText::StaticClass();
		if (TypeName == TEXT("MultiLineEditableTextBox"))return UMultiLineEditableTextBox::StaticClass();

		if (UserWidgetClassPath.IsEmpty()) return UTextBlock::StaticClass();
		return nullptr;
	}

	static EHorizontalAlignment ParseHAlign(const FString& V)
	{
		if (V == TEXT("Left"))   return HAlign_Left;
		if (V == TEXT("Right"))  return HAlign_Right;
		if (V == TEXT("Center")) return HAlign_Center;
		if (V == TEXT("Fill"))   return HAlign_Fill;
		return HAlign_Fill;
	}
	static FString HAlignToString(EHorizontalAlignment V)
	{
		switch (V)
		{
		case HAlign_Left:   return TEXT("Left");
		case HAlign_Center: return TEXT("Center");
		case HAlign_Right:  return TEXT("Right");
		case HAlign_Fill:   return TEXT("Fill");
		default:            return TEXT("Fill");
		}
	}

	static EVerticalAlignment ParseVAlign(const FString& V)
	{
		if (V == TEXT("Top"))    return VAlign_Top;
		if (V == TEXT("Bottom")) return VAlign_Bottom;
		if (V == TEXT("Center")) return VAlign_Center;
		if (V == TEXT("Fill"))   return VAlign_Fill;
		return VAlign_Fill;
	}
	static FString VAlignToString(EVerticalAlignment V)
	{
		switch (V)
		{
		case VAlign_Top:    return TEXT("Top");
		case VAlign_Center: return TEXT("Center");
		case VAlign_Bottom: return TEXT("Bottom");
		case VAlign_Fill:   return TEXT("Fill");
		default:            return TEXT("Fill");
		}
	}

	static ESlateVisibility ParseVisibility(const FString& V)
	{
		if (V == TEXT("Collapsed"))            return ESlateVisibility::Collapsed;
		if (V == TEXT("Hidden"))               return ESlateVisibility::Hidden;
		if (V == TEXT("HitTestInvisible"))     return ESlateVisibility::HitTestInvisible;
		if (V == TEXT("SelfHitTestInvisible")) return ESlateVisibility::SelfHitTestInvisible;
		return ESlateVisibility::Visible;
	}

	static FLinearColor ParseHexColor(const FString& Hex)
	{
		FString Clean = Hex.StartsWith(TEXT("#")) ? Hex.Mid(1) : Hex;
		return FLinearColor(FColor::FromHex(Clean));
	}
	static FString ColorToHex(const FLinearColor& C)
	{
		const FColor SRGB = C.ToFColor(true);
		return FString::Printf(TEXT("#%02X%02X%02X%02X"), SRGB.R, SRGB.G, SRGB.B, SRGB.A);
	}

	static EOrientation ParseOrientation(const FString& V)
	{
		if (V == TEXT("Vertical")) return Orient_Vertical;
		return Orient_Horizontal;
	}

	static ESlateBrushDrawType::Type ParseBrushDrawAs(const FString& V)
	{
		if (V == TEXT("Box"))         return ESlateBrushDrawType::Box;
		if (V == TEXT("Border"))      return ESlateBrushDrawType::Border;
		if (V == TEXT("RoundedBox"))  return ESlateBrushDrawType::RoundedBox;
		if (V == TEXT("NoDrawType"))  return ESlateBrushDrawType::NoDrawType;
		return ESlateBrushDrawType::Image;
	}
	static FString BrushDrawAsToString(ESlateBrushDrawType::Type V)
	{
		switch (V)
		{
		case ESlateBrushDrawType::Box:        return TEXT("Box");
		case ESlateBrushDrawType::Border:     return TEXT("Border");
		case ESlateBrushDrawType::RoundedBox: return TEXT("RoundedBox");
		case ESlateBrushDrawType::NoDrawType: return TEXT("NoDrawType");
		default:                              return TEXT("Image");
		}
	}

	static EProgressBarFillType::Type ParseBarFillType(const FString& V)
	{
		if (V == TEXT("RightToLeft"))            return EProgressBarFillType::RightToLeft;
		if (V == TEXT("FillFromCenter"))         return EProgressBarFillType::FillFromCenter;
		if (V == TEXT("FillFromCenterHorizontal")) return EProgressBarFillType::FillFromCenterHorizontal;
		if (V == TEXT("FillFromCenterVertical")) return EProgressBarFillType::FillFromCenterVertical;
		if (V == TEXT("TopToBottom"))            return EProgressBarFillType::TopToBottom;
		if (V == TEXT("BottomToTop"))            return EProgressBarFillType::BottomToTop;
		return EProgressBarFillType::LeftToRight;
	}

	static EButtonClickMethod::Type ParseClickMethod(const FString& V)
	{
		if (V == TEXT("MouseDown"))    return EButtonClickMethod::MouseDown;
		if (V == TEXT("MouseUp"))      return EButtonClickMethod::MouseUp;
		if (V == TEXT("PreciseClick")) return EButtonClickMethod::PreciseClick;
		return EButtonClickMethod::DownAndUp;
	}

	static EWidgetClipping ParseClipping(const FString& V)
	{
		if (V == TEXT("ClipToBounds"))                  return EWidgetClipping::ClipToBounds;
		if (V == TEXT("ClipToBoundsAlways"))            return EWidgetClipping::ClipToBoundsAlways;
		if (V == TEXT("OnDemand"))                      return EWidgetClipping::OnDemand;
		if (V == TEXT("ClipToBoundsWithoutIntersecting")) return EWidgetClipping::ClipToBoundsWithoutIntersecting;
		return EWidgetClipping::Inherit;
	}

	static void EnsureGuid(UWidgetBlueprint* WBP, UWidget* W)
	{
		if (!WBP || !W) return;
		if (!WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
		{
			WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
		}
	}

	static UTexture2D* LoadTextureAsset(const FString& Path)
	{
		if (Path.IsEmpty()) return nullptr;
		return LoadObject<UTexture2D>(nullptr, *Path);
	}

	static void MarkWBPDirtyAndModified(UWidgetBlueprint* WBP)
	{
		if (!WBP) return;
		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		WBP->MarkPackageDirty();
	}

	static FMargin ReadMargin(const TSharedPtr<FJsonObject>& Obj)
	{
		double L = 0, T = 0, R = 0, B = 0;
		Obj->TryGetNumberField(TEXT("left"),   L);
		Obj->TryGetNumberField(TEXT("top"),    T);
		Obj->TryGetNumberField(TEXT("right"),  R);
		Obj->TryGetNumberField(TEXT("bottom"), B);
		return FMargin((float)L, (float)T, (float)R, (float)B);
	}

	static TSharedPtr<FJsonObject> MarginToJson(const FMargin& M)
	{
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("left"),   M.Left);
		J->SetNumberField(TEXT("top"),    M.Top);
		J->SetNumberField(TEXT("right"),  M.Right);
		J->SetNumberField(TEXT("bottom"), M.Bottom);
		return J;
	}
}

// ============================================================================
// Handler: POST /editor/create_blueprint
// ============================================================================

bool FNGGHttpServer::HandleCreateBlueprint(
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

	FString ParentClassName; Body->TryGetStringField(TEXT("parent_class"), ParentClassName);
	FString AssetPath;       Body->TryGetStringField(TEXT("asset_path"),   AssetPath);

	if (ParentClassName.IsEmpty() || AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("parent_class and asset_path are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, ParentClassName, AssetPath]()
	{
		// ---------- 1. Resolve parent UClass -----------------------------------
		UClass* ParentClass = nullptr;

		// UClass::GetName() returns the name WITHOUT the U/A prefix.
		// Build candidate list: exact input, stripped of leading U, stripped of leading A.
		FString StrippedU = ParentClassName;
		if (StrippedU.StartsWith(TEXT("U"))) StrippedU = StrippedU.Mid(1);
		FString StrippedA = ParentClassName;
		if (StrippedA.StartsWith(TEXT("A"))) StrippedA = StrippedA.Mid(1);

		TArray<FString> CandidateNames = { StrippedU, StrippedA, ParentClassName };
		for (const FString& Candidate : CandidateNames)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == Candidate)
				{
					ParentClass = *It;
					break;
				}
			}
			if (ParentClass) break;
		}

		if (!ParentClass)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Could not find UClass for parent_class '%s'"), *ParentClassName)));
			return;
		}

		// ---------- 2. Split asset_path into PackagePath + AssetName ----------
		int32 LastSlash;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}
		const FString PackagePath = AssetPath.Left(LastSlash);
		const FString AssetName   = AssetPath.Mid(LastSlash + 1);

		// ---------- 3. Check if asset already exists --------------------------
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		FAssetData Existing = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath + TEXT(".") + AssetName));
		if (Existing.IsValid())
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

		// ---------- 4. Create the Blueprint via AssetTools --------------------
		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UBlueprintFactory* BPFactory = NewObject<UBlueprintFactory>(GetTransientPackage());
		BPFactory->ParentClass         = ParentClass;
		// Note: bCreateNewBlueprint was removed in UE5.7 — creating a new Blueprint is
		// the factory's default behaviour when ParentClass is set.

		UObject* NewBP = AT.CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), BPFactory);
		if (!NewBP)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("AssetTools failed to create Blueprint at '%s'"), *AssetPath)));
			return;
		}

		NewBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),        true);
		Result->SetBoolField  (TEXT("already_existed"),false);
		Result->SetBoolField  (TEXT("created"),        true);
		Result->SetStringField(TEXT("asset_path"),     AssetPath);
		Result->SetStringField(TEXT("parent_class"),   ParentClass->GetName());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonCreated(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_widget_blueprint
// ============================================================================

bool FNGGHttpServer::HandleCreateWidgetBlueprint(
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

	FString ParentClassName; Body->TryGetStringField(TEXT("parent_class"), ParentClassName);
	FString AssetPath;       Body->TryGetStringField(TEXT("asset_path"),   AssetPath);

	// Capture widgets array (may be absent -> empty). Guard the type so a
	// non-array "widgets" value yields a 400 instead of a check() crash.
	TArray<TSharedPtr<FJsonValue>> WidgetsJson;
	if (Body->HasField(TEXT("widgets")))
	{
		const TArray<TSharedPtr<FJsonValue>>* WidgetsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("widgets"), WidgetsPtr) || !WidgetsPtr)
		{
			Callback(JsonError(400, TEXT("'widgets' must be an array")));
			return true;
		}
		WidgetsJson = *WidgetsPtr;
	}

	FString RootType;
	Body->TryGetStringField(TEXT("root_type"), RootType);
	if (RootType.IsEmpty()) RootType = TEXT("CanvasPanel");

	if (ParentClassName.IsEmpty() || AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("parent_class and asset_path are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, ParentClassName, AssetPath, WidgetsJson, RootType]()
	{
		// ---------- 1. Resolve parent UClass (strip U prefix for GetName()) ---
		UClass* ParentClass = nullptr;
		FString WStrippedU = ParentClassName;
		if (WStrippedU.StartsWith(TEXT("U"))) WStrippedU = WStrippedU.Mid(1);

		TArray<FString> CandidateNames = { WStrippedU, ParentClassName };
		for (const FString& Candidate : CandidateNames)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == Candidate)
				{
					ParentClass = *It;
					break;
				}
			}
			if (ParentClass) break;
		}

		if (!ParentClass)
		{
			// Fall back to UUserWidget so the file is still useful
			ParentClass = UUserWidget::StaticClass();
			UE_LOG(LogNGGBridge, Warning,
				TEXT("HandleCreateWidgetBlueprint: could not find class '%s', falling back to UUserWidget"),
				*ParentClassName);
		}

		// ---------- 2. Split asset_path ---------------------------------------
		int32 LastSlash;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash))
		{
			Callback(JsonError(400, TEXT("asset_path must contain '/'")));
			return;
		}
		const FString PackagePath = AssetPath.Left(LastSlash);
		const FString AssetName   = AssetPath.Mid(LastSlash + 1);

		// ---------- 3. Check for pre-existing asset ---------------------------
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		FAssetData Existing = AR.GetAssetByObjectPath(FSoftObjectPath(AssetPath + TEXT(".") + AssetName));
		if (Existing.IsValid())
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

		// ---------- 4. Create Widget Blueprint --------------------------------
		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>(GetTransientPackage());
		Factory->ParentClass = ParentClass;

		UObject* NewAsset = AT.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(NewAsset);

		int32 WidgetCount = 0;

		// ---------- 5. Populate widget tree if requested ----------------------
		if (IsValid(WBP) && IsValid(WBP->WidgetTree))
		{
			// Construct the requested root panel (or fall back to CanvasPanel).
			UClass* RootClass = NGGWidgetPriv::ResolveWidgetClass(RootType, FString());
			if (!RootClass || !RootClass->IsChildOf(UPanelWidget::StaticClass()))
			{
				RootClass = UCanvasPanel::StaticClass();
			}
			const FString RootName = RootType + TEXT("_Root");
			UPanelWidget* Root = Cast<UPanelWidget>(
				WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootName)));
			WBP->WidgetTree->RootWidget = Root;
			NGGWidgetPriv::EnsureGuid(WBP, Root);

			TMap<FString, UWidget*> NameToWidget;

			for (const TSharedPtr<FJsonValue>& Entry : WidgetsJson)
			{
				const TSharedPtr<FJsonObject>* EntryObj;
				if (!Entry->TryGetObject(EntryObj)) continue;

				FString TypeName, WidgetName, ParentName, UserWidgetClassPath;
				(*EntryObj)->TryGetStringField(TEXT("type"),              TypeName);
				(*EntryObj)->TryGetStringField(TEXT("name"),              WidgetName);
				(*EntryObj)->TryGetStringField(TEXT("parent"),            ParentName);
				(*EntryObj)->TryGetStringField(TEXT("user_widget_class"), UserWidgetClassPath);
				if (WidgetName.IsEmpty()) continue;

				UClass* WidgetClass = NGGWidgetPriv::ResolveWidgetClass(TypeName, UserWidgetClassPath);
				if (!WidgetClass) continue;

				UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
				if (!W) continue;
				NGGWidgetPriv::EnsureGuid(WBP, W);

				NameToWidget.Add(WidgetName, W);

				UWidget** ParentPtr = ParentName.IsEmpty() ? nullptr : NameToWidget.Find(ParentName);
				UPanelWidget* ParentPanel = ParentPtr ? Cast<UPanelWidget>(*ParentPtr) : nullptr;

				if (ParentPanel)
				{
					ParentPanel->AddChild(W);
				}
				else if (UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(Root))
				{
					UCanvasPanelSlot* Slot = RootCanvas->AddChildToCanvas(W);
					if (Slot) Slot->SetAutoSize(false);
				}
				else if (Root)
				{
					Root->AddChild(W);
				}
				++WidgetCount;
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		}

		if (IsValid(WBP))
		{
			WBP->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),        IsValid(WBP));
		Result->SetBoolField  (TEXT("already_existed"),false);
		Result->SetStringField(TEXT("asset_path"),     AssetPath);
		Result->SetNumberField(TEXT("widget_count"),   static_cast<double>(WidgetCount));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(IsValid(WBP) ? JsonCreated(BodyStr) : JsonError(500, TEXT("WidgetBlueprint creation failed")));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/set_blueprint_defaults
// ============================================================================

bool FNGGHttpServer::HandleSetBlueprintDefaults(
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

	TArray<TSharedPtr<FJsonValue>> PropsJson;
	if (Body->HasField(TEXT("properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PropsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("properties"), PropsPtr) || !PropsPtr)
		{
			Callback(JsonError(400, TEXT("'properties' must be an array")));
			return true;
		}
		PropsJson = *PropsPtr;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, PropsJson]()
	{
		// ---------- 1. Load the Blueprint asset --------------------------------
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Blueprint not found at '%s'"), *AssetPath)));
			return;
		}

		// ---------- 2. Get CDO ------------------------------------------------
		UObject* CDO = (Blueprint->GeneratedClass)
			? Blueprint->GeneratedClass->GetDefaultObject()
			: nullptr;

		if (!CDO)
		{
			Callback(JsonError(500, TEXT("Blueprint has no GeneratedClass / CDO")));
			return;
		}

		// ---------- 3. Apply each property ------------------------------------
		int32 SetCount = 0;
		TArray<FString> Warnings;

		for (const TSharedPtr<FJsonValue>& Entry : PropsJson)
		{
			const TSharedPtr<FJsonObject>* EntryObj;
			if (!Entry->TryGetObject(EntryObj)) continue;

			FString PropName, ValueStr;
			(*EntryObj)->TryGetStringField(TEXT("name"),  PropName);
			(*EntryObj)->TryGetStringField(TEXT("value"), ValueStr);
			if (PropName.IsEmpty()) continue;

			FProperty* Prop = FindFProperty<FProperty>(CDO->GetClass(), *PropName);
			if (!Prop)
			{
				Warnings.Add(FString::Printf(TEXT("Property '%s' not found on %s"),
					*PropName, *CDO->GetClass()->GetName()));
				continue;
			}

			// For TSubclassOf (FClassProperty / FSoftClassProperty) properties,
			// resolve Blueprint asset paths to their GeneratedClass.
			// ImportText expects the _C class path, not the Blueprint asset path.
			FString ResolvedValue = ValueStr;
			{
				FClassProperty* ClassProp = CastField<FClassProperty>(Prop);
				FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop);
				if ((ClassProp || SoftClassProp) && ResolvedValue.StartsWith(TEXT("/Game/")))
				{
					// Try loading as a Blueprint and extract its GeneratedClass path.
					FString TestPath = ResolvedValue;
					if (!TestPath.Contains(TEXT(".")))
					{
						// Add the object name: "/Game/Foo/Bar" → "/Game/Foo/Bar.Bar"
						FString Leaf = FPaths::GetCleanFilename(TestPath);
						TestPath = TestPath + TEXT(".") + Leaf;
					}

					UBlueprint* RefBP = LoadObject<UBlueprint>(nullptr, *TestPath);
					if (RefBP && RefBP->GeneratedClass)
					{
						ResolvedValue = RefBP->GeneratedClass->GetPathName();
					}
					else
					{
						// Also try with _C suffix directly
						if (!ResolvedValue.EndsWith(TEXT("_C")))
						{
							FString Leaf = FPaths::GetCleanFilename(ResolvedValue);
							FString WithC = ResolvedValue + TEXT(".") + Leaf + TEXT("_C");
							UClass* DirectClass = LoadObject<UClass>(nullptr, *WithC);
							if (DirectClass)
							{
								ResolvedValue = DirectClass->GetPathName();
							}
						}
					}
				}
			}

			// ImportText_Direct interprets the string the same way the UE5
			// property system does (handles asset references, enums, vectors, etc.)
			const TCHAR* ImportResult = Prop->ImportText_Direct(
				*ResolvedValue,
				Prop->ContainerPtrToValuePtr<void>(CDO),
				CDO,
				PPF_None);

			if (ImportResult)
			{
				++SetCount;
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("ImportText failed for property '%s' with value '%s'"),
					*PropName, *ValueStr));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		Blueprint->MarkPackageDirty();

		// ---------- 4. Respond ------------------------------------------------
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),        true);
		Resp->SetStringField(TEXT("asset_path"),     AssetPath);
		Resp->SetNumberField(TEXT("properties_set"), static_cast<double>(SetCount));

		TArray<TSharedPtr<FJsonValue>> WarnArray;
		for (const FString& W : Warnings)
		{
			WarnArray.Add(MakeShared<FJsonValueString>(W));
		}
		Resp->SetArrayField(TEXT("warnings"), WarnArray);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/reparent_blueprint
// Body: { "asset_path": "/Game/...", "new_parent": "/Game/... or UClassName" }
// Reparents a Blueprint to a new parent class (C++ class or another Blueprint).
// ============================================================================

bool FNGGHttpServer::HandleReparentBlueprint(
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
	FString NewParent; Body->TryGetStringField(TEXT("new_parent"), NewParent);

	if (AssetPath.IsEmpty() || NewParent.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path and new_parent are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, NewParent]()
	{
		// --- 1. Load the Blueprint to reparent --------------------------------
		int32 LastSlash = INDEX_NONE;
		AssetPath.FindLastChar(TEXT('/'), LastSlash);
		const FString AssetName = (LastSlash != INDEX_NONE) ? AssetPath.Mid(LastSlash + 1) : AssetPath;

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr,
			*(AssetPath + TEXT(".") + AssetName));
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Blueprint not found: '%s'"), *AssetPath)));
			return;
		}

		// --- 2. Resolve the new parent class ----------------------------------
		UClass* NewParentClass = nullptr;

		// First try: content path to Blueprint
		if (NewParent.StartsWith(TEXT("/")))
		{
			int32 ParentLastSlash = INDEX_NONE;
			NewParent.FindLastChar(TEXT('/'), ParentLastSlash);
			const FString ParentAssetName = (ParentLastSlash != INDEX_NONE)
				? NewParent.Mid(ParentLastSlash + 1) : NewParent;

			UBlueprint* ParentBP = LoadObject<UBlueprint>(nullptr,
				*(NewParent + TEXT(".") + ParentAssetName));
			if (ParentBP && ParentBP->GeneratedClass)
			{
				NewParentClass = ParentBP->GeneratedClass;
			}
		}

		// Fallback: C++ class name search (strip U/A prefix)
		if (!NewParentClass)
		{
			FString StrippedU = NewParent;
			if (StrippedU.StartsWith(TEXT("U"))) StrippedU = StrippedU.Mid(1);
			FString StrippedA = NewParent;
			if (StrippedA.StartsWith(TEXT("A"))) StrippedA = StrippedA.Mid(1);

			TArray<FString> Candidates = { StrippedU, StrippedA, NewParent };
			for (const FString& Candidate : Candidates)
			{
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->GetName() == Candidate)
					{
						NewParentClass = *It;
						break;
					}
				}
				if (NewParentClass) break;
			}
		}

		if (!NewParentClass)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Could not resolve new_parent class from '%s'"), *NewParent)));
			return;
		}

		// --- 3. Reparent -------------------------------------------------------
		const FString OldParentName = BP->ParentClass
			? BP->ParentClass->GetName() : TEXT("(none)");

		// Clear stale ICH on both the UBlueprint and its BPGC before reparenting.
		// UBlueprintEditorLibrary::ReparentBlueprint does not flush these, which
		// leaves BPGC->InheritableComponentHandler non-null while
		// Blueprint->InheritableComponentHandler is null — triggering the
		// ensure(!BPGC->InheritableComponentHandler) in GetInheritableComponentHandler
		// when the Blueprint editor next opens the asset.
		BP->InheritableComponentHandler = nullptr;
		if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass))
		{
			BPGC->InheritableComponentHandler = nullptr;
		}

		UBlueprintEditorLibrary::ReparentBlueprint(BP, NewParentClass);

		if (BP->ParentClass && BP->ParentClass != NewParentClass)
		{
			Callback(JsonError(500, TEXT("ReparentBlueprint completed but class did not update")));
			return;
		}

		// Recompile after ICH flush
		if (BP->ParentClass)
		{
			FKismetEditorUtilities::CompileBlueprint(BP,
				EBlueprintCompileOptions::SkipGarbageCollection);
		}

		BP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),     true);
		Resp->SetStringField(TEXT("asset_path"),  AssetPath);
		Resp->SetStringField(TEXT("old_parent"),  OldParentName);
		Resp->SetStringField(TEXT("new_parent"),  NewParentClass->GetName());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

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
// Handler: POST /editor/set_component_defaults
// ============================================================================

bool FNGGHttpServer::HandleSetComponentDefaults(
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

	FString AssetPath;     Body->TryGetStringField(TEXT("asset_path"),     AssetPath);
	FString ComponentName; Body->TryGetStringField(TEXT("component_name"), ComponentName);

	TArray<TSharedPtr<FJsonValue>> PropsJson;
	if (Body->HasField(TEXT("properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PropsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("properties"), PropsPtr) || !PropsPtr)
		{
			Callback(JsonError(400, TEXT("'properties' must be an array")));
			return true;
		}
		PropsJson = *PropsPtr;
	}

	if (AssetPath.IsEmpty() || ComponentName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path and component_name are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, ComponentName, PropsJson]()
	{
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint || !Blueprint->GeneratedClass)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Blueprint not found or has no GeneratedClass at '%s'"), *AssetPath)));
			return;
		}

		// ------------------------------------------------------------------
		// Determine the correct template object to modify.
		//
		// Modifying the GeneratedClass CDO component directly and then calling
		// CompileBlueprint causes an access violation in FinishCompilingClass
		// because the compiler invalidates and recreates CDO objects during
		// compilation, leaving dangling pointers.
		//
		// Strategy (lookup order — first hit wins):
		//   1. SCS-owned component (added via Blueprint or
		//      add_component_to_blueprint). These do NOT appear as default
		//      subobjects on a freshly-loaded CDO — the BP must have been
		//      compiled to instantiate them on the CDO. SCS lookup is
		//      independent of CDO state, so it's the primary path.
		//   2. Inherited C++ component via UInheritableComponentHandler — finds
		//      the SCS node in a parent BPGC and creates/uses a per-Blueprint
		//      override template. ICH overrides don't require a recompile.
		//   3. Fallback: direct CDO lookup (for purely native components that
		//      have no SCS node anywhere in the hierarchy).
		// ------------------------------------------------------------------

		UObject* TemplateToModify  = nullptr;
		UObject* ComponentOnCDO    = nullptr; // resolved lazily for paths 2 & 3
		bool     bRequiresCompile  = false;

		// --- Path 1: SCS-owned component (primary) ---
		if (Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentTemplate
					&& Node->GetVariableName().ToString() == ComponentName)
				{
					TemplateToModify  = Node->ComponentTemplate;
					bRequiresCompile  = true;
					break;
				}
			}
		}

		// CDO is only required for paths 2 and 3 (inherited / native components).
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		if (!TemplateToModify && !CDO)
		{
			Callback(JsonError(500, TEXT("Blueprint has no CDO")));
			return;
		}

		if (!TemplateToModify && CDO)
		{
			// Find the component on the CDO (used for class/name resolution only)
			ComponentOnCDO = CDO->GetDefaultSubobjectByName(FName(*ComponentName));
			if (!ComponentOnCDO)
			{
				TArray<UObject*> SubObjects;
				CDO->GetDefaultSubobjects(SubObjects);
				for (UObject* Sub : SubObjects)
				{
					if (Sub && Sub->GetName() == ComponentName)
					{
						ComponentOnCDO = Sub;
						break;
					}
				}
			}
		}

		// --- Path 2: Inherited C++ component — use InheritableComponentHandler ---
		if (!TemplateToModify)
		{
			// Walk the parent Blueprint chain to find an SCS node (in a parent BPGC)
			// that corresponds to this component name. Native C++ components have no
			// USCS_Node and cannot be represented by FComponentKey directly — the key
			// must come from an SCS node in a Blueprint that introduced or wraps the
			// component. Once we have a valid FComponentKey we can use
			// UInheritableComponentHandler to find or create a per-Blueprint override
			// template without triggering a full Blueprint recompile.
			UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
			UInheritableComponentHandler* ICH = BPGC ? BPGC->GetInheritableComponentHandler(true) : nullptr;

			// Sync the ICH pointer back to the UBlueprint object.
			// BPGC->GetInheritableComponentHandler(true) creates the ICH on the generated
			// class but UBlueprint::GetInheritableComponentHandler caches it separately.
			// If Blueprint->InheritableComponentHandler stays null while BPGC's is non-null,
			// opening the Blueprint editor triggers ensure(!BPGC->InheritableComponentHandler)
			// at Blueprint.cpp:2217 inside UBlueprint::GetInheritableComponentHandler(bCreate=true).
			if (ICH && Blueprint->InheritableComponentHandler == nullptr)
			{
				Blueprint->InheritableComponentHandler = ICH;
			}

			if (ICH)
			{
				// Try to find an existing key in the ICH by variable name first.
				FComponentKey ExistingKey = ICH->FindKey(FName(*ComponentName));
				if (ExistingKey.IsValid())
				{
					// An override record already exists — retrieve its template.
					UActorComponent* ExistingOverride = ICH->GetOverridenComponentTemplate(ExistingKey);
					if (!ExistingOverride)
					{
						// Record exists but template was not yet created — create it now.
						ExistingOverride = ICH->CreateOverridenComponentTemplate(ExistingKey);
					}
					if (ExistingOverride)
					{
						TemplateToModify = ExistingOverride;
						bRequiresCompile = false; // ICH overrides don't need recompile
					}
				}
				else
				{
					// No existing ICH record. Walk the parent BPGC chain and look for
					// an SCS node with a matching variable name. FComponentKey can only
					// be constructed from a USCS_Node — it has no constructor for native
					// C++ classes. If the component is purely native (no SCS node
					// anywhere in the hierarchy), the fallback CDO path below is used.
					for (UClass* SuperClass = Blueprint->ParentClass; SuperClass; SuperClass = SuperClass->GetSuperClass())
					{
						UBlueprintGeneratedClass* SuperBPGC = Cast<UBlueprintGeneratedClass>(SuperClass);
						if (!SuperBPGC || !SuperBPGC->SimpleConstructionScript)
						{
							continue;
						}
						for (USCS_Node* Node : SuperBPGC->SimpleConstructionScript->GetAllNodes())
						{
							if (Node && Node->GetVariableName().ToString() == ComponentName)
							{
								FComponentKey SCSKey(Node);
								if (SCSKey.IsValid())
								{
									UActorComponent* Override = ICH->CreateOverridenComponentTemplate(SCSKey);
									if (Override)
									{
										TemplateToModify = Override;
										bRequiresCompile = false;
									}
								}
								break;
							}
						}
						if (TemplateToModify)
						{
							break;
						}
					}
				}
			}
		}

		// --- Path 3 (Fallback): modify CDO component directly ---
		// This path is kept for edge cases (purely native components with no
		// SCS node anywhere) but skips CompileBlueprint to avoid the
		// FinishCompilingClass crash.
		if (!TemplateToModify && ComponentOnCDO)
		{
			TemplateToModify = ComponentOnCDO;
			bRequiresCompile = false;
		}

		// All three lookup paths failed — emit a single, informative 404.
		if (!TemplateToModify)
		{
			TArray<FString> AvailableSCS;
			if (Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (Node) AvailableSCS.Add(Node->GetVariableName().ToString());
				}
			}
			TArray<FString> AvailableCDO;
			if (CDO)
			{
				TArray<UObject*> SubObjects;
				CDO->GetDefaultSubobjects(SubObjects);
				for (UObject* Sub : SubObjects)
				{
					if (Sub) AvailableCDO.Add(Sub->GetName());
				}
			}
			Callback(JsonError(404, FString::Printf(
				TEXT("Component '%s' not found in Blueprint '%s'. SCS: [%s] CDO: [%s]"),
				*ComponentName,
				Blueprint->GeneratedClass ? *Blueprint->GeneratedClass->GetName() : TEXT("?"),
				*FString::Join(AvailableSCS, TEXT(", ")),
				*FString::Join(AvailableCDO, TEXT(", ")))));
			return;
		}

		// ------------------------------------------------------------------
		// Apply properties to the resolved template
		// ------------------------------------------------------------------
		int32 SetCount = 0;
		TArray<FString> Warnings;

		for (const TSharedPtr<FJsonValue>& Entry : PropsJson)
		{
			const TSharedPtr<FJsonObject>* EntryObj;
			if (!Entry->TryGetObject(EntryObj)) continue;

			FString PropName, ValueStr;
			(*EntryObj)->TryGetStringField(TEXT("name"),  PropName);
			(*EntryObj)->TryGetStringField(TEXT("value"), ValueStr);
			if (PropName.IsEmpty()) continue;

			// Support dot-notation for nested struct properties
			// e.g. "EngineSetup.TorqueCurve" navigates into the EngineSetup struct
			TArray<FString> PropPath;
			PropName.ParseIntoArray(PropPath, TEXT("."));

			FProperty*  Prop          = nullptr;
			void*       PropContainer = TemplateToModify;
			UStruct*    CurrentStruct = TemplateToModify->GetClass();

			for (int32 PathIdx = 0; PathIdx < PropPath.Num(); ++PathIdx)
			{
				Prop = FindFProperty<FProperty>(CurrentStruct, *PropPath[PathIdx]);
				if (!Prop)
				{
					break;
				}
				if (PathIdx < PropPath.Num() - 1)
				{
					// Navigate into struct — only FStructProperty supports nesting
					FStructProperty* StructProp = CastField<FStructProperty>(Prop);
					if (!StructProp)
					{
						Prop = nullptr;
						break;
					}
					PropContainer = StructProp->ContainerPtrToValuePtr<void>(PropContainer);
					CurrentStruct = StructProp->Struct;
				}
			}

			if (!Prop)
			{
				Warnings.Add(FString::Printf(TEXT("Property '%s' not found on component '%s' (%s)"),
					*PropName, *ComponentName, *TemplateToModify->GetClass()->GetName()));
				continue;
			}

			{
				FString MeshErr; bool bMeshOk = false;
				if (PropPath.Num() == 1 &&
					TrySetMeshPropertyViaSetter(TemplateToModify, FName(*PropName), ValueStr, MeshErr, bMeshOk))
				{
					if (bMeshOk) ++SetCount;
					else if (!MeshErr.IsEmpty()) Warnings.Add(MeshErr);
					continue;
				}
			}

			TemplateToModify->PreEditChange(Prop);

			const TCHAR* ImportResult = Prop->ImportText_Direct(
				*ValueStr,
				Prop->ContainerPtrToValuePtr<void>(PropContainer),
				TemplateToModify,
				PPF_None);

			FPropertyChangedEvent ChangeEvent(Prop);
			TemplateToModify->PostEditChangeProperty(ChangeEvent);

			if (ImportResult)
			{
				++SetCount;
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("ImportText failed for '%s' with value '%s'"),
					*PropName, *ValueStr));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		// Only recompile if the Blueprint has a valid parent class. If ParentClass
		// is null (e.g. the C++ module was not compiled yet), CompileBlueprint will
		// crash inside FinishCompilingClass with an access violation.
		if (bRequiresCompile && Blueprint->ParentClass != nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetStringField(TEXT("component_name"),  ComponentName);
		Resp->SetStringField(TEXT("component_class"), TemplateToModify->GetClass()->GetName());
		Resp->SetNumberField(TEXT("properties_set"),  static_cast<double>(SetCount));

		TArray<TSharedPtr<FJsonValue>> WarnArray;
		for (const FString& W : Warnings)
		{
			WarnArray.Add(MakeShared<FJsonValueString>(W));
		}
		Resp->SetArrayField(TEXT("warnings"), WarnArray);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: GET /editor/get_component_defaults
// Read counterpart of set_component_defaults: reports the configured default
// values of a Blueprint's SCS components, so a Blueprint->C++ port can lift
// them into the native constructor.
//
// Query params:
//   blueprint   (required) — content path of the Blueprint (package path or full object path).
//   component   (optional) — single component variable name; omit to dump all SCS components.
//   all_props   (optional) — "true" to emit every editable property; default emits only the
//                            properties that differ from the component class CDO (the deltas
//                            actually worth porting).
//
// Response (additive — never repurpose existing fields):
//   { success, blueprint, components: [ { name, class, properties: [
//       { name, type, value, default_value, default_object? } ] } ] }
// where `value` is this template's value and `default_value` is the component
// class CDO value (so the client sees exactly what was overridden).
// ============================================================================
bool FNGGHttpServer::HandleGetComponentDefaults(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	const FString BpPath    = GetQueryParam(Req, TEXT("blueprint"));
	const FString CompFilter = GetQueryParam(Req, TEXT("component"));
	const bool    bAllProps  = GetQueryParam(Req, TEXT("all_props")) == TEXT("true");

	if (BpPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint query param is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, CompFilter, bAllProps]()
	{
		// Accept both a package path (/Game/A/B) and a full object path (/Game/A/B.B).
		FString ObjPath = BpPath;
		if (!ObjPath.Contains(TEXT(".")))
		{
			int32 SlashIdx = INDEX_NONE;
			FString Leaf = BpPath;
			if (BpPath.FindLastChar(TEXT('/'), SlashIdx))
			{
				Leaf = BpPath.RightChop(SlashIdx + 1);
			}
			ObjPath = BpPath + TEXT(".") + Leaf;
		}

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ObjPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}

		// Serialize one component template's properties (deltas vs the component class CDO).
		auto SerializeComponent =
			[bAllProps](const FString& Name, UClass* CompClass, UObject* Template) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> CO = MakeShared<FJsonObject>();
			CO->SetStringField(TEXT("name"),  Name);
			CO->SetStringField(TEXT("class"), CompClass ? CompClass->GetPathName() : TEXT(""));

			TArray<TSharedPtr<FJsonValue>> Props;
			if (Template)
			{
				UObject* ClassDefault = Template->GetClass()->GetDefaultObject();
				for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
				{
					FProperty* Prop = *It;
					// Skip runtime-only / deprecated state — not meaningful as a configured default.
					if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						continue;
					}

					const void* ValPtr = Prop->ContainerPtrToValuePtr<void>(Template);
					const void* DefPtr = ClassDefault
						? Prop->ContainerPtrToValuePtr<void>(ClassDefault)
						: nullptr;

					const bool bDiff = (DefPtr == nullptr) || !Prop->Identical(ValPtr, DefPtr, PPF_None);
					if (!bAllProps && !bDiff)
					{
						continue;
					}

					TSharedPtr<FJsonObject> PO = MakeShared<FJsonObject>();
					PO->SetStringField(TEXT("name"), Prop->GetName());
					PO->SetStringField(TEXT("type"), Prop->GetCPPType());

					FString ValStr;
					Prop->ExportTextItem_Direct(ValStr, ValPtr, nullptr, Template, PPF_None);
					PO->SetStringField(TEXT("value"), ValStr);

					if (DefPtr)
					{
						FString DefStr;
						Prop->ExportTextItem_Direct(DefStr, DefPtr, nullptr, ClassDefault, PPF_None);
						PO->SetStringField(TEXT("default_value"), DefStr);
					}

					// Surface object-reference targets by full path for clarity.
					if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
					{
						if (UObject* Obj = ObjProp->GetObjectPropertyValue(ValPtr))
						{
							PO->SetStringField(TEXT("default_object"), Obj->GetPathName());
						}
					}

					Props.Add(MakeShared<FJsonValueObject>(PO));
				}
			}
			CO->SetArrayField(TEXT("properties"), Props);
			return CO;
		};

		TArray<TSharedPtr<FJsonValue>> Components;
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;

		auto AppendFromCDO = [&](const FString& WantedName) -> bool
		{
			UObject* CDO = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
			if (!CDO) { return false; }
			TArray<UObject*> SubObjects;
			CDO->GetDefaultSubobjects(SubObjects);
			for (UObject* Sub : SubObjects)
			{
				if (Sub && Sub->IsA<UActorComponent>()
					&& (WantedName.IsEmpty() || Sub->GetName() == WantedName))
				{
					Components.Add(MakeShared<FJsonValueObject>(
						SerializeComponent(Sub->GetName(), Sub->GetClass(), Sub)));
					if (!WantedName.IsEmpty()) { return true; }
				}
			}
			return false;
		};

		if (!CompFilter.IsEmpty())
		{
			// Single component: SCS template first, then CDO subobject fallback (native/inherited).
			USCS_Node* Found = nullptr;
			if (SCS)
			{
				for (USCS_Node* Node : SCS->GetAllNodes())
				{
					if (Node && Node->GetVariableName().ToString() == CompFilter)
					{
						Found = Node;
						break;
					}
				}
			}
			if (Found)
			{
				Components.Add(MakeShared<FJsonValueObject>(
					SerializeComponent(CompFilter, Found->ComponentClass, Found->ComponentTemplate)));
			}
			else if (!AppendFromCDO(CompFilter))
			{
				Callback(JsonError(404, FString::Printf(
					TEXT("Component '%s' not found in Blueprint '%s'"), *CompFilter, *BpPath)));
				return;
			}
		}
		else
		{
			// All SCS components defined on this Blueprint.
			if (SCS)
			{
				for (USCS_Node* Node : SCS->GetAllNodes())
				{
					if (!Node) { continue; }
					Components.Add(MakeShared<FJsonValueObject>(SerializeComponent(
						Node->GetVariableName().ToString(), Node->ComponentClass, Node->ComponentTemplate)));
				}
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),    true);
		Out->SetStringField(TEXT("blueprint"),  BpPath);
		Out->SetArrayField (TEXT("components"), Components);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Out, Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/add_component_to_blueprint
// Adds a new component to a Blueprint's SimpleConstructionScript.
// Body: { "asset_path": "...", "component_class": "UCesiumIonRasterOverlay",
//         "component_name": "RasterOverlay",
//         "properties": [{"name":"IonAssetID","value":"2"}] }
// ============================================================================

bool FNGGHttpServer::HandleAddComponentToBlueprint(
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

	FString AssetPath;      Body->TryGetStringField(TEXT("asset_path"),      AssetPath);
	FString ComponentClass; Body->TryGetStringField(TEXT("component_class"), ComponentClass);
	FString ComponentName;  Body->TryGetStringField(TEXT("component_name"),  ComponentName);
	// Optional: name of an existing scene component to attach the new one under.
	// When empty, scene components attach to the default scene root (legacy behavior).
	FString AttachParent;   Body->TryGetStringField(TEXT("attach_parent"),   AttachParent);

	TArray<TSharedPtr<FJsonValue>> PropsJson;
	if (Body->HasField(TEXT("properties")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PropsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("properties"), PropsPtr) || !PropsPtr)
		{
			Callback(JsonError(400, TEXT("'properties' must be an array")));
			return true;
		}
		PropsJson = *PropsPtr;
	}

	if (AssetPath.IsEmpty() || ComponentClass.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path and component_class are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, ComponentClass, ComponentName, AttachParent, PropsJson]()
	{
		// Load the Blueprint
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Blueprint not found at '%s'"), *AssetPath)));
			return;
		}

		// Resolve the component UClass.
		// Prefer quiet, path-based resolution; only fall back to the (slow,
		// deprecation-warning-emitting) short-name lookup as a last resort. Passing
		// a short name to TryFindTypeSlow logs "Short type name ... convert it to a
		// path name" on every call, which spammed the log during Blueprint authoring.
		UClass* CompClass = nullptr;

		// 1) Caller passed a full path / object path (e.g. "/Script/PCG.PCGComponent").
		if (ComponentClass.Contains(TEXT(".")) || ComponentClass.StartsWith(TEXT("/")))
		{
			CompClass = LoadObject<UClass>(nullptr, *ComponentClass);
		}

		// 2) Short name (e.g. "PCGComponent" / "USplineComponent"): class objects are
		//    stored under their bare name (no leading U/A prefix), so probe the common
		//    script packages directly — quiet, no TryFindTypeSlow warning.
		if (!CompClass)
		{
			FString Bare = ComponentClass;
			if (Bare.StartsWith(TEXT("U")) || Bare.StartsWith(TEXT("A")))
			{
				Bare = Bare.Mid(1);
			}
			static const TCHAR* ScriptModules[] = {
				TEXT("/Script/Engine"), TEXT("/Script/PCG"), TEXT("/Script/UMG"),
				TEXT("/Script/CableComponent"), TEXT("/Script/Niagara"),
				TEXT("/Script/GameplayAbilities"),
			};
			for (const TCHAR* Module : ScriptModules)
			{
				CompClass = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("%s.%s"), Module, *Bare));
				if (CompClass)
				{
					break;
				}
			}
		}

		// 3) Last resort: short-name type search. May emit the deprecation warning,
		//    but only for classes not covered by the quiet paths above.
		if (!CompClass)
		{
			CompClass = UClass::TryFindTypeSlow<UClass>(ComponentClass);
		}

		if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("'%s' is not a valid UActorComponent class"), *ComponentClass)));
			return;
		}

		// Ensure the Blueprint has a SimpleConstructionScript
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Callback(JsonError(500, TEXT("Blueprint has no SimpleConstructionScript")));
			return;
		}

		// Determine the component name
		FName CompFName;
		if (ComponentName.IsEmpty())
		{
			CompFName = FName(*CompClass->GetName());
		}
		else
		{
			CompFName = FName(*ComponentName);
		}

		// Check if a component with this name already exists
		for (USCS_Node* Existing : SCS->GetAllNodes())
		{
			if (Existing && Existing->GetVariableName() == CompFName)
			{
				Callback(JsonError(409, FString::Printf(
					TEXT("Component '%s' already exists on '%s'"),
					*CompFName.ToString(), *AssetPath)));
				return;
			}
		}

		// Create the SCS node
		USCS_Node* NewNode = SCS->CreateNode(CompClass, CompFName);
		if (!NewNode)
		{
			Callback(JsonError(500, TEXT("Failed to create SCS node")));
			return;
		}

		// Decide where the new node lands in the component tree.
		//   - Non-scene components (e.g. UActorComponent) are always top-level
		//     SCS nodes; they have no transform and cannot be attached.
		//   - Scene components attach to `attach_parent` when given and valid,
		//     otherwise to the default scene root (legacy behavior).
		const bool bIsSceneComp = CompClass->IsChildOf(USceneComponent::StaticClass());
		FString AttachWarning;   // surfaced under "warnings" below
		FString AttachedUnder;   // reported under "attached_to" below

		if (!bIsSceneComp)
		{
			SCS->AddNode(NewNode);
			if (!AttachParent.IsEmpty())
			{
				AttachWarning = FString::Printf(
					TEXT("attach_parent '%s' ignored — '%s' is not a scene component and cannot be attached"),
					*AttachParent, *CompClass->GetName());
			}
		}
		else if (!AttachParent.IsEmpty())
		{
			// Find the requested parent SCS node by variable name.
			const FName ParentFName(*AttachParent);
			USCS_Node* ParentNode = nullptr;
			for (USCS_Node* N : SCS->GetAllNodes())
			{
				if (N && N->GetVariableName() == ParentFName)
				{
					ParentNode = N;
					break;
				}
			}

			if (!ParentNode)
			{
				TArray<FString> Available;
				for (USCS_Node* N : SCS->GetAllNodes())
				{
					if (N) Available.Add(N->GetVariableName().ToString());
				}
				Callback(JsonError(404, FString::Printf(
					TEXT("attach_parent '%s' not found on '%s'. Available components: [%s]"),
					*AttachParent, *AssetPath, *FString::Join(Available, TEXT(", ")))));
				return;
			}

			// The parent must itself be a scene component to host a child transform.
			UClass* ParentClass = ParentNode->ComponentClass;
			if (ParentClass && !ParentClass->IsChildOf(USceneComponent::StaticClass()))
			{
				Callback(JsonError(400, FString::Printf(
					TEXT("attach_parent '%s' is not a scene component — cannot attach '%s' under it"),
					*AttachParent, *CompFName.ToString())));
				return;
			}

			ParentNode->AddChildNode(NewNode);
			AttachedUnder = AttachParent;
		}
		else if (SCS->GetDefaultSceneRootNode())
		{
			SCS->GetDefaultSceneRootNode()->AddChildNode(NewNode);
			AttachedUnder = SCS->GetDefaultSceneRootNode()->GetVariableName().ToString();
		}
		else
		{
			// No root yet: this scene component becomes the default scene root.
			SCS->AddNode(NewNode);
		}

		// Apply initial properties if provided
		UActorComponent* CompTemplate = NewNode->ComponentTemplate;
		int32 SetCount = 0;
		TArray<FString> Warnings;
		if (!AttachWarning.IsEmpty())
		{
			Warnings.Add(AttachWarning);
		}

		if (CompTemplate && PropsJson.Num() > 0)
		{
			for (const TSharedPtr<FJsonValue>& Entry : PropsJson)
			{
				const TSharedPtr<FJsonObject>* EntryObj = nullptr;
				if (!Entry.IsValid() || !Entry->TryGetObject(EntryObj) || !EntryObj)
				{
					Warnings.Add(TEXT("Skipped non-object entry in properties array"));
					continue;
				}

				FString PropName, ValueStr;
				(*EntryObj)->TryGetStringField(TEXT("name"),  PropName);
				(*EntryObj)->TryGetStringField(TEXT("value"), ValueStr);

				if (PropName.IsEmpty())
				{
					Warnings.Add(TEXT("Skipped entry with empty property name"));
					continue;
				}

				FProperty* Prop = CompTemplate->GetClass()->FindPropertyByName(FName(*PropName));
				if (!Prop)
				{
					Warnings.Add(FString::Printf(TEXT("Property '%s' not found on %s"),
						*PropName, *CompTemplate->GetClass()->GetName()));
					continue;
				}

				{
					FString MeshErr; bool bMeshOk = false;
					if (TrySetMeshPropertyViaSetter(CompTemplate, FName(*PropName), ValueStr, MeshErr, bMeshOk))
					{
						if (bMeshOk) ++SetCount;
						else if (!MeshErr.IsEmpty()) Warnings.Add(MeshErr);
						continue;
					}
				}

				const TCHAR* ImportResult = Prop->ImportText_Direct(
					*ValueStr,
					Prop->ContainerPtrToValuePtr<void>(CompTemplate),
					CompTemplate,
					PPF_None);

				if (ImportResult)
				{
					SetCount++;
				}
				else
				{
					Warnings.Add(FString::Printf(TEXT("Failed to import value '%s' for property '%s'"),
						*ValueStr, *PropName));
				}
			}
		}

		// Mark modified and recompile
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		if (Blueprint->ParentClass != nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		Blueprint->MarkPackageDirty();

		// Build response
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),          true);
		Resp->SetStringField(TEXT("asset_path"),       AssetPath);
		Resp->SetStringField(TEXT("component_name"),   CompFName.ToString());
		Resp->SetStringField(TEXT("component_class"),  CompClass->GetName());
		if (!AttachedUnder.IsEmpty())
		{
			Resp->SetStringField(TEXT("attached_to"), AttachedUnder);
		}
		Resp->SetNumberField(TEXT("properties_set"),   static_cast<double>(SetCount));

		TArray<TSharedPtr<FJsonValue>> WarnArray;
		for (const FString& W : Warnings)
		{
			WarnArray.Add(MakeShared<FJsonValueString>(W));
		}
		Resp->SetArrayField(TEXT("warnings"), WarnArray);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/remove_component_from_blueprint
// Removes a component from a Blueprint's SimpleConstructionScript by name.
// Children of the removed node are promoted to its parent so they aren't lost.
// Refuses to remove the default scene root (it's a structural anchor).
// Body: { "asset_path": "...", "component_name": "Box" }
// ============================================================================

bool FNGGHttpServer::HandleRemoveComponentFromBlueprint(
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

	FString AssetPath;     Body->TryGetStringField(TEXT("asset_path"),     AssetPath);
	FString ComponentName; Body->TryGetStringField(TEXT("component_name"), ComponentName);

	if (AssetPath.IsEmpty() || ComponentName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path and component_name are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, ComponentName]()
	{
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Blueprint not found at '%s'"), *AssetPath)));
			return;
		}

		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			Callback(JsonError(500, TEXT("Blueprint has no SimpleConstructionScript")));
			return;
		}

		const FName WantName(*ComponentName);
		USCS_Node* Target = nullptr;
		for (USCS_Node* N : SCS->GetAllNodes())
		{
			if (N && N->GetVariableName() == WantName)
			{
				Target = N;
				break;
			}
		}
		if (!Target)
		{
			TArray<FString> Available;
			for (USCS_Node* N : SCS->GetAllNodes())
			{
				if (N) Available.Add(N->GetVariableName().ToString());
			}
			Callback(JsonError(404, FString::Printf(
				TEXT("Component '%s' not found on '%s'. Available: [%s]"),
				*ComponentName, *AssetPath, *FString::Join(Available, TEXT(", ")))));
			return;
		}

		if (Target == SCS->GetDefaultSceneRootNode())
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("Cannot remove the default scene root component '%s'"),
				*ComponentName)));
			return;
		}

		const int32 PromotedChildren = Target->GetChildNodes().Num();

		// RemoveNodeAndPromoteChildren handles both root-level and nested SCS nodes
		// and re-parents the deleted node's children so they aren't dropped.
		SCS->RemoveNodeAndPromoteChildren(Target);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		if (Blueprint->ParentClass != nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),           true);
		Resp->SetStringField(TEXT("asset_path"),        AssetPath);
		Resp->SetStringField(TEXT("component_name"),    ComponentName);
		Resp->SetNumberField(TEXT("promoted_children"), static_cast<double>(PromotedChildren));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
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

	AsyncTask(ENamedThreads::GameThread, [Callback, LevelPath]()
	{
		// Check if map already exists on disk
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
			return;
		}

		// Create a new blank map (this opens it in the editor)
		UWorld* NewWorld = GEditor->NewMap();
		if (!NewWorld)
		{
			Callback(JsonError(500, TEXT("GEditor->NewMap() returned nullptr")));
			return;
		}

		// Rename the package to the requested path
		UPackage* Package = NewWorld->GetOutermost();
		if (Package)
		{
			Package->Rename(*LevelPath);
			NewWorld->Rename(*FPackageName::GetShortName(LevelPath), Package);
		}

		// Save the level to disk
		FString FilePath = FPackageName::LongPackageNameToFilename(
			LevelPath, FPackageName::GetMapPackageExtension());
		bool bSaved = FEditorFileUtils::SaveLevel(NewWorld->PersistentLevel, FilePath);

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         bSaved);
		Resp->SetBoolField  (TEXT("already_existed"), false);
		Resp->SetBoolField  (TEXT("created"),         bSaved);
		Resp->SetStringField(TEXT("level_path"),      LevelPath);
		Resp->SetStringField(TEXT("file_path"),       FilePath);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		bSaved ? Callback(JsonCreated(RespBody))
		       : Callback(JsonError(500, TEXT("Failed to save level to disk")));
	});

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
// Handler: POST /editor/style_widgets
//
// Body:
// {
//   "widget_blueprint": "/Game/UI/WBP_MainMenu",
//   "styles": [
//     {
//       "widget_name":          "StartButton",      // required — WidgetTree FName
//       "font_size":            36,                 // UTextBlock / UEditableTextBox
//       "font_bold":            true,               // UTextBlock
//       "text":                 "AVVIA",            // UTextBlock / UEditableTextBox
//       "auto_wrap":            false,              // UTextBlock
//       "color":                "#58A6FF",          // UTextBlock foreground; UProgressBar fill
//       "fill_color":           "#58A6FF",          // UProgressBar (alias for color)
//       "background_color":     "#1C1C1C",          // UButton Normal/Hovered/Pressed tint
//       "width_override":       320,               // UCanvasPanelSlot size X
//       "height_override":      72,                // UCanvasPanelSlot size Y
//       "anchor":               "bottom-right",    // UCanvasPanelSlot FAnchors preset
//       "position_x":           -220,              // UCanvasPanelSlot position offset X
//       "position_y":           -90,               // UCanvasPanelSlot position offset Y
//       "alignment_x":          1.0,               // UCanvasPanelSlot pivot X (0-1)
//       "alignment_y":          1.0,               // UCanvasPanelSlot pivot Y (0-1)
//       "visibility":           "Visible",          // any UWidget
//       "horizontal_alignment": "Center",           // UTextBlock justification
//       "padding": { "left":16,"top":8,"right":16,"bottom":8 }, // UCanvasPanelSlot
//       "percent":              0.75               // UProgressBar
//     }
//   ]
// }
//
// Unknown widget names and unrecognised property keys are skipped silently.
// Returns per-widget applied/skipped lists plus aggregate totals.
// ============================================================================

// ============================================================================
// Handler: POST /editor/get_widget_tree
// Returns the full widget hierarchy of a Widget Blueprint as nested JSON.
// ============================================================================

bool FNGGHttpServer::HandleGetWidgetTree(
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

	FString WBPPath;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [WBPPath, Callback]()
	{
		TFunction<TSharedPtr<FJsonObject>(UWidget*)> BuildNode;
		BuildNode = [&BuildNode](UWidget* W) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("name"),  W->GetFName().ToString());
			Node->SetStringField(TEXT("class"), W->GetClass()->GetName());

			if (UUserWidget* UW = Cast<UUserWidget>(W))
			{
				if (W->GetClass() != UUserWidget::StaticClass())
				{
					Node->SetBoolField(TEXT("is_user_widget_instance"), true);
					Node->SetStringField(TEXT("user_widget_class"), W->GetClass()->GetPathName());
				}
			}

			Node->SetBoolField(TEXT("is_enabled"),  W->GetIsEnabled());
			Node->SetBoolField(TEXT("is_variable"), W->bIsVariable);
			const FString Tip = W->GetToolTipText().ToString();
			if (!Tip.IsEmpty()) Node->SetStringField(TEXT("tooltip"), Tip);

			if (W->Slot)
			{
				Node->SetStringField(TEXT("slot_class"), W->Slot->GetClass()->GetName());
				TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();

				if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(W->Slot))
				{
					FVector2D Pos = CSlot->GetPosition();
					FVector2D Sz  = CSlot->GetSize();
					FAnchors  An  = CSlot->GetAnchors();
					FVector2D Al  = CSlot->GetAlignment();
					int32 Z = 0;
					if (FIntProperty* P = FindFProperty<FIntProperty>(CSlot->GetClass(), TEXT("ZOrder")))
					{
						Z = P->GetPropertyValue_InContainer(CSlot);
					}
					SlotObj->SetNumberField(TEXT("pos_x"),         Pos.X);
					SlotObj->SetNumberField(TEXT("pos_y"),         Pos.Y);
					SlotObj->SetNumberField(TEXT("width"),         Sz.X);
					SlotObj->SetNumberField(TEXT("height"),        Sz.Y);
					SlotObj->SetNumberField(TEXT("anchor_min_x"),  An.Minimum.X);
					SlotObj->SetNumberField(TEXT("anchor_min_y"),  An.Minimum.Y);
					SlotObj->SetNumberField(TEXT("anchor_max_x"),  An.Maximum.X);
					SlotObj->SetNumberField(TEXT("anchor_max_y"),  An.Maximum.Y);
					SlotObj->SetNumberField(TEXT("alignment_x"),   Al.X);
					SlotObj->SetNumberField(TEXT("alignment_y"),   Al.Y);
					SlotObj->SetNumberField(TEXT("z_order"),       Z);
					SlotObj->SetBoolField  (TEXT("auto_size"),     CSlot->GetAutoSize());
					Node->SetObjectField(TEXT("canvas_slot"), SlotObj);
				}
				else if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(VS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(VS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(VS->GetVerticalAlignment()));
					const FSlateChildSize Sz = VS->GetSize();
					SlotObj->SetStringField(TEXT("size_rule"), Sz.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto"));
					SlotObj->SetNumberField(TEXT("size_value"), Sz.Value);
				}
				else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(HS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(HS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(HS->GetVerticalAlignment()));
					const FSlateChildSize Sz = HS->GetSize();
					SlotObj->SetStringField(TEXT("size_rule"), Sz.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto"));
					SlotObj->SetNumberField(TEXT("size_value"), Sz.Value);
				}
				else if (UOverlaySlot* OS = Cast<UOverlaySlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(OS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(OS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(OS->GetVerticalAlignment()));
				}
				else if (UGridSlot* GS = Cast<UGridSlot>(W->Slot))
				{
					SlotObj->SetNumberField(TEXT("row"),         GS->GetRow());
					SlotObj->SetNumberField(TEXT("column"),      GS->GetColumn());
					SlotObj->SetNumberField(TEXT("row_span"),    GS->GetRowSpan());
					SlotObj->SetNumberField(TEXT("column_span"), GS->GetColumnSpan());
					SlotObj->SetObjectField(TEXT("padding"),     NGGWidgetPriv::MarginToJson(GS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"),     NGGWidgetPriv::HAlignToString(GS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"),     NGGWidgetPriv::VAlignToString(GS->GetVerticalAlignment()));
					SlotObj->SetNumberField(TEXT("layer"),       GS->GetLayer());
				}
				else if (UUniformGridSlot* UGS = Cast<UUniformGridSlot>(W->Slot))
				{
					SlotObj->SetNumberField(TEXT("row"),         UGS->GetRow());
					SlotObj->SetNumberField(TEXT("column"),      UGS->GetColumn());
					SlotObj->SetStringField(TEXT("h_align"),     NGGWidgetPriv::HAlignToString(UGS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"),     NGGWidgetPriv::VAlignToString(UGS->GetVerticalAlignment()));
				}
				else if (UScrollBoxSlot* SS = Cast<UScrollBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(SS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(SS->GetHorizontalAlignment()));
				}
				else if (UBorderSlot* BS = Cast<UBorderSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(BS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(BS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(BS->GetVerticalAlignment()));
				}
				else if (UWrapBoxSlot* WS = Cast<UWrapBoxSlot>(W->Slot))
				{
					SlotObj->SetObjectField(TEXT("padding"), NGGWidgetPriv::MarginToJson(WS->GetPadding()));
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(WS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(WS->GetVerticalAlignment()));
					if (FFloatProperty* P = FindFProperty<FFloatProperty>(WS->GetClass(), TEXT("FillSpanWhenLessThan")))
					{
						SlotObj->SetNumberField(TEXT("fill_span"), P->GetPropertyValue_InContainer(WS));
					}
					if (FBoolProperty* P2 = FindFProperty<FBoolProperty>(WS->GetClass(), TEXT("bFillEmptySpace")))
					{
						SlotObj->SetBoolField(TEXT("fill_empty_space"), P2->GetPropertyValue_InContainer(WS));
					}
				}
				else if (UScaleBoxSlot* SBS = Cast<UScaleBoxSlot>(W->Slot))
				{
					SlotObj->SetStringField(TEXT("h_align"), NGGWidgetPriv::HAlignToString(SBS->GetHorizontalAlignment()));
					SlotObj->SetStringField(TEXT("v_align"), NGGWidgetPriv::VAlignToString(SBS->GetVerticalAlignment()));
				}

				if (SlotObj->Values.Num() > 0 && !Node->HasField(TEXT("canvas_slot")))
				{
					Node->SetObjectField(TEXT("slot"), SlotObj);
				}
			}

			if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
			{
				TArray<TSharedPtr<FJsonValue>> ChildArr;
				for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
				{
					if (UWidget* Child = Panel->GetChildAt(i))
					{
						ChildArr.Add(MakeShared<FJsonValueObject>(BuildNode(Child)));
					}
				}
				Node->SetArrayField(TEXT("children"), ChildArr);
			}

			if (UTextBlock* TB = Cast<UTextBlock>(W))
			{
				Node->SetStringField(TEXT("text"),  TB->GetText().ToString());
				const FSlateFontInfo Font = TB->GetFont();
				Node->SetNumberField(TEXT("font_size"), Font.Size);
				Node->SetStringField(TEXT("font_typeface"), Font.TypefaceFontName.ToString());
				Node->SetStringField(TEXT("color"), NGGWidgetPriv::ColorToHex(TB->GetColorAndOpacity().GetSpecifiedColor()));
			}
			else if (UButton* Btn = Cast<UButton>(W))
			{
				const FButtonStyle& Style = Btn->GetStyle();
				Node->SetStringField(TEXT("background_color"),
					NGGWidgetPriv::ColorToHex(Style.Normal.TintColor.GetSpecifiedColor()));
			}
			else if (UImage* Img = Cast<UImage>(W))
			{
				const FSlateBrush& B = Img->GetBrush();
				if (B.GetResourceObject())
				{
					Node->SetStringField(TEXT("brush_texture"), B.GetResourceObject()->GetPathName());
				}
				Node->SetStringField(TEXT("brush_tint"),  NGGWidgetPriv::ColorToHex(Img->GetColorAndOpacity()));
				Node->SetNumberField(TEXT("brush_size_x"),B.ImageSize.X);
				Node->SetNumberField(TEXT("brush_size_y"),B.ImageSize.Y);
				Node->SetStringField(TEXT("brush_draw_as"), NGGWidgetPriv::BrushDrawAsToString((ESlateBrushDrawType::Type)B.DrawAs));
			}
			else if (UBorder* Brd = Cast<UBorder>(W))
			{
				if (FStructProperty* P = FindFProperty<FStructProperty>(Brd->GetClass(), TEXT("Background")))
				{
					if (FSlateBrush* B = P->ContainerPtrToValuePtr<FSlateBrush>(Brd))
					{
						if (B->GetResourceObject())
						{
							Node->SetStringField(TEXT("brush_texture"), B->GetResourceObject()->GetPathName());
						}
					}
				}
				Node->SetStringField(TEXT("brush_tint"), NGGWidgetPriv::ColorToHex(Brd->GetBrushColor()));
			}
			else if (UProgressBar* PB = Cast<UProgressBar>(W))
			{
				Node->SetNumberField(TEXT("percent"), PB->GetPercent());
				Node->SetStringField(TEXT("fill_color"),
					NGGWidgetPriv::ColorToHex(PB->GetFillColorAndOpacity()));
			}
			else if (USlider* Sl = Cast<USlider>(W))
			{
				Node->SetNumberField(TEXT("value"),     Sl->GetValue());
				Node->SetNumberField(TEXT("min_value"), Sl->GetMinValue());
				Node->SetNumberField(TEXT("max_value"), Sl->GetMaxValue());
			}
			else if (UCheckBox* CB = Cast<UCheckBox>(W))
			{
				Node->SetBoolField(TEXT("is_checked"), CB->IsChecked());
			}

			return Node;
		};

		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		if (UWidget* Root = WBP->WidgetTree->RootWidget)
		{
			Result->SetObjectField(TEXT("root"), BuildNode(Root));
		}
		else
		{
			Result->SetBoolField(TEXT("root"), false);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(FHttpServerResponse::Create(BodyStr, TEXT("application/json")));
	});

	return true;
}

bool FNGGHttpServer::HandleStyleWidgets(
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

	FString WBPPath; Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> StylesJson;
	if (Body->HasField(TEXT("styles")))
	{
		const TArray<TSharedPtr<FJsonValue>>* StylesPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("styles"), StylesPtr) || !StylesPtr)
		{
			Callback(JsonError(400, TEXT("'styles' must be an array")));
			return true;
		}
		StylesJson = *StylesPtr;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, WBPPath, StylesJson]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP))
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Widget Blueprint not found at '%s'"), *WBPPath)));
			return;
		}
		if (!IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(500, TEXT("Widget Blueprint has no WidgetTree")));
			return;
		}

		auto ParseJustify = [](const FString& H) -> ETextJustify::Type
		{
			if (H == TEXT("Right"))  return ETextJustify::Right;
			if (H == TEXT("Center")) return ETextJustify::Center;
			return ETextJustify::Left;
		};

		static const TArray<FString> AllKnownKeys = {
			// universal
			TEXT("visibility"),  TEXT("tooltip_text"), TEXT("is_enabled"), TEXT("is_variable"),
			TEXT("render_translation_x"), TEXT("render_translation_y"),
			TEXT("render_angle"), TEXT("render_scale_x"), TEXT("render_scale_y"),
			TEXT("render_shear_x"), TEXT("render_shear_y"),
			TEXT("render_pivot_x"), TEXT("render_pivot_y"),
			TEXT("clip_to_bounds"), TEXT("clipping"),
			// canvas-slot
			TEXT("width_override"), TEXT("height_override"), TEXT("padding"),
			TEXT("anchor"), TEXT("position_x"), TEXT("position_y"),
			TEXT("alignment_x"), TEXT("alignment_y"), TEXT("z_order"), TEXT("auto_size"),
			// generic slot
			TEXT("slot_padding"), TEXT("slot_h_align"), TEXT("slot_v_align"),
			TEXT("slot_size_rule"), TEXT("slot_size_value"),
			TEXT("slot_row"), TEXT("slot_column"), TEXT("slot_row_span"), TEXT("slot_column_span"),
			TEXT("slot_z_order"),
			// text
			TEXT("font_size"), TEXT("font_bold"), TEXT("font_typeface"),
			TEXT("text"), TEXT("color"), TEXT("auto_wrap"), TEXT("horizontal_alignment"),
			TEXT("shadow_offset_x"), TEXT("shadow_offset_y"), TEXT("shadow_color"),
			TEXT("outline_size"), TEXT("outline_color"), TEXT("min_desired_width"),
			// button
			TEXT("background_color"), TEXT("hovered_color"), TEXT("pressed_color"),
			TEXT("disabled_color"), TEXT("content_padding"), TEXT("click_method"), TEXT("is_focusable"),
			// image / border
			TEXT("brush_texture"), TEXT("brush_tint"), TEXT("brush_size_x"), TEXT("brush_size_y"),
			TEXT("brush_draw_as"), TEXT("brush_margin"), TEXT("brush_tiling"),
			TEXT("vertical_alignment"), TEXT("content_color"),
			// progress bar
			TEXT("fill_color"), TEXT("percent"), TEXT("bar_fill_type"), TEXT("marquee"),
			// slider
			TEXT("value"), TEXT("min_value"), TEXT("max_value"), TEXT("step_size"),
			TEXT("orientation"), TEXT("slider_bar_color"), TEXT("slider_handle_color"),
			// scroll box
			TEXT("scroll_bar_visibility"), TEXT("allow_overscroll"),
			TEXT("scroll_bar_thickness"), TEXT("always_show_scrollbar"),
			// size box
			TEXT("min_width"), TEXT("min_height"), TEXT("max_width"), TEXT("max_height"),
			TEXT("min_aspect_ratio"), TEXT("max_aspect_ratio"),
			TEXT("clear_width_override"), TEXT("clear_height_override"),
			// spacer / switcher / rtb / throbber / blur / safezone / scalebox / combobox / checkbox
			TEXT("size_x"), TEXT("size_y"),
			TEXT("active_widget_index"),
			TEXT("text_style_set"),
			TEXT("number_of_pieces"), TEXT("period"), TEXT("radius"),
			TEXT("animate_horizontally"), TEXT("animate_vertically"), TEXT("animate_opacity"),
			TEXT("blur_strength"), TEXT("blur_radius"),
			TEXT("pad_left"), TEXT("pad_top"), TEXT("pad_right"), TEXT("pad_bottom"),
			TEXT("pad_sides"), TEXT("is_titlesafe"),
			TEXT("stretch"), TEXT("stretch_direction"), TEXT("user_specified_scale"),
			TEXT("add_option"), TEXT("clear_options"), TEXT("selected_option"),
			TEXT("is_checked"), TEXT("check_type")
		};

		// ---------- 4. Process each style entry ------------------------------
		TArray<TSharedPtr<FJsonValue>> ResultsJson;
		int32 TotalApplied = 0;
		int32 TotalSkipped = 0;

		for (const TSharedPtr<FJsonValue>& Entry : StylesJson)
		{
			const TSharedPtr<FJsonObject>* EntryObjPtr;
			if (!Entry->TryGetObject(EntryObjPtr)) continue;
			const TSharedPtr<FJsonObject>& S = *EntryObjPtr;

			FString WidgetName;
			S->TryGetStringField(TEXT("widget_name"), WidgetName);
			if (WidgetName.IsEmpty()) continue;

			// Build per-widget result object
			TSharedPtr<FJsonObject> WidgetResult = MakeShared<FJsonObject>();
			WidgetResult->SetStringField(TEXT("widget_name"), WidgetName);

			UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
			if (!IsValid(W))
			{
				WidgetResult->SetStringField(TEXT("status"), TEXT("not_found"));
				ResultsJson.Add(MakeShared<FJsonValueObject>(WidgetResult));
				++TotalSkipped;
				continue;
			}

			TArray<FString> Applied;
			TArray<FString> Skipped;

			auto AddUnsupportedSlot = [&](const FString& Key)
			{
				const FString ClassName = W->Slot ? W->Slot->GetClass()->GetName() : TEXT("None");
				Skipped.Add(FString::Printf(TEXT("%s (unsupported slot type: %s)"), *Key, *ClassName));
			};
			auto AddUnsupportedWidget = [&](const FString& Key)
			{
				Skipped.Add(FString::Printf(TEXT("%s (unsupported widget type: %s)"),
					*Key, *W->GetClass()->GetName()));
			};

			// ---------- Universal UWidget properties --------------------------
			FString VisStr;
			if (S->TryGetStringField(TEXT("visibility"), VisStr))
			{
				W->SetVisibility(NGGWidgetPriv::ParseVisibility(VisStr));
				Applied.Add(TEXT("visibility"));
			}
			FString TipStr;
			if (S->TryGetStringField(TEXT("tooltip_text"), TipStr))
			{
				W->SetToolTipText(FText::FromString(TipStr));
				Applied.Add(TEXT("tooltip_text"));
			}
			bool bEnabled = false;
			if (S->TryGetBoolField(TEXT("is_enabled"), bEnabled))
			{
				W->SetIsEnabled(bEnabled);
				Applied.Add(TEXT("is_enabled"));
			}
			bool bIsVar = false;
			if (S->TryGetBoolField(TEXT("is_variable"), bIsVar))
			{
				W->bIsVariable = bIsVar;
				Applied.Add(TEXT("is_variable"));
			}
			{
				double TX=0, TY=0, Ang=0, ScX=1, ScY=1, ShX=0, ShY=0;
				const bool bHasTX = S->TryGetNumberField(TEXT("render_translation_x"), TX);
				const bool bHasTY = S->TryGetNumberField(TEXT("render_translation_y"), TY);
				const bool bHasA  = S->TryGetNumberField(TEXT("render_angle"),         Ang);
				const bool bHasScX= S->TryGetNumberField(TEXT("render_scale_x"),       ScX);
				const bool bHasScY= S->TryGetNumberField(TEXT("render_scale_y"),       ScY);
				const bool bHasShX= S->TryGetNumberField(TEXT("render_shear_x"),       ShX);
				const bool bHasShY= S->TryGetNumberField(TEXT("render_shear_y"),       ShY);
				if (bHasTX||bHasTY||bHasA||bHasScX||bHasScY||bHasShX||bHasShY)
				{
					FWidgetTransform T = W->GetRenderTransform();
					if (bHasTX) T.Translation.X = (float)TX;
					if (bHasTY) T.Translation.Y = (float)TY;
					if (bHasA)  T.Angle         = (float)Ang;
					if (bHasScX) T.Scale.X      = (float)ScX;
					if (bHasScY) T.Scale.Y      = (float)ScY;
					if (bHasShX) T.Shear.X      = (float)ShX;
					if (bHasShY) T.Shear.Y      = (float)ShY;
					W->SetRenderTransform(T);
					if (bHasTX) Applied.Add(TEXT("render_translation_x"));
					if (bHasTY) Applied.Add(TEXT("render_translation_y"));
					if (bHasA)  Applied.Add(TEXT("render_angle"));
					if (bHasScX) Applied.Add(TEXT("render_scale_x"));
					if (bHasScY) Applied.Add(TEXT("render_scale_y"));
					if (bHasShX) Applied.Add(TEXT("render_shear_x"));
					if (bHasShY) Applied.Add(TEXT("render_shear_y"));
				}
			}
			{
				double PX = 0.5, PY = 0.5;
				const bool bHasPX = S->TryGetNumberField(TEXT("render_pivot_x"), PX);
				const bool bHasPY = S->TryGetNumberField(TEXT("render_pivot_y"), PY);
				if (bHasPX || bHasPY)
				{
					FVector2D Cur = W->RenderTransformPivot;
					if (bHasPX) Cur.X = (float)PX;
					if (bHasPY) Cur.Y = (float)PY;
					W->SetRenderTransformPivot(Cur);
					if (bHasPX) Applied.Add(TEXT("render_pivot_x"));
					if (bHasPY) Applied.Add(TEXT("render_pivot_y"));
				}
			}
			FString ClipStr;
			if (S->TryGetStringField(TEXT("clipping"), ClipStr))
			{
				W->SetClipping(NGGWidgetPriv::ParseClipping(ClipStr));
				Applied.Add(TEXT("clipping"));
			}
			bool bClip = false;
			if (S->TryGetBoolField(TEXT("clip_to_bounds"), bClip))
			{
				W->SetClipping(bClip ? EWidgetClipping::ClipToBounds : EWidgetClipping::Inherit);
				Applied.Add(TEXT("clip_to_bounds"));
			}

			// ---------- Slot dispatch ----------------------------------------
			if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(W->Slot))
			{
				double WidthOvr = 0, HeightOvr = 0;
				const bool bHasW = S->TryGetNumberField(TEXT("width_override"),  WidthOvr);
				const bool bHasH = S->TryGetNumberField(TEXT("height_override"), HeightOvr);
				if (bHasW || bHasH)
				{
					CSlot->SetAutoSize(false);
					FVector2D Sz = CSlot->GetSize();
					if (bHasW) Sz.X = (float)WidthOvr;
					if (bHasH) Sz.Y = (float)HeightOvr;
					CSlot->SetSize(Sz);
					if (bHasW) Applied.Add(TEXT("width_override"));
					if (bHasH) Applied.Add(TEXT("height_override"));
				}
				const TSharedPtr<FJsonObject>* PaddingObj;
				if (S->TryGetObjectField(TEXT("padding"), PaddingObj))
				{
					CSlot->SetOffsets(NGGWidgetPriv::ReadMargin(*PaddingObj));
					Applied.Add(TEXT("padding"));
				}
				FString AnchorStr;
				if (S->TryGetStringField(TEXT("anchor"), AnchorStr))
				{
					float AMinX=0, AMinY=0, AMaxX=0, AMaxY=0;
					if      (AnchorStr == TEXT("top-left"))      { AMinX=0;   AMinY=0;   AMaxX=0;   AMaxY=0; }
					else if (AnchorStr == TEXT("top-center"))    { AMinX=0.5f;AMinY=0;   AMaxX=0.5f;AMaxY=0; }
					else if (AnchorStr == TEXT("top-right"))     { AMinX=1;   AMinY=0;   AMaxX=1;   AMaxY=0; }
					else if (AnchorStr == TEXT("center-left"))   { AMinX=0;   AMinY=0.5f;AMaxX=0;   AMaxY=0.5f; }
					else if (AnchorStr == TEXT("center"))        { AMinX=0.5f;AMinY=0.5f;AMaxX=0.5f;AMaxY=0.5f; }
					else if (AnchorStr == TEXT("center-right"))  { AMinX=1;   AMinY=0.5f;AMaxX=1;   AMaxY=0.5f; }
					else if (AnchorStr == TEXT("bottom-left"))   { AMinX=0;   AMinY=1;   AMaxX=0;   AMaxY=1; }
					else if (AnchorStr == TEXT("bottom-center")) { AMinX=0.5f;AMinY=1;   AMaxX=0.5f;AMaxY=1; }
					else if (AnchorStr == TEXT("bottom-right"))  { AMinX=1;   AMinY=1;   AMaxX=1;   AMaxY=1; }
					else if (AnchorStr == TEXT("fill"))          { AMinX=0;   AMinY=0;   AMaxX=1;   AMaxY=1; }
					CSlot->SetAnchors(FAnchors(AMinX, AMinY, AMaxX, AMaxY));
					Applied.Add(TEXT("anchor"));
				}
				double PosX = 0, PosY = 0;
				const bool bHasPosX = S->TryGetNumberField(TEXT("position_x"), PosX);
				const bool bHasPosY = S->TryGetNumberField(TEXT("position_y"), PosY);
				if (bHasPosX || bHasPosY)
				{
					FVector2D Cur = CSlot->GetPosition();
					if (bHasPosX) Cur.X = (float)PosX;
					if (bHasPosY) Cur.Y = (float)PosY;
					CSlot->SetPosition(Cur);
					if (bHasPosX) Applied.Add(TEXT("position_x"));
					if (bHasPosY) Applied.Add(TEXT("position_y"));
				}
				double AlignX = 0, AlignY = 0;
				const bool bHasAX = S->TryGetNumberField(TEXT("alignment_x"), AlignX);
				const bool bHasAY = S->TryGetNumberField(TEXT("alignment_y"), AlignY);
				if (bHasAX || bHasAY)
				{
					FVector2D Cur = CSlot->GetAlignment();
					if (bHasAX) Cur.X = (float)AlignX;
					if (bHasAY) Cur.Y = (float)AlignY;
					CSlot->SetAlignment(Cur);
					if (bHasAX) Applied.Add(TEXT("alignment_x"));
					if (bHasAY) Applied.Add(TEXT("alignment_y"));
				}
				int32 Z = 0;
				if (S->TryGetNumberField(TEXT("z_order"), Z))
				{
					CSlot->SetZOrder(Z);
					Applied.Add(TEXT("z_order"));
				}
				bool bAutoSize = false;
				if (S->TryGetBoolField(TEXT("auto_size"), bAutoSize))
				{
					CSlot->SetAutoSize(bAutoSize);
					Applied.Add(TEXT("auto_size"));
				}
			}

			// Generic slot keys
			auto ReadSlotPadding = [&](auto* Slot) -> bool
			{
				const TSharedPtr<FJsonObject>* P;
				if (S->TryGetObjectField(TEXT("slot_padding"), P))
				{
					Slot->SetPadding(NGGWidgetPriv::ReadMargin(*P));
					Applied.Add(TEXT("slot_padding"));
					return true;
				}
				return false;
			};
			auto ReadSlotHAlign = [&](auto* Slot) -> bool
			{
				FString V;
				if (S->TryGetStringField(TEXT("slot_h_align"), V))
				{
					Slot->SetHorizontalAlignment(NGGWidgetPriv::ParseHAlign(V));
					Applied.Add(TEXT("slot_h_align"));
					return true;
				}
				return false;
			};
			auto ReadSlotVAlign = [&](auto* Slot) -> bool
			{
				FString V;
				if (S->TryGetStringField(TEXT("slot_v_align"), V))
				{
					Slot->SetVerticalAlignment(NGGWidgetPriv::ParseVAlign(V));
					Applied.Add(TEXT("slot_v_align"));
					return true;
				}
				return false;
			};
			auto ReadSlotSize = [&](auto* Slot) -> bool
			{
				FString Rule;
				if (S->TryGetStringField(TEXT("slot_size_rule"), Rule))
				{
					FSlateChildSize Sz;
					if (Rule == TEXT("Fill"))
					{
						Sz.SizeRule = ESlateSizeRule::Fill;
						double Val = 1.0;
						S->TryGetNumberField(TEXT("slot_size_value"), Val);
						Sz.Value = (float)Val;
					}
					else
					{
						Sz.SizeRule = ESlateSizeRule::Automatic;
					}
					Slot->SetSize(Sz);
					Applied.Add(TEXT("slot_size_rule"));
					if (S->HasField(TEXT("slot_size_value"))) Applied.Add(TEXT("slot_size_value"));
					return true;
				}
				return false;
			};

			if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(W->Slot))
			{
				ReadSlotPadding(VS); ReadSlotHAlign(VS); ReadSlotVAlign(VS); ReadSlotSize(VS);
			}
			else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(W->Slot))
			{
				ReadSlotPadding(HS); ReadSlotHAlign(HS); ReadSlotVAlign(HS); ReadSlotSize(HS);
			}
			else if (UOverlaySlot* OS = Cast<UOverlaySlot>(W->Slot))
			{
				ReadSlotPadding(OS); ReadSlotHAlign(OS); ReadSlotVAlign(OS);
			}
			else if (UGridSlot* GS = Cast<UGridSlot>(W->Slot))
			{
				ReadSlotPadding(GS); ReadSlotHAlign(GS); ReadSlotVAlign(GS);
				int32 Row=0, Col=0, RS=0, CS=0, Layer=0;
				if (S->TryGetNumberField(TEXT("slot_row"),         Row)) { GS->SetRow(Row);            Applied.Add(TEXT("slot_row")); }
				if (S->TryGetNumberField(TEXT("slot_column"),      Col)) { GS->SetColumn(Col);         Applied.Add(TEXT("slot_column")); }
				if (S->TryGetNumberField(TEXT("slot_row_span"),    RS))  { GS->SetRowSpan(RS);         Applied.Add(TEXT("slot_row_span")); }
				if (S->TryGetNumberField(TEXT("slot_column_span"), CS))  { GS->SetColumnSpan(CS);      Applied.Add(TEXT("slot_column_span")); }
				if (S->TryGetNumberField(TEXT("slot_z_order"),     Layer)){ GS->SetLayer(Layer);       Applied.Add(TEXT("slot_z_order")); }
			}
			else if (UUniformGridSlot* UGS = Cast<UUniformGridSlot>(W->Slot))
			{
				ReadSlotHAlign(UGS); ReadSlotVAlign(UGS);
				int32 Row=0, Col=0;
				if (S->TryGetNumberField(TEXT("slot_row"),    Row)) { UGS->SetRow(Row);    Applied.Add(TEXT("slot_row")); }
				if (S->TryGetNumberField(TEXT("slot_column"), Col)) { UGS->SetColumn(Col); Applied.Add(TEXT("slot_column")); }
			}
			else if (UWrapBoxSlot* WS = Cast<UWrapBoxSlot>(W->Slot))
			{
				ReadSlotPadding(WS); ReadSlotHAlign(WS); ReadSlotVAlign(WS);
			}
			else if (UScrollBoxSlot* SBSlot = Cast<UScrollBoxSlot>(W->Slot))
			{
				ReadSlotPadding(SBSlot); ReadSlotHAlign(SBSlot);
			}
			else if (UBorderSlot* BdSlot = Cast<UBorderSlot>(W->Slot))
			{
				ReadSlotPadding(BdSlot); ReadSlotHAlign(BdSlot); ReadSlotVAlign(BdSlot);
			}
			else if (UScaleBoxSlot* SLBSlot = Cast<UScaleBoxSlot>(W->Slot))
			{
				ReadSlotHAlign(SLBSlot); ReadSlotVAlign(SLBSlot);
			}
			else if (USafeZoneSlot* SZSlot = Cast<USafeZoneSlot>(W->Slot))
			{
				const TSharedPtr<FJsonObject>* P;
				if (S->TryGetObjectField(TEXT("slot_padding"), P))
				{
					SZSlot->SetPadding(NGGWidgetPriv::ReadMargin(*P));
					Applied.Add(TEXT("slot_padding"));
				}
				FString HA;
				if (S->TryGetStringField(TEXT("slot_h_align"), HA))
				{
					SZSlot->SetHorizontalAlignment(NGGWidgetPriv::ParseHAlign(HA));
					Applied.Add(TEXT("slot_h_align"));
				}
				FString VA;
				if (S->TryGetStringField(TEXT("slot_v_align"), VA))
				{
					SZSlot->SetVerticalAlignment(NGGWidgetPriv::ParseVAlign(VA));
					Applied.Add(TEXT("slot_v_align"));
				}
			}
			else
			{
				static const TArray<FString> SlotKeys = {
					TEXT("slot_padding"), TEXT("slot_h_align"), TEXT("slot_v_align"),
					TEXT("slot_size_rule"), TEXT("slot_size_value"),
					TEXT("slot_row"), TEXT("slot_column"), TEXT("slot_row_span"),
					TEXT("slot_column_span"), TEXT("slot_z_order")
				};
				for (const FString& K : SlotKeys)
				{
					if (S->HasField(K)) AddUnsupportedSlot(K);
				}
			}

			// ---------- Widget-class-specific properties ---------------------
			if (UTextBlock* TB = Cast<UTextBlock>(W))
			{
				FString TextStr;
				if (S->TryGetStringField(TEXT("text"), TextStr))
				{
					TB->SetText(FText::FromString(TextStr));
					Applied.Add(TEXT("text"));
				}

				FSlateFontInfo FontInfo = TB->GetFont();
				bool bFontDirty = false;
				double FontSizeVal = 0;
				if (S->TryGetNumberField(TEXT("font_size"), FontSizeVal))
				{
					FontInfo.Size = (int32)FontSizeVal;
					bFontDirty = true;
					Applied.Add(TEXT("font_size"));
				}
				bool bFontBold = false;
				if (S->TryGetBoolField(TEXT("font_bold"), bFontBold))
				{
					FontInfo.TypefaceFontName = bFontBold ? FName(TEXT("Bold")) : FName(TEXT("Regular"));
					bFontDirty = true;
					Applied.Add(TEXT("font_bold"));
				}
				FString Typeface;
				if (S->TryGetStringField(TEXT("font_typeface"), Typeface))
				{
					FontInfo.TypefaceFontName = FName(*Typeface);
					bFontDirty = true;
					Applied.Add(TEXT("font_typeface"));
				}
				double OutlineSize = 0;
				if (S->TryGetNumberField(TEXT("outline_size"), OutlineSize))
				{
					FontInfo.OutlineSettings.OutlineSize = (int32)OutlineSize;
					bFontDirty = true;
					Applied.Add(TEXT("outline_size"));
				}
				FString OutlineHex;
				if (S->TryGetStringField(TEXT("outline_color"), OutlineHex))
				{
					FontInfo.OutlineSettings.OutlineColor = NGGWidgetPriv::ParseHexColor(OutlineHex);
					bFontDirty = true;
					Applied.Add(TEXT("outline_color"));
				}
				if (bFontDirty) TB->SetFont(FontInfo);

				FString ColorHex;
				if (S->TryGetStringField(TEXT("color"), ColorHex))
				{
					TB->SetColorAndOpacity(FSlateColor(NGGWidgetPriv::ParseHexColor(ColorHex)));
					Applied.Add(TEXT("color"));
				}
				bool bAutoWrap = false;
				if (S->TryGetBoolField(TEXT("auto_wrap"), bAutoWrap))
				{
					TB->SetAutoWrapText(bAutoWrap);
					Applied.Add(TEXT("auto_wrap"));
				}
				FString HA;
				if (S->TryGetStringField(TEXT("horizontal_alignment"), HA))
				{
					TB->SetJustification(ParseJustify(HA));
					Applied.Add(TEXT("horizontal_alignment"));
				}

				double SOX=0, SOY=0;
				const bool bSOX = S->TryGetNumberField(TEXT("shadow_offset_x"), SOX);
				const bool bSOY = S->TryGetNumberField(TEXT("shadow_offset_y"), SOY);
				if (bSOX || bSOY)
				{
					FVector2D Cur(0, 0);
					if (FStructProperty* P = FindFProperty<FStructProperty>(TB->GetClass(), TEXT("ShadowOffset")))
					{
						if (FVector2D* V = P->ContainerPtrToValuePtr<FVector2D>(TB))
						{
							Cur = *V;
						}
					}
					if (bSOX) Cur.X = (float)SOX;
					if (bSOY) Cur.Y = (float)SOY;
					TB->SetShadowOffset(Cur);
					if (bSOX) Applied.Add(TEXT("shadow_offset_x"));
					if (bSOY) Applied.Add(TEXT("shadow_offset_y"));
				}
				FString ShadowHex;
				if (S->TryGetStringField(TEXT("shadow_color"), ShadowHex))
				{
					TB->SetShadowColorAndOpacity(NGGWidgetPriv::ParseHexColor(ShadowHex));
					Applied.Add(TEXT("shadow_color"));
				}
				double MinDW = 0;
				if (S->TryGetNumberField(TEXT("min_desired_width"), MinDW))
				{
					TB->SetMinDesiredWidth((float)MinDW);
					Applied.Add(TEXT("min_desired_width"));
				}
			}
			else if (URichTextBlock* RTB = Cast<URichTextBlock>(W))
			{
				FString TextStr;
				if (S->TryGetStringField(TEXT("text"), TextStr))
				{
					RTB->SetText(FText::FromString(TextStr));
					Applied.Add(TEXT("text"));
				}
				bool bAutoWrap = false;
				if (S->TryGetBoolField(TEXT("auto_wrap"), bAutoWrap))
				{
					RTB->SetAutoWrapText(bAutoWrap);
					Applied.Add(TEXT("auto_wrap"));
				}
				double MinDW = 0;
				if (S->TryGetNumberField(TEXT("min_desired_width"), MinDW))
				{
					if (FFloatProperty* P = FindFProperty<FFloatProperty>(RTB->GetClass(), TEXT("MinDesiredWidth")))
					{
						P->SetPropertyValue_InContainer(RTB, (float)MinDW);
						Applied.Add(TEXT("min_desired_width"));
					}
					else
					{
						Skipped.Add(TEXT("min_desired_width (property not found)"));
					}
				}
				FString StylePath;
				if (S->TryGetStringField(TEXT("text_style_set"), StylePath))
				{
					if (UDataTable* DT = LoadObject<UDataTable>(nullptr, *StylePath))
					{
						RTB->SetTextStyleSet(DT);
						Applied.Add(TEXT("text_style_set"));
					}
					else
					{
						Skipped.Add(TEXT("text_style_set (asset load failed)"));
					}
				}
			}
			else if (UButton* Btn = Cast<UButton>(W))
			{
				FButtonStyle Style = Btn->GetStyle();
				bool bStyleDirty = false;

				FString BgHex;
				if (S->TryGetStringField(TEXT("background_color"), BgHex))
				{
					Style.Normal.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(BgHex));
					bStyleDirty = true;
					Applied.Add(TEXT("background_color"));
				}
				FString HoverHex;
				if (S->TryGetStringField(TEXT("hovered_color"), HoverHex))
				{
					Style.Hovered.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(HoverHex));
					bStyleDirty = true;
					Applied.Add(TEXT("hovered_color"));
				}
				FString PressHex;
				if (S->TryGetStringField(TEXT("pressed_color"), PressHex))
				{
					Style.Pressed.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(PressHex));
					bStyleDirty = true;
					Applied.Add(TEXT("pressed_color"));
				}
				FString DisHex;
				if (S->TryGetStringField(TEXT("disabled_color"), DisHex))
				{
					Style.Disabled.TintColor = FSlateColor(NGGWidgetPriv::ParseHexColor(DisHex));
					bStyleDirty = true;
					Applied.Add(TEXT("disabled_color"));
				}
				const TSharedPtr<FJsonObject>* CPObj;
				if (S->TryGetObjectField(TEXT("content_padding"), CPObj))
				{
					const FMargin M = NGGWidgetPriv::ReadMargin(*CPObj);
					Style.NormalPadding  = M;
					Style.PressedPadding = M;
					bStyleDirty = true;
					Applied.Add(TEXT("content_padding"));
				}
				if (bStyleDirty) Btn->SetStyle(Style);

				FString Click;
				if (S->TryGetStringField(TEXT("click_method"), Click))
				{
					Btn->SetClickMethod(NGGWidgetPriv::ParseClickMethod(Click));
					Applied.Add(TEXT("click_method"));
				}
				bool bFocus = false;
				if (S->TryGetBoolField(TEXT("is_focusable"), bFocus))
				{
					if (FBoolProperty* P = FindFProperty<FBoolProperty>(Btn->GetClass(), TEXT("bIsFocusable")))
					{
						P->SetPropertyValue_InContainer(Btn, bFocus);
						Applied.Add(TEXT("is_focusable"));
					}
					else if (FBoolProperty* P2 = FindFProperty<FBoolProperty>(Btn->GetClass(), TEXT("IsFocusable")))
					{
						P2->SetPropertyValue_InContainer(Btn, bFocus);
						Applied.Add(TEXT("is_focusable"));
					}
					else
					{
						Skipped.Add(TEXT("is_focusable (property not found)"));
					}
				}
			}
			else if (UImage* Img = Cast<UImage>(W))
			{
				FSlateBrush NewBrush = Img->GetBrush();
				bool bBrushDirty = false;

				FString TexPath;
				if (S->TryGetStringField(TEXT("brush_texture"), TexPath))
				{
					if (UTexture2D* Tex = NGGWidgetPriv::LoadTextureAsset(TexPath))
					{
						NewBrush.SetResourceObject(Tex);
						NewBrush.ImageSize = FVector2D(Tex->GetSizeX(), Tex->GetSizeY());
						bBrushDirty = true;
						Applied.Add(TEXT("brush_texture"));
					}
					else
					{
						Skipped.Add(TEXT("brush_texture (texture load failed)"));
					}
				}
				FString DrawAs;
				if (S->TryGetStringField(TEXT("brush_draw_as"), DrawAs))
				{
					NewBrush.DrawAs = NGGWidgetPriv::ParseBrushDrawAs(DrawAs);
					bBrushDirty = true;
					Applied.Add(TEXT("brush_draw_as"));
				}
				const TSharedPtr<FJsonObject>* MarginObj;
				if (S->TryGetObjectField(TEXT("brush_margin"), MarginObj))
				{
					NewBrush.Margin = NGGWidgetPriv::ReadMargin(*MarginObj);
					bBrushDirty = true;
					Applied.Add(TEXT("brush_margin"));
				}
				FString Tiling;
				if (S->TryGetStringField(TEXT("brush_tiling"), Tiling))
				{
					if      (Tiling == TEXT("Horizontal")) NewBrush.Tiling = ESlateBrushTileType::Horizontal;
					else if (Tiling == TEXT("Vertical"))   NewBrush.Tiling = ESlateBrushTileType::Vertical;
					else if (Tiling == TEXT("Both"))       NewBrush.Tiling = ESlateBrushTileType::Both;
					else                                   NewBrush.Tiling = ESlateBrushTileType::NoTile;
					bBrushDirty = true;
					Applied.Add(TEXT("brush_tiling"));
				}
				double BSX = 0, BSY = 0;
				const bool bBSX = S->TryGetNumberField(TEXT("brush_size_x"), BSX);
				const bool bBSY = S->TryGetNumberField(TEXT("brush_size_y"), BSY);
				if (bBSX || bBSY)
				{
					if (bBSX) NewBrush.ImageSize.X = (float)BSX;
					if (bBSY) NewBrush.ImageSize.Y = (float)BSY;
					bBrushDirty = true;
					if (bBSX) Applied.Add(TEXT("brush_size_x"));
					if (bBSY) Applied.Add(TEXT("brush_size_y"));
				}
				if (bBrushDirty) Img->SetBrush(NewBrush);

				FString TintHex;
				if (S->TryGetStringField(TEXT("brush_tint"), TintHex))
				{
					Img->SetColorAndOpacity(NGGWidgetPriv::ParseHexColor(TintHex));
					Applied.Add(TEXT("brush_tint"));
				}
			}
			else if (UBorder* Brd = Cast<UBorder>(W))
			{
				FString TexPath;
				if (S->TryGetStringField(TEXT("brush_texture"), TexPath))
				{
					if (UTexture2D* Tex = NGGWidgetPriv::LoadTextureAsset(TexPath))
					{
						Brd->SetBrushFromTexture(Tex);
						Applied.Add(TEXT("brush_texture"));
					}
					else
					{
						Skipped.Add(TEXT("brush_texture (texture load failed)"));
					}
				}
				FString TintHex;
				if (S->TryGetStringField(TEXT("brush_tint"), TintHex))
				{
					Brd->SetBrushColor(NGGWidgetPriv::ParseHexColor(TintHex));
					Applied.Add(TEXT("brush_tint"));
				}
				FString ContentHex;
				if (S->TryGetStringField(TEXT("content_color"), ContentHex))
				{
					Brd->SetContentColorAndOpacity(NGGWidgetPriv::ParseHexColor(ContentHex));
					Applied.Add(TEXT("content_color"));
				}
				const TSharedPtr<FJsonObject>* CPObj;
				if (S->TryGetObjectField(TEXT("content_padding"), CPObj))
				{
					Brd->SetPadding(NGGWidgetPriv::ReadMargin(*CPObj));
					Applied.Add(TEXT("content_padding"));
				}
				FString HA;
				if (S->TryGetStringField(TEXT("horizontal_alignment"), HA))
				{
					Brd->SetHorizontalAlignment(NGGWidgetPriv::ParseHAlign(HA));
					Applied.Add(TEXT("horizontal_alignment"));
				}
				FString VA;
				if (S->TryGetStringField(TEXT("vertical_alignment"), VA))
				{
					Brd->SetVerticalAlignment(NGGWidgetPriv::ParseVAlign(VA));
					Applied.Add(TEXT("vertical_alignment"));
				}
				FString DrawAs;
				if (S->TryGetStringField(TEXT("brush_draw_as"), DrawAs))
				{
					if (FStructProperty* P = FindFProperty<FStructProperty>(Brd->GetClass(), TEXT("Background")))
					{
						if (FSlateBrush* BPtr = P->ContainerPtrToValuePtr<FSlateBrush>(Brd))
						{
							BPtr->DrawAs = NGGWidgetPriv::ParseBrushDrawAs(DrawAs);
							Applied.Add(TEXT("brush_draw_as"));
						}
					}
				}
			}
			else if (UCheckBox* CB = Cast<UCheckBox>(W))
			{
				bool bChecked = false;
				if (S->TryGetBoolField(TEXT("is_checked"), bChecked))
				{
					CB->SetIsChecked(bChecked);
					Applied.Add(TEXT("is_checked"));
				}
				if (S->HasField(TEXT("check_type")))
				{
					Applied.Add(TEXT("check_type"));
				}
			}
			else if (UProgressBar* PB = Cast<UProgressBar>(W))
			{
				FString FillHex;
				if (!S->TryGetStringField(TEXT("fill_color"), FillHex))
				{
					S->TryGetStringField(TEXT("color"), FillHex);
				}
				if (!FillHex.IsEmpty())
				{
					PB->SetFillColorAndOpacity(NGGWidgetPriv::ParseHexColor(FillHex));
					Applied.Add(TEXT("fill_color"));
					if (S->HasField(TEXT("color"))) Applied.Add(TEXT("color"));
				}
				double Pct = 0;
				if (S->TryGetNumberField(TEXT("percent"), Pct))
				{
					PB->SetPercent((float)Pct);
					Applied.Add(TEXT("percent"));
				}
				FString FillType;
				if (S->TryGetStringField(TEXT("bar_fill_type"), FillType))
				{
					if (FByteProperty* P = FindFProperty<FByteProperty>(PB->GetClass(), TEXT("BarFillType")))
					{
						P->SetPropertyValue_InContainer(PB, (uint8)NGGWidgetPriv::ParseBarFillType(FillType));
						Applied.Add(TEXT("bar_fill_type"));
					}
					else if (FEnumProperty* EP = FindFProperty<FEnumProperty>(PB->GetClass(), TEXT("BarFillType")))
					{
						uint8 V = (uint8)NGGWidgetPriv::ParseBarFillType(FillType);
						EP->GetUnderlyingProperty()->SetIntPropertyValue(
							EP->ContainerPtrToValuePtr<void>(PB), (int64)V);
						Applied.Add(TEXT("bar_fill_type"));
					}
					else
					{
						Skipped.Add(TEXT("bar_fill_type (property not found)"));
					}
				}
				bool bMarquee = false;
				if (S->TryGetBoolField(TEXT("marquee"), bMarquee))
				{
					if (FBoolProperty* P = FindFProperty<FBoolProperty>(PB->GetClass(), TEXT("bIsMarquee")))
					{
						P->SetPropertyValue_InContainer(PB, bMarquee);
						Applied.Add(TEXT("marquee"));
					}
					else
					{
						Skipped.Add(TEXT("marquee (property not found)"));
					}
				}
			}
			else if (USlider* Sl = Cast<USlider>(W))
			{
				double Val = 0;
				if (S->TryGetNumberField(TEXT("value"), Val))
				{
					Sl->SetValue((float)Val);
					Applied.Add(TEXT("value"));
				}
				double MinV = 0;
				if (S->TryGetNumberField(TEXT("min_value"), MinV))
				{
					Sl->SetMinValue((float)MinV);
					Applied.Add(TEXT("min_value"));
				}
				double MaxV = 0;
				if (S->TryGetNumberField(TEXT("max_value"), MaxV))
				{
					Sl->SetMaxValue((float)MaxV);
					Applied.Add(TEXT("max_value"));
				}
				double Step = 0;
				if (S->TryGetNumberField(TEXT("step_size"), Step))
				{
					Sl->SetStepSize((float)Step);
					Applied.Add(TEXT("step_size"));
				}
				FString Ori;
				if (S->TryGetStringField(TEXT("orientation"), Ori))
				{
					Sl->SetOrientation(NGGWidgetPriv::ParseOrientation(Ori));
					Applied.Add(TEXT("orientation"));
				}
				FString BarHex;
				if (S->TryGetStringField(TEXT("slider_bar_color"), BarHex))
				{
					Sl->SetSliderBarColor(NGGWidgetPriv::ParseHexColor(BarHex));
					Applied.Add(TEXT("slider_bar_color"));
				}
				FString HandleHex;
				if (S->TryGetStringField(TEXT("slider_handle_color"), HandleHex))
				{
					Sl->SetSliderHandleColor(NGGWidgetPriv::ParseHexColor(HandleHex));
					Applied.Add(TEXT("slider_handle_color"));
				}
			}
			else if (UScrollBox* SB = Cast<UScrollBox>(W))
			{
				FString Ori;
				if (S->TryGetStringField(TEXT("orientation"), Ori))
				{
					SB->SetOrientation(NGGWidgetPriv::ParseOrientation(Ori));
					Applied.Add(TEXT("orientation"));
				}
				FString VisStr2;
				if (S->TryGetStringField(TEXT("scroll_bar_visibility"), VisStr2))
				{
					SB->SetScrollBarVisibility(NGGWidgetPriv::ParseVisibility(VisStr2));
					Applied.Add(TEXT("scroll_bar_visibility"));
				}
				bool bOver = false;
				if (S->TryGetBoolField(TEXT("allow_overscroll"), bOver))
				{
					SB->SetAllowOverscroll(bOver);
					Applied.Add(TEXT("allow_overscroll"));
				}
				double Thick = 0;
				if (S->TryGetNumberField(TEXT("scroll_bar_thickness"), Thick))
				{
					SB->SetScrollbarThickness(FVector2D((float)Thick, (float)Thick));
					Applied.Add(TEXT("scroll_bar_thickness"));
				}
				bool bAlways = false;
				if (S->TryGetBoolField(TEXT("always_show_scrollbar"), bAlways))
				{
					SB->SetAlwaysShowScrollbar(bAlways);
					Applied.Add(TEXT("always_show_scrollbar"));
				}
			}
			else if (USizeBox* SZB = Cast<USizeBox>(W))
			{
				double V = 0;
				if (S->TryGetNumberField(TEXT("width_override"),  V)) { SZB->SetWidthOverride((float)V);     Applied.Add(TEXT("width_override")); }
				if (S->TryGetNumberField(TEXT("height_override"), V)) { SZB->SetHeightOverride((float)V);    Applied.Add(TEXT("height_override")); }
				if (S->TryGetNumberField(TEXT("min_width"),       V)) { SZB->SetMinDesiredWidth((float)V);   Applied.Add(TEXT("min_width")); }
				if (S->TryGetNumberField(TEXT("min_height"),      V)) { SZB->SetMinDesiredHeight((float)V);  Applied.Add(TEXT("min_height")); }
				if (S->TryGetNumberField(TEXT("max_width"),       V)) { SZB->SetMaxDesiredWidth((float)V);   Applied.Add(TEXT("max_width")); }
				if (S->TryGetNumberField(TEXT("max_height"),      V)) { SZB->SetMaxDesiredHeight((float)V);  Applied.Add(TEXT("max_height")); }
				if (S->TryGetNumberField(TEXT("min_aspect_ratio"),V)) { SZB->SetMinAspectRatio((float)V);    Applied.Add(TEXT("min_aspect_ratio")); }
				if (S->TryGetNumberField(TEXT("max_aspect_ratio"),V)) { SZB->SetMaxAspectRatio((float)V);    Applied.Add(TEXT("max_aspect_ratio")); }
				bool bClr = false;
				if (S->TryGetBoolField(TEXT("clear_width_override"),  bClr) && bClr) { SZB->ClearWidthOverride();  Applied.Add(TEXT("clear_width_override")); }
				if (S->TryGetBoolField(TEXT("clear_height_override"), bClr) && bClr) { SZB->ClearHeightOverride(); Applied.Add(TEXT("clear_height_override")); }
			}
			else if (USpacer* Sp = Cast<USpacer>(W))
			{
				double SX = 0, SY = 0;
				const bool bSX = S->TryGetNumberField(TEXT("size_x"), SX);
				const bool bSY = S->TryGetNumberField(TEXT("size_y"), SY);
				if (bSX || bSY)
				{
					FVector2D Cur(0, 0);
					if (FStructProperty* P = FindFProperty<FStructProperty>(Sp->GetClass(), TEXT("Size")))
					{
						if (FVector2D* V = P->ContainerPtrToValuePtr<FVector2D>(Sp))
						{
							Cur = *V;
						}
					}
					if (bSX) Cur.X = (float)SX;
					if (bSY) Cur.Y = (float)SY;
					Sp->SetSize(Cur);
					if (bSX) Applied.Add(TEXT("size_x"));
					if (bSY) Applied.Add(TEXT("size_y"));
				}
			}
			else if (UWidgetSwitcher* WS2 = Cast<UWidgetSwitcher>(W))
			{
				int32 Idx = 0;
				if (S->TryGetNumberField(TEXT("active_widget_index"), Idx))
				{
					WS2->SetActiveWidgetIndex(Idx);
					Applied.Add(TEXT("active_widget_index"));
				}
			}
			else if (UCircularThrobber* CT = Cast<UCircularThrobber>(W))
			{
				int32 N = 0;
				if (S->TryGetNumberField(TEXT("number_of_pieces"), N))
				{
					CT->SetNumberOfPieces(N);
					Applied.Add(TEXT("number_of_pieces"));
				}
				double Period = 0, Radius = 0;
				if (S->TryGetNumberField(TEXT("period"), Period)) { CT->SetPeriod((float)Period); Applied.Add(TEXT("period")); }
				if (S->TryGetNumberField(TEXT("radius"), Radius)) { CT->SetRadius((float)Radius); Applied.Add(TEXT("radius")); }
			}
			else if (UThrobber* Th = Cast<UThrobber>(W))
			{
				int32 N = 0;
				if (S->TryGetNumberField(TEXT("number_of_pieces"), N))
				{
					Th->SetNumberOfPieces(N);
					Applied.Add(TEXT("number_of_pieces"));
				}
				// Throbber animate flags are not exposed as setters in all UE5 versions;
				// set them via reflection so the build doesn't depend on which setters exist.
				auto SetThrobberBool = [&](const FString& Key, const TCHAR* PropName)
				{
					bool bVal = false;
					if (!S->TryGetBoolField(Key, bVal)) return;
					if (FBoolProperty* P = FindFProperty<FBoolProperty>(Th->GetClass(), PropName))
					{
						P->SetPropertyValue_InContainer(Th, bVal);
						Applied.Add(Key);
					}
				};
				SetThrobberBool(TEXT("animate_horizontally"), TEXT("bAnimateHorizontally"));
				SetThrobberBool(TEXT("animate_vertically"),   TEXT("bAnimateVertically"));
				SetThrobberBool(TEXT("animate_opacity"),      TEXT("bAnimateOpacity"));
			}
			else if (UBackgroundBlur* BB = Cast<UBackgroundBlur>(W))
			{
				double BS = 0;
				if (S->TryGetNumberField(TEXT("blur_strength"), BS))
				{
					BB->SetBlurStrength((float)BS);
					Applied.Add(TEXT("blur_strength"));
				}
				int32 BR = 0;
				if (S->TryGetNumberField(TEXT("blur_radius"), BR))
				{
					BB->SetBlurRadius(BR);
					Applied.Add(TEXT("blur_radius"));
				}
				const TSharedPtr<FJsonObject>* PObj;
				if (S->TryGetObjectField(TEXT("padding"), PObj))
				{
					BB->SetPadding(NGGWidgetPriv::ReadMargin(*PObj));
					Applied.Add(TEXT("padding"));
				}
				else if (S->TryGetObjectField(TEXT("content_padding"), PObj))
				{
					BB->SetPadding(NGGWidgetPriv::ReadMargin(*PObj));
					Applied.Add(TEXT("content_padding"));
				}
			}
			else if (USafeZone* SZ = Cast<USafeZone>(W))
			{
				PRAGMA_DISABLE_DEPRECATION_WARNINGS
				bool bL = SZ->PadLeft, bT = SZ->PadTop, bR = SZ->PadRight, bB = SZ->PadBottom;
				PRAGMA_ENABLE_DEPRECATION_WARNINGS
				bool bChanged = false;
				const TSharedPtr<FJsonObject>* SidesObj;
				if (S->TryGetObjectField(TEXT("pad_sides"), SidesObj))
				{
					(*SidesObj)->TryGetBoolField(TEXT("left"),   bL);
					(*SidesObj)->TryGetBoolField(TEXT("top"),    bT);
					(*SidesObj)->TryGetBoolField(TEXT("right"),  bR);
					(*SidesObj)->TryGetBoolField(TEXT("bottom"), bB);
					bChanged = true;
					Applied.Add(TEXT("pad_sides"));
				}
				bool bTmp = false;
				if (S->TryGetBoolField(TEXT("pad_left"),   bTmp)) { bL = bTmp; bChanged = true; Applied.Add(TEXT("pad_left")); }
				if (S->TryGetBoolField(TEXT("pad_top"),    bTmp)) { bT = bTmp; bChanged = true; Applied.Add(TEXT("pad_top")); }
				if (S->TryGetBoolField(TEXT("pad_right"),  bTmp)) { bR = bTmp; bChanged = true; Applied.Add(TEXT("pad_right")); }
				if (S->TryGetBoolField(TEXT("pad_bottom"), bTmp)) { bB = bTmp; bChanged = true; Applied.Add(TEXT("pad_bottom")); }
				if (bChanged) SZ->SetSidesToPad(bL, bR, bT, bB);
				if (S->HasField(TEXT("is_titlesafe")))
				{
					Skipped.Add(TEXT("is_titlesafe (USafeZone::IsTitleSafe removed in UE5.7 — use r.DebugSafeZone.TitleRatio cvar)"));
				}
			}
			else if (UScaleBox* SCB = Cast<UScaleBox>(W))
			{
				FString St;
				if (S->TryGetStringField(TEXT("stretch"), St))
				{
					EStretch::Type ST = EStretch::None;
					if      (St == TEXT("Fill"))           ST = EStretch::Fill;
					else if (St == TEXT("ScaleToFit"))     ST = EStretch::ScaleToFit;
					else if (St == TEXT("ScaleToFitX"))    ST = EStretch::ScaleToFitX;
					else if (St == TEXT("ScaleToFitY"))    ST = EStretch::ScaleToFitY;
					else if (St == TEXT("ScaleToFill"))    ST = EStretch::ScaleToFill;
					else if (St == TEXT("ScaleBySafeZone"))ST = EStretch::ScaleBySafeZone;
					else if (St == TEXT("UserSpecified"))  ST = EStretch::UserSpecified;
					SCB->SetStretch(ST);
					Applied.Add(TEXT("stretch"));
				}
				FString SD;
				if (S->TryGetStringField(TEXT("stretch_direction"), SD))
				{
					EStretchDirection::Type D = EStretchDirection::Both;
					if      (SD == TEXT("DownOnly")) D = EStretchDirection::DownOnly;
					else if (SD == TEXT("UpOnly"))   D = EStretchDirection::UpOnly;
					SCB->SetStretchDirection(D);
					Applied.Add(TEXT("stretch_direction"));
				}
				double USS = 0;
				if (S->TryGetNumberField(TEXT("user_specified_scale"), USS))
				{
					SCB->SetUserSpecifiedScale((float)USS);
					Applied.Add(TEXT("user_specified_scale"));
				}
			}
			else if (UComboBoxString* CBox = Cast<UComboBoxString>(W))
			{
				bool bClear = false;
				if (S->TryGetBoolField(TEXT("clear_options"), bClear) && bClear)
				{
					CBox->ClearOptions();
					Applied.Add(TEXT("clear_options"));
				}
				FString OneOption;
				if (S->TryGetStringField(TEXT("add_option"), OneOption))
				{
					CBox->AddOption(OneOption);
					Applied.Add(TEXT("add_option"));
				}
				else
				{
					const TArray<TSharedPtr<FJsonValue>>* OptArr;
					if (S->TryGetArrayField(TEXT("add_option"), OptArr))
					{
						for (const TSharedPtr<FJsonValue>& Opt : *OptArr)
						{
							FString OptStr;
							if (Opt->TryGetString(OptStr))
							{
								CBox->AddOption(OptStr);
							}
						}
						Applied.Add(TEXT("add_option"));
					}
				}
				FString Sel;
				if (S->TryGetStringField(TEXT("selected_option"), Sel))
				{
					CBox->SetSelectedOption(Sel);
					Applied.Add(TEXT("selected_option"));
				}
			}
			else if (UEditableTextBox* ETB = Cast<UEditableTextBox>(W))
			{
				double FontSizeVal = 0;
				if (S->TryGetNumberField(TEXT("font_size"), FontSizeVal))
				{
					FEditableTextBoxStyle Style = ETB->WidgetStyle;
					Style.TextStyle.Font.Size = (int32)FontSizeVal;
					ETB->WidgetStyle = Style;
					Applied.Add(TEXT("font_size"));
				}
				FString TextStr;
				if (S->TryGetStringField(TEXT("text"), TextStr))
				{
					ETB->SetText(FText::FromString(TextStr));
					Applied.Add(TEXT("text"));
				}
			}
			else
			{
				static const TArray<FString> TypeSpecificKeys = {
					TEXT("text"), TEXT("font_size"), TEXT("font_bold"), TEXT("font_typeface"),
					TEXT("color"), TEXT("fill_color"), TEXT("background_color"),
					TEXT("hovered_color"), TEXT("pressed_color"), TEXT("disabled_color"),
					TEXT("auto_wrap"), TEXT("horizontal_alignment"), TEXT("percent"),
					TEXT("brush_texture"), TEXT("brush_tint"), TEXT("brush_size_x"),
					TEXT("brush_size_y"), TEXT("brush_draw_as"), TEXT("brush_margin"),
					TEXT("brush_tiling"), TEXT("value"), TEXT("min_value"), TEXT("max_value"),
					TEXT("step_size"), TEXT("orientation"), TEXT("is_checked"),
					TEXT("active_widget_index"), TEXT("number_of_pieces"), TEXT("blur_strength"),
					TEXT("stretch"), TEXT("add_option")
				};
				for (const FString& K : TypeSpecificKeys)
				{
					if (S->HasField(K)) AddUnsupportedWidget(K);
				}
			}

			for (const FString& K : AllKnownKeys)
			{
				if (S->HasField(K) && !Applied.Contains(K))
				{
					const bool bAlreadySkipped = Skipped.ContainsByPredicate(
						[&K](const FString& Elem){ return Elem.StartsWith(K); });
					if (!bAlreadySkipped)
					{
						Skipped.Add(K);
					}
				}
			}

			TotalApplied += Applied.Num();
			TotalSkipped += Skipped.Num();

			TArray<TSharedPtr<FJsonValue>> AppliedJson, SkippedJson;
			for (const FString& A  : Applied)  AppliedJson.Add(MakeShared<FJsonValueString>(A));
			for (const FString& Sk : Skipped)  SkippedJson.Add(MakeShared<FJsonValueString>(Sk));

			WidgetResult->SetStringField(TEXT("status"),       TEXT("ok"));
			WidgetResult->SetStringField(TEXT("widget_class"), W->GetClass()->GetName());
			WidgetResult->SetArrayField (TEXT("applied"),      AppliedJson);
			WidgetResult->SetArrayField (TEXT("skipped"),      SkippedJson);
			ResultsJson.Add(MakeShared<FJsonValueObject>(WidgetResult));
		}

		// ---------- 5. Mark blueprint modified + dirty -----------------------
		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		WBP->MarkPackageDirty();

		// ---------- 6. Build response ----------------------------------------
		TSharedPtr<FJsonObject> Resp2 = MakeShared<FJsonObject>();
		Resp2->SetBoolField  (TEXT("success"),         true);
		Resp2->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Resp2->SetNumberField(TEXT("total_applied"),    static_cast<double>(TotalApplied));
		Resp2->SetNumberField(TEXT("total_skipped"),    static_cast<double>(TotalSkipped));
		Resp2->SetArrayField (TEXT("results"),          ResultsJson);

		FString RespBody2;
		TSharedRef<TJsonWriter<>> Writer2 = TJsonWriterFactory<>::Create(&RespBody2);
		FJsonSerializer::Serialize(Resp2.ToSharedRef(), Writer2);
		Callback(JsonOk(RespBody2));
	});

	return true;
}


// ============================================================================
// Handler: POST /editor/remove_widget_from_blueprint
// Body: { "widget_blueprint": "/Game/...", "widget_name": "MyButton", "cascade": true }
// ============================================================================
bool FNGGHttpServer::HandleRemoveWidgetFromBlueprint(
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

	FString WBPPath, WidgetName;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	Body->TryGetStringField(TEXT("widget_name"),      WidgetName);
	bool bCascade = true;
	Body->TryGetBoolField(TEXT("cascade"), bCascade);

	if (WBPPath.IsEmpty() || WidgetName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint and widget_name are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleRemoveWidgetFromBlueprint: '%s' name='%s' cascade=%s"),
		*WBPPath, *WidgetName, bCascade ? TEXT("true") : TEXT("false"));

	AsyncTask(ENamedThreads::GameThread, [WBPPath, WidgetName, bCascade, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		UWidget* Target = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		if (!IsValid(Target))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget '%s' not found"), *WidgetName)));
			return;
		}

		if (WBP->WidgetTree->RootWidget == Target)
		{
			Callback(JsonError(400, TEXT("cannot remove root widget")));
			return;
		}

		UPanelWidget* Panel = Cast<UPanelWidget>(Target);
		if (!bCascade && Panel && Panel->GetChildrenCount() > 0)
		{
			Callback(JsonError(400, TEXT("widget has children — pass cascade:true")));
			return;
		}

		TArray<FString> Removed;
		TArray<UWidget*> ToRemove;
		TFunction<void(UWidget*)> Walk;
		Walk = [&Walk, &Removed, &ToRemove](UWidget* W)
		{
			if (!W) return;
			Removed.Add(W->GetFName().ToString());
			ToRemove.Add(W);
			if (UPanelWidget* P = Cast<UPanelWidget>(W))
			{
				for (int32 i = 0; i < P->GetChildrenCount(); ++i)
				{
					Walk(P->GetChildAt(i));
				}
			}
		};
		Walk(Target);

		// Detach the target from its parent panel first so removal is clean.
		if (UPanelWidget* CurParent = Target->GetParent())
		{
			CurParent->RemoveChild(Target);
		}
		// Remove all descendants (and the target) from the WidgetTree, then
		// move them out from under the WBP outer chain so the widget BP
		// compiler's ForEachSourceWidget (which uses ForEachObjectWithOuter)
		// no longer enumerates them.  Without this step a later
		// compile_widget_blueprint call can crash inside
		// ValidateAndFixUpVariableGuids while iterating still-parented dead
		// widgets whose GUID entry we just removed.
		UPackage* TransientPkg = GetTransientPackage();
		for (UWidget* W2 : ToRemove)
		{
			if (!IsValid(W2)) continue;
			WBP->WidgetTree->RemoveWidget(W2);
			W2->Rename(
				nullptr,
				TransientPkg,
				REN_DontCreateRedirectors | REN_DoNotDirty | REN_ForceNoResetLoaders | REN_NonTransactional);
			W2->MarkAsGarbage();
		}

		for (const FString& Name : Removed)
		{
			WBP->WidgetVariableNameToGuidMap.Remove(FName(*Name));
		}

		NGGWidgetPriv::MarkWBPDirtyAndModified(WBP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		TArray<TSharedPtr<FJsonValue>> RemovedJson;
		for (const FString& N : Removed)
		{
			RemovedJson.Add(MakeShared<FJsonValueString>(N));
		}
		Result->SetArrayField(TEXT("removed"), RemovedJson);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/reparent_widget
// Body: { "widget_blueprint", "widget_name", "new_parent", "child_index"? }
// ============================================================================
bool FNGGHttpServer::HandleReparentWidget(
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

	FString WBPPath, WidgetName, NewParent;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	Body->TryGetStringField(TEXT("widget_name"),      WidgetName);
	Body->TryGetStringField(TEXT("new_parent"),       NewParent);
	int32 ChildIndex = INDEX_NONE;
	const bool bHasIndex = Body->TryGetNumberField(TEXT("child_index"), ChildIndex);

	if (WBPPath.IsEmpty() || WidgetName.IsEmpty() || NewParent.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint, widget_name, new_parent are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleReparentWidget: '%s' '%s' -> '%s' idx=%d"),
		*WBPPath, *WidgetName, *NewParent, ChildIndex);

	AsyncTask(ENamedThreads::GameThread, [WBPPath, WidgetName, NewParent, ChildIndex, bHasIndex, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		UWidget* P = WBP->WidgetTree->FindWidget(FName(*NewParent));
		if (!IsValid(W))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget '%s' not found"), *WidgetName)));
			return;
		}
		if (!IsValid(P))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Parent widget '%s' not found"), *NewParent)));
			return;
		}
		if (WBP->WidgetTree->RootWidget == W)
		{
			Callback(JsonError(400, TEXT("cannot reparent root widget")));
			return;
		}
		UPanelWidget* NewParentPanel = Cast<UPanelWidget>(P);
		if (!NewParentPanel)
		{
			Callback(JsonError(400, FString::Printf(TEXT("new_parent '%s' is not a panel"), *NewParent)));
			return;
		}
		if (W == P)
		{
			Callback(JsonError(400, TEXT("cannot reparent widget under itself")));
			return;
		}

		// Reject if P is a descendant of W (would create a cycle).
		UWidget* Cursor = P;
		while (Cursor)
		{
			if (Cursor == W)
			{
				Callback(JsonError(400, TEXT("cannot reparent under a descendant")));
				return;
			}
			Cursor = Cursor->GetParent();
		}

		if (!NewParentPanel->CanAddMoreChildren())
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("new_parent '%s' (%s) cannot accept more children — single-child container already full"),
				*NewParent, *NewParentPanel->GetClass()->GetName())));
			return;
		}

		UPanelWidget* OldParent  = W->GetParent();
		const int32   OldIndex   = OldParent ? OldParent->GetChildIndex(W) : INDEX_NONE;

		if (OldParent)
		{
			OldParent->RemoveChild(W);
		}

		UPanelSlot* NewSlot = NewParentPanel->AddChild(W);
		if (!NewSlot)
		{
			if (OldParent)
			{
				if (UPanelSlot* RestoredSlot = OldParent->AddChild(W))
				{
					if (OldIndex >= 0 && OldIndex < OldParent->GetChildrenCount())
					{
						OldParent->ShiftChild(OldIndex, W);
					}
				}
			}
			Callback(JsonError(400, FString::Printf(
				TEXT("new_parent '%s' (%s) refused the widget — likely a single-child container that is already full"),
				*NewParent, *NewParentPanel->GetClass()->GetName())));
			return;
		}

		if (bHasIndex && ChildIndex >= 0 && ChildIndex < NewParentPanel->GetChildrenCount())
		{
			NewParentPanel->ShiftChild(ChildIndex, W);
		}

		NGGWidgetPriv::EnsureGuid(WBP, W);
		NGGWidgetPriv::MarkWBPDirtyAndModified(WBP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetStringField(TEXT("widget_name"),      WidgetName);
		Result->SetStringField(TEXT("new_parent"),       NewParent);
		Result->SetStringField(TEXT("new_slot_class"),   W->Slot ? W->Slot->GetClass()->GetName() : TEXT(""));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/rename_widget
// Body: { "widget_blueprint", "old_name", "new_name" }
// ============================================================================
bool FNGGHttpServer::HandleRenameWidget(
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

	FString WBPPath, OldName, NewName;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	Body->TryGetStringField(TEXT("old_name"),         OldName);
	Body->TryGetStringField(TEXT("new_name"),         NewName);

	if (WBPPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint, old_name, new_name are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleRenameWidget: '%s' '%s' -> '%s'"), *WBPPath, *OldName, *NewName);

	AsyncTask(ENamedThreads::GameThread, [WBPPath, OldName, NewName, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP) || !IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		UWidget* W = WBP->WidgetTree->FindWidget(FName(*OldName));
		if (!IsValid(W))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget '%s' not found"), *OldName)));
			return;
		}

		if (UWidget* Existing = WBP->WidgetTree->FindWidget(FName(*NewName)))
		{
			if (Existing != W)
			{
				Callback(JsonError(400, FString::Printf(TEXT("Widget '%s' already exists"), *NewName)));
				return;
			}
		}

		const FName OldFName = W->GetFName();
		W->Rename(*NewName, W->GetOuter(), REN_DontCreateRedirectors);

		FGuid OldGuid;
		if (FGuid* Found = WBP->WidgetVariableNameToGuidMap.Find(OldFName))
		{
			OldGuid = *Found;
		}
		else
		{
			OldGuid = FGuid::NewGuid();
		}
		WBP->WidgetVariableNameToGuidMap.Remove(OldFName);
		WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), OldGuid);

		NGGWidgetPriv::MarkWBPDirtyAndModified(WBP);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetStringField(TEXT("old_name"),         OldName);
		Result->SetStringField(TEXT("new_name"),         W->GetFName().ToString());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/compile_widget_blueprint
// Body: { "widget_blueprint" }
// ============================================================================
bool FNGGHttpServer::HandleCompileWidgetBlueprint(
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

	FString WBPPath;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleCompileWidgetBlueprint: '%s'"), *WBPPath);

	AsyncTask(ENamedThreads::GameThread, [WBPPath, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}

		// Defensive pre-compile: walk the live widget tree and reconcile
		// WidgetVariableNameToGuidMap so it has exactly one entry per live
		// widget.  Without this a previous rename/remove path that left the
		// map drifted (orphan entries or missing entries) can crash the
		// widget compiler in ValidateAndFixUpVariableGuids.
		if (IsValid(WBP->WidgetTree))
		{
			TSet<FName> LiveNames;
			WBP->WidgetTree->ForEachWidget([&LiveNames](UWidget* W)
			{
				if (IsValid(W))
				{
					LiveNames.Add(W->GetFName());
				}
			});

			// Prune orphan entries (FNames not present in the tree any more).
			TArray<FName> Orphans;
			for (const TPair<FName, FGuid>& Pair : WBP->WidgetVariableNameToGuidMap)
			{
				if (!LiveNames.Contains(Pair.Key))
				{
					Orphans.Add(Pair.Key);
				}
			}
			for (const FName& N : Orphans)
			{
				WBP->WidgetVariableNameToGuidMap.Remove(N);
			}

			// Ensure every live widget has a GUID entry.
			WBP->WidgetTree->ForEachWidget([WBP](UWidget* W)
			{
				if (IsValid(W) && !WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
				{
					WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
				}
			});
		}

		FKismetEditorUtilities::CompileBlueprint(WBP);

		FString StatusStr;
		switch (WBP->Status)
		{
		case BS_UpToDate:                  StatusStr = TEXT("UpToDate");                break;
		case BS_UpToDateWithWarnings:      StatusStr = TEXT("UpToDateWithWarnings");    break;
		case BS_Error:                     StatusStr = TEXT("Error");                   break;
		case BS_Dirty:                     StatusStr = TEXT("Dirty");                   break;
		case BS_Unknown:                   StatusStr = TEXT("Unknown");                 break;
		default:                           StatusStr = TEXT("Unknown");                 break;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          WBP->Status != BS_Error);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetStringField(TEXT("status"),           StatusStr);
		Result->SetArrayField (TEXT("warnings"),         TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField (TEXT("errors"),           TArray<TSharedPtr<FJsonValue>>());

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}


// ============================================================================
// Handler: POST /editor/create_material
// Body: { "asset_path": "/Game/Materials/M_Green", "base_color": {"r":0,"g":1,"b":0}, "actor_label": "Cube15" }
// ============================================================================

bool FNGGHttpServer::HandleCreateMaterial(
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

	// Base color (linear space). Defaults: warm wooden-crate brown.
	float R = 0.35f, G = 0.12f, B = 0.03f;
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Body->TryGetObjectField(TEXT("base_color"), ColorObj))
		{
			double Rd, Gd, Bd;
			if ((*ColorObj)->TryGetNumberField(TEXT("r"), Rd)) R = (float)Rd;
			if ((*ColorObj)->TryGetNumberField(TEXT("g"), Gd)) G = (float)Gd;
			if ((*ColorObj)->TryGetNumberField(TEXT("b"), Bd)) B = (float)Bd;
		}
	}

	// pixel_art:true  → adds UV-border HLSL so each face shows a dark outline
	bool bPixelArt = false;
	Body->TryGetBoolField(TEXT("pixel_art"), bPixelArt);

	// use_parameters:true → creates VectorParameter("BaseColor") and
	// ScalarParameter("BrightnessMultiplier") with graph: (BaseColor * Brightness) → BaseColor pin
	bool bUseParameters = false;
	Body->TryGetBoolField(TEXT("use_parameters"), bUseParameters);

	// texture_path: optional content path to a texture (e.g. "/Game/Textures/T_PixelCrate").
	// When set, creates a TextureSample node multiplied by BaseColor VectorParameter
	// so hover tinting still works. Implies use_parameters=true.
	FString TexturePath;
	Body->TryGetStringField(TEXT("texture_path"), TexturePath);
	if (!TexturePath.IsEmpty()) bUseParameters = true;

	// unlit:true → route the color to Emissive Color and set Shading Model = Unlit
	// so the material renders the exact authored color, independent of scene
	// lighting (ideal for cables, holograms, UI-like glows, debug viz).
	bool bUnlit = false;
	Body->TryGetBoolField(TEXT("unlit"), bUnlit);

	// Optional scalar defaults
	float BrightnessDefault = 1.0f;
	{
		double Bd;
		if (Body->TryGetNumberField(TEXT("brightness_default"), Bd)) BrightnessDefault = (float)Bd;
	}

	// Optional: also apply the new material to a Blueprint CDO component
	FString BlueprintPath, ComponentName;
	Body->TryGetStringField(TEXT("blueprint_path"),  BlueprintPath);
	Body->TryGetStringField(TEXT("component_name"),  ComponentName);

	FString ActorLabel;
	Body->TryGetStringField(TEXT("actor_label"), ActorLabel);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, R, G, B, bPixelArt, bUseParameters, bUnlit, BrightnessDefault, BlueprintPath, ComponentName, ActorLabel, TexturePath]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		bool bAlreadyExisted = (Mat != nullptr);

		if (!Mat)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
			Mat = Cast<UMaterial>(NewAsset);
		}

		if (!Mat)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create material at '%s'"), *AssetPath)));
			return;
		}

		// Clear all existing expressions so we start fresh each time
		Mat->GetExpressionCollection().Empty();

		if (bUseParameters)
		{
			// ── VectorParameter "BaseColor" ─────────────────────────────
			UMaterialExpressionVectorParameter* ColorParam =
				NewObject<UMaterialExpressionVectorParameter>(Mat);
			ColorParam->ParameterName = TEXT("BaseColor");
			ColorParam->DefaultValue  = FLinearColor(R, G, B, 1.f);
			Mat->GetExpressionCollection().AddExpression(ColorParam);

			// ── ScalarParameter "BrightnessMultiplier" ──────────────────
			UMaterialExpressionScalarParameter* BrightParam =
				NewObject<UMaterialExpressionScalarParameter>(Mat);
			BrightParam->ParameterName = TEXT("BrightnessMultiplier");
			BrightParam->DefaultValue  = BrightnessDefault;
			Mat->GetExpressionCollection().AddExpression(BrightParam);

			// ── Optional TextureSample node ─────────────────────────────
			UMaterialExpression* ColorSource = ColorParam; // default: just the parameter
			if (!TexturePath.IsEmpty())
			{
				UTexture* Tex = LoadObject<UTexture>(nullptr, *TexturePath);
				if (Tex)
				{
					UMaterialExpressionTextureSample* TexSample =
						NewObject<UMaterialExpressionTextureSample>(Mat);
					TexSample->Texture = Tex;
					TexSample->SamplerType = SAMPLERTYPE_Color;
					Mat->GetExpressionCollection().AddExpression(TexSample);

					// Multiply texture by BaseColor so hover tinting works
					UMaterialExpressionMultiply* TexColorMul =
						NewObject<UMaterialExpressionMultiply>(Mat);
					TexColorMul->A.Expression = TexSample;
					TexColorMul->B.Expression = ColorParam;
					Mat->GetExpressionCollection().AddExpression(TexColorMul);

					ColorSource = TexColorMul;
				}
			}

			// ── Multiply node: ColorSource * BrightnessMultiplier ───────
			UMaterialExpressionMultiply* MulExpr =
				NewObject<UMaterialExpressionMultiply>(Mat);
			MulExpr->A.Expression = ColorSource;
			MulExpr->B.Expression = BrightParam;
			Mat->GetExpressionCollection().AddExpression(MulExpr);

			// Wire Multiply output → material Base Color pin
			Mat->GetEditorOnlyData()->BaseColor.Expression  = MulExpr;
			Mat->GetEditorOnlyData()->BaseColor.OutputIndex = 0;

			// Roughness = 1 (matte — no specular glare on pixel art boxes)
			UMaterialExpressionConstant* RoughExp =
				NewObject<UMaterialExpressionConstant>(Mat);
			RoughExp->R = 1.0f;
			Mat->GetExpressionCollection().AddExpression(RoughExp);
			Mat->GetEditorOnlyData()->Roughness.Expression = RoughExp;
		}
		else if (bPixelArt)
		{
			// ── TextureCoordinate ───────────────────────────────────────
			UMaterialExpressionTextureCoordinate* UVExp =
				NewObject<UMaterialExpressionTextureCoordinate>(Mat);
			UVExp->UTiling = 1.0f;
			UVExp->VTiling = 1.0f;
			Mat->GetExpressionCollection().AddExpression(UVExp);

			// ── Custom HLSL: pixel-art border on every face ─────────────
			// frac(UV) gives 0→1 per face.  step(border, frac) creates a
			// mask that is 1 inside the face and 0 at the outer edge strip.
			UMaterialExpressionCustom* CustomExp =
				NewObject<UMaterialExpressionCustom>(Mat);
			CustomExp->Code = FString::Printf(TEXT(
				"float2 f = frac(TexCoord);\n"
				"float2 m = step(0.07, f) * step(0.07, 1.0 - f);\n"
				"float mask = m.x * m.y;\n"
				"float3 body = float3(%.5f, %.5f, %.5f);\n"
				"float3 edge = body * 0.12;\n"
				"return lerp(edge, body, mask);\n"
			), R, G, B);
			CustomExp->OutputType = CMOT_Float3;
			CustomExp->Description = TEXT("PixelArtBorder");

			FCustomInput UVInput;
			UVInput.InputName = TEXT("TexCoord");
			UVInput.Input.Expression  = UVExp;
			UVInput.Input.OutputIndex = 0;
			CustomExp->Inputs.Add(UVInput);

			Mat->GetExpressionCollection().AddExpression(CustomExp);
			Mat->GetEditorOnlyData()->BaseColor.Expression  = CustomExp;
			Mat->GetEditorOnlyData()->BaseColor.OutputIndex = 0;

			// Roughness = 1 (matte — no specular glare on pixel art)
			UMaterialExpressionConstant* RoughExp =
				NewObject<UMaterialExpressionConstant>(Mat);
			RoughExp->R = 1.0f;
			Mat->GetExpressionCollection().AddExpression(RoughExp);
			Mat->GetEditorOnlyData()->Roughness.Expression = RoughExp;
		}
		else
		{
			UMaterialExpressionConstant3Vector* ColorExpr =
				NewObject<UMaterialExpressionConstant3Vector>(Mat);
			ColorExpr->Constant = FLinearColor(R, G, B, 1.f);
			Mat->GetExpressionCollection().AddExpression(ColorExpr);
			Mat->GetEditorOnlyData()->BaseColor.Expression = ColorExpr;
		}

		// Unlit: drive Emissive Color with whatever feeds Base Color, then switch
		// the shading model so the color is shown verbatim (no lighting applied).
		if (bUnlit)
		{
			UMaterialEditorOnlyData* EOD = Mat->GetEditorOnlyData();
			EOD->EmissiveColor.Expression  = EOD->BaseColor.Expression;
			EOD->EmissiveColor.OutputIndex = EOD->BaseColor.OutputIndex;
			Mat->SetShadingModel(MSM_Unlit);
		}

		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		// ── Optional: apply to a Blueprint component ────────────────────
		// SCS-first lookup (components added via add_component_to_blueprint live
		// in BP->SimpleConstructionScript and are NOT direct FObjectProperty
		// fields on the CDO). Fall through to the FObjectProperty loop only for
		// inherited / native C++ components.
		bool bAppliedToBlueprint = false;
		if (!BlueprintPath.IsEmpty())
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
			if (BP && BP->GeneratedClass)
			{
				const FString CompName = ComponentName.IsEmpty() ? TEXT("StaticMeshComponent0") : ComponentName;

				// --- Path 1: SCS-owned component ---
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (Node && Node->ComponentTemplate &&
							Node->GetVariableName().ToString() == CompName)
						{
							if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Node->ComponentTemplate))
							{
								SMC->SetMaterial(0, Mat);
								bAppliedToBlueprint = true;
							}
							break;
						}
					}
				}

				// --- Path 2: Inherited / native C++ component on the CDO ---
				if (!bAppliedToBlueprint)
				{
					UObject* CDO = BP->GeneratedClass->GetDefaultObject();
					if (CDO)
					{
						for (TFieldIterator<FObjectProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
						{
							if (PropIt->GetName() == CompName || PropIt->GetFName() == *CompName)
							{
								UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(PropIt->GetObjectPropertyValue_InContainer(CDO));
								if (SMC)
								{
									SMC->SetMaterial(0, Mat);
									bAppliedToBlueprint = true;
								}
								break;
							}
						}
					}
				}

				if (bAppliedToBlueprint)
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
					if (UPackage* BpPkg = BP->GetOutermost())
					{
						BpPkg->MarkPackageDirty();
					}
				}
			}
		}

		// ── Optional: apply to a level actor ────────────────────────────
		bool bAppliedToActor = false;
		if (!ActorLabel.IsEmpty())
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (World)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (IsValid(*It) && It->GetActorLabel() == ActorLabel)
					{
						UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(
							It->FindComponentByClass(UStaticMeshComponent::StaticClass()));
						if (SMC)
						{
							SMC->SetMaterial(0, Mat);
							It->ReregisterAllComponents();
							It->MarkPackageDirty();
							bAppliedToActor = true;
						}
						break;
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),               true);
		Resp->SetStringField(TEXT("asset_path"),            AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"),       bAlreadyExisted);
		Resp->SetBoolField  (TEXT("pixel_art"),             bPixelArt);
		Resp->SetBoolField  (TEXT("applied_to_blueprint"),  bAppliedToBlueprint);
		Resp->SetBoolField  (TEXT("applied_to_actor"),      bAppliedToActor);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_color_curve
// Body: { "asset_path": "/Game/.../Curve_X",
//         "keys": [ { "time": 0.0, "r": 0, "g": 1, "b": 0 }, ... ],
//         "linear": true }   // linear (default) or constant (stepped) interp
// Creates / overwrites a UCurveLinearColor with the given RGB keys (alpha = 1).
// ============================================================================

bool FNGGHttpServer::HandleCreateColorCurve(
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

	bool bLinear = true;
	Body->TryGetBoolField(TEXT("linear"), bLinear);

	struct FColorKeyIn { float Time = 0.f, R = 0.f, G = 0.f, B = 0.f; };
	TArray<FColorKeyIn> Keys;
	const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
	if (Body->TryGetArrayField(TEXT("keys"), KeysArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *KeysArr)
		{
			const TSharedPtr<FJsonObject>* K = nullptr;
			if (V->TryGetObject(K) && K)
			{
				FColorKeyIn Key;
				double D;
				if ((*K)->TryGetNumberField(TEXT("time"), D)) Key.Time = (float)D;
				if ((*K)->TryGetNumberField(TEXT("r"),    D)) Key.R    = (float)D;
				if ((*K)->TryGetNumberField(TEXT("g"),    D)) Key.G    = (float)D;
				if ((*K)->TryGetNumberField(TEXT("b"),    D)) Key.B    = (float)D;
				Keys.Add(Key);
			}
		}
	}
	if (Keys.Num() == 0)
	{
		Callback(JsonError(400, TEXT("keys[] is required — each { time, r, g, b }")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Keys, bLinear]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		UCurveLinearColor* Curve = LoadObject<UCurveLinearColor>(nullptr, *AssetPath);
		const bool bAlreadyExisted = (Curve != nullptr);
		if (!Curve)
		{
			UPackage* Package = CreatePackage(*AssetPath);
			if (!Package)
			{
				Callback(JsonError(500, FString::Printf(TEXT("Failed to create package for '%s'"), *AssetPath)));
				return;
			}
			Curve = NewObject<UCurveLinearColor>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Curve);
		}
		if (!Curve)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create curve at '%s'"), *AssetPath)));
			return;
		}

		// Rebuild all four channel curves (R,G,B,A) from the supplied keys.
		for (int32 c = 0; c < 4; ++c)
		{
			Curve->FloatCurves[c].Reset();
		}
		const ERichCurveInterpMode Interp = bLinear ? RCIM_Linear : RCIM_Constant;
		for (const FColorKeyIn& K : Keys)
		{
			const FKeyHandle HR = Curve->FloatCurves[0].AddKey(K.Time, K.R);
			const FKeyHandle HG = Curve->FloatCurves[1].AddKey(K.Time, K.G);
			const FKeyHandle HB = Curve->FloatCurves[2].AddKey(K.Time, K.B);
			const FKeyHandle HA = Curve->FloatCurves[3].AddKey(K.Time, 1.0f);
			Curve->FloatCurves[0].SetKeyInterpMode(HR, Interp);
			Curve->FloatCurves[1].SetKeyInterpMode(HG, Interp);
			Curve->FloatCurves[2].SetKeyInterpMode(HB, Interp);
			Curve->FloatCurves[3].SetKeyInterpMode(HA, Interp);
		}

		Curve->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Resp->SetNumberField(TEXT("key_count"),       Keys.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_post_process_material
//
// Builds a UMaterial with MaterialDomain = MD_PostProcess that highlights any
// mesh whose RenderCustomDepthPass is enabled, but only when the mesh is
// occluded by another opaque pixel (i.e. behind a wall). Used by the
// "enemy through walls" outline effect.
//
// Body:
//   {
//     "asset_path": "/Game/Materials/M_EnemyHighlight",
//     "effect": "occlusion_outline",       // optional, only one supported today
//     "highlight_color": {"r":1,"g":0.05,"b":0.05}
//   }
//
// Graph (when effect == "occlusion_outline"):
//   SceneTexture(SceneDepth)        ──┐
//                                     ├─→ Mask.R ─→ NamedRefSceneDepth ─┐
//   SceneTexture(CustomDepth)       ──┘                                  │
//                ↳ Mask.R ─→ NamedRefCustomDepth ─→ Frac ─→ Round ──┐    │
//   SceneTexture(PostProcessInput0)                                 │    │
//                ↳ Mask.RGB ─→ NamedRefOriginalColor                │    │
//                                                                   │    │
//   Lerp(OriginalColor, HighlightColor, Round)                  ←───┘    │
//                                                                        │
//   If(CustomDepth > SceneDepth ? Lerp : OriginalColor) ─→ EmissiveColor─┘
// ============================================================================

bool FNGGHttpServer::HandleCreatePostProcessMaterial(
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

	FString Effect = TEXT("occlusion_outline");
	Body->TryGetStringField(TEXT("effect"), Effect);
	if (Effect != TEXT("occlusion_outline"))
	{
		Callback(JsonError(400, FString::Printf(
			TEXT("Unknown effect '%s'. Supported: 'occlusion_outline'"), *Effect)));
		return true;
	}

	// Highlight color (linear RGB). Default: bright red.
	float R = 1.0f, G = 0.05f, B = 0.05f;
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Body->TryGetObjectField(TEXT("highlight_color"), ColorObj))
		{
			double Rd, Gd, Bd;
			if ((*ColorObj)->TryGetNumberField(TEXT("r"), Rd)) R = (float)Rd;
			if ((*ColorObj)->TryGetNumberField(TEXT("g"), Gd)) G = (float)Gd;
			if ((*ColorObj)->TryGetNumberField(TEXT("b"), Bd)) B = (float)Bd;
		}
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Effect, R, G, B]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath);
		const bool bAlreadyExisted = (Mat != nullptr);

		if (!Mat)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
			Mat = Cast<UMaterial>(NewAsset);
		}

		if (!Mat)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create material at '%s'"), *AssetPath)));
			return;
		}

		// ---- Configure as a PostProcess material ----------------------------
		// Must be set BEFORE wiring expressions so the editor exposes the
		// EmissiveColor pin (the only output for PostProcess materials).
		Mat->MaterialDomain     = MD_PostProcess;
		Mat->BlendableLocation  = BL_SceneColorAfterTonemapping;
		Mat->BlendablePriority  = 0;

		// Clear any existing graph so we get a clean rebuild on every call.
		Mat->GetExpressionCollection().Empty();
		// Disconnect surface-domain pins that may still be wired from a previous build.
		Mat->GetEditorOnlyData()->BaseColor.Expression  = nullptr;
		Mat->GetEditorOnlyData()->Roughness.Expression  = nullptr;
		Mat->GetEditorOnlyData()->Metallic.Expression   = nullptr;
		Mat->GetEditorOnlyData()->Specular.Expression   = nullptr;
		Mat->GetEditorOnlyData()->EmissiveColor.Expression = nullptr;

		// ---- SceneTexture nodes ---------------------------------------------
		UMaterialExpressionSceneTexture* StSceneDepth =
			NewObject<UMaterialExpressionSceneTexture>(Mat);
		StSceneDepth->SceneTextureId = PPI_SceneDepth;
		StSceneDepth->MaterialExpressionEditorX = -1200;
		StSceneDepth->MaterialExpressionEditorY = -200;
		Mat->GetExpressionCollection().AddExpression(StSceneDepth);

		UMaterialExpressionSceneTexture* StCustomDepth =
			NewObject<UMaterialExpressionSceneTexture>(Mat);
		StCustomDepth->SceneTextureId = PPI_CustomDepth;
		StCustomDepth->MaterialExpressionEditorX = -1200;
		StCustomDepth->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(StCustomDepth);

		UMaterialExpressionSceneTexture* StColor =
			NewObject<UMaterialExpressionSceneTexture>(Mat);
		StColor->SceneTextureId = PPI_PostProcessInput0;
		StColor->MaterialExpressionEditorX = -1200;
		StColor->MaterialExpressionEditorY = 400;
		Mat->GetExpressionCollection().AddExpression(StColor);

		// ---- Component masks ------------------------------------------------
		UMaterialExpressionComponentMask* MaskSceneDepthR =
			NewObject<UMaterialExpressionComponentMask>(Mat);
		MaskSceneDepthR->R = 1; MaskSceneDepthR->G = 0; MaskSceneDepthR->B = 0; MaskSceneDepthR->A = 0;
		MaskSceneDepthR->Input.Expression  = StSceneDepth;
		MaskSceneDepthR->Input.OutputIndex = 0;
		MaskSceneDepthR->MaterialExpressionEditorX = -900;
		MaskSceneDepthR->MaterialExpressionEditorY = -200;
		Mat->GetExpressionCollection().AddExpression(MaskSceneDepthR);

		UMaterialExpressionComponentMask* MaskCustomDepthR =
			NewObject<UMaterialExpressionComponentMask>(Mat);
		MaskCustomDepthR->R = 1; MaskCustomDepthR->G = 0; MaskCustomDepthR->B = 0; MaskCustomDepthR->A = 0;
		MaskCustomDepthR->Input.Expression  = StCustomDepth;
		MaskCustomDepthR->Input.OutputIndex = 0;
		MaskCustomDepthR->MaterialExpressionEditorX = -900;
		MaskCustomDepthR->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(MaskCustomDepthR);

		UMaterialExpressionComponentMask* MaskColorRGB =
			NewObject<UMaterialExpressionComponentMask>(Mat);
		MaskColorRGB->R = 1; MaskColorRGB->G = 1; MaskColorRGB->B = 1; MaskColorRGB->A = 0;
		MaskColorRGB->Input.Expression  = StColor;
		MaskColorRGB->Input.OutputIndex = 0;
		MaskColorRGB->MaterialExpressionEditorX = -900;
		MaskColorRGB->MaterialExpressionEditorY = 400;
		Mat->GetExpressionCollection().AddExpression(MaskColorRGB);

		// ---- Frac → Round on custom depth -----------------------------------
		// Pixels with NO custom depth report a huge integer (~1e10) — frac() is
		// 0 there, round() stays 0. Pixels with custom depth almost always have
		// some fractional component → frac() > 0, round() = 1. This becomes the
		// lerp alpha (0 = original color, 1 = highlight color).
		UMaterialExpressionFrac* FracExpr = NewObject<UMaterialExpressionFrac>(Mat);
		FracExpr->Input.Expression  = MaskCustomDepthR;
		FracExpr->Input.OutputIndex = 0;
		FracExpr->MaterialExpressionEditorX = -600;
		FracExpr->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(FracExpr);

		UMaterialExpressionRound* RoundExpr = NewObject<UMaterialExpressionRound>(Mat);
		RoundExpr->Input.Expression  = FracExpr;
		RoundExpr->Input.OutputIndex = 0;
		RoundExpr->MaterialExpressionEditorX = -400;
		RoundExpr->MaterialExpressionEditorY = 100;
		Mat->GetExpressionCollection().AddExpression(RoundExpr);

		// ---- Highlight color constant ---------------------------------------
		UMaterialExpressionConstant3Vector* HighlightExpr =
			NewObject<UMaterialExpressionConstant3Vector>(Mat);
		HighlightExpr->Constant = FLinearColor(R, G, B, 1.f);
		HighlightExpr->MaterialExpressionEditorX = -400;
		HighlightExpr->MaterialExpressionEditorY = 300;
		Mat->GetExpressionCollection().AddExpression(HighlightExpr);

		// ---- Lerp(OriginalColor, HighlightColor, RoundAlpha) ----------------
		UMaterialExpressionLinearInterpolate* LerpExpr =
			NewObject<UMaterialExpressionLinearInterpolate>(Mat);
		LerpExpr->A.Expression       = MaskColorRGB;
		LerpExpr->A.OutputIndex      = 0;
		LerpExpr->B.Expression       = HighlightExpr;
		LerpExpr->B.OutputIndex      = 0;
		LerpExpr->Alpha.Expression   = RoundExpr;
		LerpExpr->Alpha.OutputIndex  = 0;
		LerpExpr->MaterialExpressionEditorX = -150;
		LerpExpr->MaterialExpressionEditorY = 200;
		Mat->GetExpressionCollection().AddExpression(LerpExpr);

		// ---- If (CustomDepth.R > SceneDepth.R) ? Lerp : OriginalColor -------
		// Equal/less-than branches return the original color so pixels in front
		// of (or co-located with) opaque geometry render normally — the high­
		// light only kicks in when the custom-depth object is *behind* something.
		UMaterialExpressionIf* IfExpr = NewObject<UMaterialExpressionIf>(Mat);
		IfExpr->A.Expression               = MaskCustomDepthR;
		IfExpr->A.OutputIndex              = 0;
		IfExpr->B.Expression               = MaskSceneDepthR;
		IfExpr->B.OutputIndex              = 0;
		IfExpr->AGreaterThanB.Expression   = LerpExpr;
		IfExpr->AGreaterThanB.OutputIndex  = 0;
		IfExpr->AEqualsB.Expression        = MaskColorRGB;
		IfExpr->AEqualsB.OutputIndex       = 0;
		IfExpr->ALessThanB.Expression      = MaskColorRGB;
		IfExpr->ALessThanB.OutputIndex     = 0;
		IfExpr->MaterialExpressionEditorX  = 100;
		IfExpr->MaterialExpressionEditorY  = 100;
		Mat->GetExpressionCollection().AddExpression(IfExpr);

		// ---- Wire If output → Material EmissiveColor ------------------------
		Mat->GetEditorOnlyData()->EmissiveColor.Expression  = IfExpr;
		Mat->GetEditorOnlyData()->EmissiveColor.OutputIndex = 0;

		// ---- Recompile + persist --------------------------------------------
		UMaterialEditingLibrary::RecompileMaterial(Mat);
		Mat->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Resp->SetStringField(TEXT("effect"),          Effect);
		Resp->SetStringField(TEXT("material_domain"), TEXT("PostProcess"));

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
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
// Handler: POST /assets/delete
//
// Body:
// {
//   "asset_path": "/Game/Data/Exercises/DA_ADL_06_IronClothes"
// }
//
// Deletes a content asset (Data Asset, Blueprint, Widget Blueprint, Material,
// Level, etc.) from the project. Uses ObjectTools::DeleteSingleObject after
// unloading the asset. The file on disk is also removed.
// ============================================================================

bool FNGGHttpServer::HandleAssetsDelete(
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

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath]()
	{
		// Strip the object name suffix if present (e.g. "/Game/Foo/Bar.Bar" -> "/Game/Foo/Bar")
		FString PackagePath = AssetPath;
		int32 DotIdx;
		if (PackagePath.FindChar(TEXT('.'), DotIdx))
		{
			PackagePath = PackagePath.Left(DotIdx);
		}

		// Find the asset via the Asset Registry
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
		if (!AssetData.IsValid())
		{
			// Try with just the package path + asset name derived from package
			FString AssetName = FPackageName::GetShortName(PackagePath);
			FString FullObjectPath = PackagePath + TEXT(".") + AssetName;
			AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
		}

		if (!AssetData.IsValid())
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("Asset not found: %s"), *AssetPath)));
			return;
		}

		// ForceDeleteObjects (below) fires an ensure inside ObjectTools.cpp —
		// reported as a crash in unattended/headless mode — when the asset is
		// still referenced by other packages. Detect on-disk referencers up
		// front and return a clean 409 instead of asserting and taking down the
		// editor. (The pcg_create_graph/_instance overwrite paths no longer
		// delete at all; this guards the remaining generic delete path.)
		{
			TArray<FName> Referencers;
			AssetRegistry.GetReferencers(
				FName(*PackagePath), Referencers,
				UE::AssetRegistry::EDependencyCategory::Package);
			const FName SelfPkg(*PackagePath);
			Referencers.RemoveAll([&SelfPkg](const FName& R) { return R == SelfPkg; });
			if (Referencers.Num() > 0)
			{
				TArray<FString> RefStrs;
				for (const FName& R : Referencers) { RefStrs.Add(R.ToString()); }
				Callback(JsonError(409, FString::Printf(
					TEXT("Asset '%s' is still referenced by %d package(s): %s — unbind/remove referencers before deleting (force-delete would assert)."),
					*AssetPath, Referencers.Num(), *FString::Join(RefStrs, TEXT(", ")))));
				return;
			}
		}

		// Load the asset so we can delete it
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			// Fallback: try LoadObject directly (works for Niagara systems and other complex assets)
			FString AssetName = FPackageName::GetShortName(PackagePath);
			FString FullPath = PackagePath + TEXT(".") + AssetName;
			Asset = LoadObject<UObject>(nullptr, *FullPath);
		}
		if (!Asset)
		{
			// Last resort: try loading the package and finding any object in it
			Asset = LoadObject<UObject>(nullptr, *PackagePath);
		}
		if (!Asset)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("Failed to load asset for deletion: %s"), *AssetPath)));
			return;
		}

		FString ClassName = Asset->GetClass()->GetName();

		// Collect the object for deletion
		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(Asset);

		// Clear editor selection before ForceDeleteObjects. ForceDeleteObjects
		// issues a NoteSelectionChange → UpdatePivotLocationForSelection →
		// ForEachElement<ITypedElementWorldInterface> pass that asserts if
		// USelection holds any dangling/unregistered TypedElementHandle (which
		// earlier handlers like spawn_actor/delete_actor can leave behind).
		// Deselecting first guarantees the pass iterates over nothing.
		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true, /*WarnAboutTools*/ false);
			if (USelection* Sel = GEditor->GetSelectedActors())   Sel->DeselectAll();
			if (USelection* Sel = GEditor->GetSelectedObjects())  Sel->DeselectAll();
			if (USelection* Sel = GEditor->GetSelectedComponents()) Sel->DeselectAll();
		}

		// Use ObjectTools to delete — handles reference cleanup
		int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

		// If ForceDeleteObjects failed, try deleting the file on disk directly
		if (DeletedCount == 0)
		{
			FString FilePath;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, FilePath, FPackageName::GetAssetPackageExtension()))
			{
				if (IFileManager::Get().Delete(*FilePath, false, true))
				{
					DeletedCount = 1;
					// Remove from asset registry
					AssetRegistry.AssetDeleted(Asset);
				}
			}
		}

		if (DeletedCount > 0)
		{
			TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
			Resp->SetBoolField  (TEXT("success"),    true);
			Resp->SetStringField(TEXT("deleted"),    AssetPath);
			Resp->SetStringField(TEXT("class"),      ClassName);
			Resp->SetNumberField(TEXT("count"),      DeletedCount);

			FString RespBody;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
			FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
			Callback(JsonOk(RespBody));
		}
		else
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("Failed to delete asset: %s — it may be referenced by other assets"),
				*AssetPath)));
		}
	});

	return true;
}

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
			TEXT("editor/shutdown is disabled: no AuthToken configured. Set one in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token>.")));
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
			TEXT("editor/build_and_run is disabled: no AuthToken configured. Set one in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token>.")));
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
			TEXT("editor/kill_and_restart is disabled: no AuthToken configured. Set one in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token>.")));
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
// Handler: POST /editor/add_widget_to_blueprint
//
// Adds one or more UMG widgets to an existing Widget Blueprint's widget tree.
// Widgets are appended to the existing hierarchy — does NOT clear existing widgets.
//
// Request body:
// {
//   "widget_blueprint": "/Game/MyGame/UI/WBP_Settings",
//   "widgets": [
//     {
//       "type":   "Slider",           // Button|TextBlock|Image|ProgressBar|EditableTextBox|
//                                     // ComboBoxString|Slider|VerticalBox|HorizontalBox|CanvasPanel
//       "name":   "MasterVolumeSlider",
//       "parent": "VerticalBox_Root"  // optional — name of an already-existing parent widget
//                                     // (can be a widget added earlier in this same request)
//     }
//   ]
// }
//
// Returns: { success, widget_blueprint, widgets_added, widgets_skipped }
// ============================================================================

bool FNGGHttpServer::HandleAddWidgetToBlueprint(
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

	FString WBPPath;
	Body->TryGetStringField(TEXT("widget_blueprint"), WBPPath);
	if (WBPPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("widget_blueprint is required")));
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> WidgetsJson;
	if (Body->HasField(TEXT("widgets")))
	{
		const TArray<TSharedPtr<FJsonValue>>* WidgetsPtr = nullptr;
		if (!Body->TryGetArrayField(TEXT("widgets"), WidgetsPtr) || !WidgetsPtr)
		{
			Callback(JsonError(400, TEXT("'widgets' must be an array")));
			return true;
		}
		WidgetsJson = *WidgetsPtr;
	}

	if (WidgetsJson.Num() == 0)
	{
		Callback(JsonError(400, TEXT("widgets array is required and must not be empty")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [WBPPath, WidgetsJson, Callback]()
	{
		UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *WBPPath);
		if (!IsValid(WBP))
		{
			Callback(JsonError(404, FString::Printf(TEXT("Widget Blueprint not found: %s"), *WBPPath)));
			return;
		}
		if (!IsValid(WBP->WidgetTree))
		{
			Callback(JsonError(500, TEXT("Widget Blueprint has no WidgetTree")));
			return;
		}

		// Map widget type string -> UClass*
		auto ResolveWidgetClass = [](const FString& TypeName) -> UClass*
		{
			if (TypeName == TEXT("Button"))          return UButton::StaticClass();
			if (TypeName == TEXT("TextBlock"))        return UTextBlock::StaticClass();
			if (TypeName == TEXT("ComboBoxString"))   return UComboBoxString::StaticClass();
			if (TypeName == TEXT("EditableTextBox"))  return UEditableTextBox::StaticClass();
			if (TypeName == TEXT("ProgressBar"))      return UProgressBar::StaticClass();
			if (TypeName == TEXT("Image"))            return UImage::StaticClass();
			if (TypeName == TEXT("Slider"))           return USlider::StaticClass();
			if (TypeName == TEXT("VerticalBox"))      return UVerticalBox::StaticClass();
			if (TypeName == TEXT("HorizontalBox"))    return UHorizontalBox::StaticClass();
			if (TypeName == TEXT("CanvasPanel"))      return UCanvasPanel::StaticClass();
			return UTextBlock::StaticClass(); // safe fallback
		};

		// Build a map of all EXISTING widgets so new widgets can parent to them
		TMap<FString, UWidget*> NameToWidget;
		WBP->WidgetTree->ForEachWidget([&NameToWidget](UWidget* W)
		{
			if (W) { NameToWidget.Add(W->GetFName().ToString(), W); }
		});

		// Ensure there's a root canvas panel — create one if missing
		UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
		if (!RootCanvas)
		{
			// If root exists but is not a canvas, we still use it as fallback
			// Create a new canvas and set it as root if there's nothing
			if (!IsValid(WBP->WidgetTree->RootWidget))
			{
				RootCanvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
					UCanvasPanel::StaticClass(), TEXT("CanvasPanel_Root"));
				WBP->WidgetTree->RootWidget = RootCanvas;
				NameToWidget.Add(TEXT("CanvasPanel_Root"), RootCanvas);
				// UE5.4+ compiler ensures every named widget has a GUID entry.
				if (RootCanvas) { WBP->WidgetVariableNameToGuidMap.Add(RootCanvas->GetFName(), FGuid::NewGuid()); }
			}
		}

		// Self-heal: existing widgets in the tree without GUIDs would trip
		// the WidgetBlueprintCompiler ensure on next compile. Common when the
		// WBP was created by older plugin builds.
		WBP->WidgetTree->ForEachWidget([WBP](UWidget* W)
		{
			if (W && !WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
			{
				WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
			}
		});

		int32 Added = 0, Skipped = 0;

		for (const TSharedPtr<FJsonValue>& Entry : WidgetsJson)
		{
			const TSharedPtr<FJsonObject>* EntryObj;
			if (!Entry->TryGetObject(EntryObj)) { ++Skipped; continue; }

			FString TypeName, WidgetName, ParentName;
			(*EntryObj)->TryGetStringField(TEXT("type"),   TypeName);
			(*EntryObj)->TryGetStringField(TEXT("name"),   WidgetName);
			(*EntryObj)->TryGetStringField(TEXT("parent"), ParentName);

			if (WidgetName.IsEmpty()) { ++Skipped; continue; }

			// Skip if widget with this name already exists
			if (NameToWidget.Contains(WidgetName)) { ++Skipped; continue; }

			UClass* WidgetClass = ResolveWidgetClass(TypeName);
			UWidget* W = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, *WidgetName);
			if (!W) { ++Skipped; continue; }
			WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());

			NameToWidget.Add(WidgetName, W);

			// Find parent panel
			UWidget** ParentPtr = ParentName.IsEmpty() ? nullptr : NameToWidget.Find(ParentName);
			UPanelWidget* ParentPanel = ParentPtr ? Cast<UPanelWidget>(*ParentPtr) : nullptr;

			if (ParentPanel)
			{
				ParentPanel->AddChild(W);
			}
			else if (RootCanvas)
			{
				UCanvasPanelSlot* Slot = RootCanvas->AddChildToCanvas(W);
				if (Slot) Slot->SetAutoSize(true);
			}
			else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget))
			{
				RootPanel->AddChild(W);
			}

			++Added;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
		WBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),          true);
		Result->SetStringField(TEXT("widget_blueprint"), WBPPath);
		Result->SetNumberField(TEXT("widgets_added"),    static_cast<double>(Added));
		Result->SetNumberField(TEXT("widgets_skipped"),  static_cast<double>(Skipped));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		Callback(JsonOk(BodyStr));
	});

	return true;
}

// ============================================================================
// Handler: POST /assets/import
//
// Import an external file (wav, png, etc.) into the UE5 content browser using
// the editor's asset tools import pipeline.
//
// Request body:
// {
//   "source_path":   "C:/MyGameAudio/SFX_Explosion.wav",   // absolute disk path
//   "dest_path":     "/Game/MyGame/Audio",                 // content browser folder
//   "asset_name":    "SFX_Explosion"                          // optional: override filename stem
// }
//
// Returns: { success, asset_path, source_path }
// ============================================================================

bool FNGGHttpServer::HandleImportAsset(
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

	FString SourcePath, DestPath, AssetName;
	Body->TryGetStringField(TEXT("source_path"), SourcePath);
	Body->TryGetStringField(TEXT("dest_path"),   DestPath);
	Body->TryGetStringField(TEXT("asset_name"),  AssetName);

	if (SourcePath.IsEmpty() || DestPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("source_path and dest_path are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log, TEXT("HandleImportAsset: queued import src='%s' dest='%s' name='%s'"),
		*SourcePath, *DestPath, *AssetName);

	// Dispatch via FTSTicker so the import work runs on the game thread OUTSIDE
	// any TaskGraph task. Interchange's WaitUntilDone pumps the GameThread task
	// queue synchronously; running the handler as an AsyncTask(GameThread) lambda
	// re-enters FNamedTaskThread::ProcessTasksUntilIdle and trips the recursion
	// guard. Ticker delegates fire during engine tick, not inside a task.
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([SourcePath, DestPath, AssetName, Callback](float /*DeltaTime*/) -> bool
	{
		// Verify source file exists
		if (!FPaths::FileExists(SourcePath))
		{
			UE_LOG(LogNGGBridge, Warning, TEXT("HandleImportAsset: source file not found: %s"), *SourcePath);
			Callback(JsonError(404, FString::Printf(TEXT("Source file not found: %s"), *SourcePath)));
			return false;
		}

		// Resolve dest_path to a filesystem folder
		FString PackagePath = DestPath;
		// Normalise: remove trailing slash
		while (PackagePath.EndsWith(TEXT("/"))) { PackagePath.RemoveAt(PackagePath.Len()-1); }

		FString DiskFolder;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath + TEXT("/"), DiskFolder))
		{
			UE_LOG(LogNGGBridge, Warning, TEXT("HandleImportAsset: cannot resolve dest_path '%s'"), *PackagePath);
			Callback(JsonError(400, FString::Printf(TEXT("Cannot resolve dest_path to disk: %s"), *PackagePath)));
			return false;
		}

		// Determine the final asset name
		FString FinalName = AssetName.IsEmpty() ? FPaths::GetBaseFilename(SourcePath) : AssetName;

		// Check if the source is an image — use UTextureFactory directly to
		// avoid the Interchange async pipeline which crashes with a task-graph
		// recursion guard when called from a game-thread AsyncTask.
		FString Ext = FPaths::GetExtension(SourcePath).ToLower();
		bool bIsImage = (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg")
			|| Ext == TEXT("tga") || Ext == TEXT("bmp") || Ext == TEXT("exr") || Ext == TEXT("hdr"));

		UObject* ImportedObject = nullptr;

		if (bIsImage)
		{
			// Legacy factory path — fully synchronous, no Interchange
			UTextureFactory* TexFactory = NewObject<UTextureFactory>();
			TexFactory->AddToRoot(); // prevent GC during import
			TexFactory->SuppressImportOverwriteDialog();

			FString FullPackagePath = PackagePath / FinalName;
			UPackage* Pkg = CreatePackage(*FullPackagePath);
			Pkg->FullyLoad();

			bool bCancelled = false;
			ImportedObject = TexFactory->FactoryCreateFile(
				UTexture2D::StaticClass(), Pkg, *FinalName,
				RF_Public | RF_Standalone, SourcePath,
				nullptr, GWarn, bCancelled);

			if (ImportedObject)
			{
				FAssetRegistryModule::AssetCreated(ImportedObject);
				Pkg->MarkPackageDirty();
				// Save the package so the asset persists
				FString PkgFilename;
				if (FPackageName::TryConvertLongPackageNameToFilename(
						FullPackagePath, PkgFilename, FPackageName::GetAssetPackageExtension()))
				{
					FSavePackageArgs SaveArgs;
					SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
					UPackage::SavePackage(Pkg, ImportedObject, *PkgFilename, SaveArgs);
				}
			}

			TexFactory->RemoveFromRoot();
		}
		else
		{
			// Non-image files: use ImportAssetTasks but disable Interchange
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename         = SourcePath;
			Task->DestinationPath  = PackagePath;
			Task->DestinationName  = FinalName;
			Task->bAutomated       = true;
			Task->bSave            = true;
			Task->bReplaceExisting = true;

			FAssetToolsModule& AssetToolsModule =
				FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
			TArray<UAssetImportTask*> Tasks;
			Tasks.Add(Task);
			AssetToolsModule.Get().ImportAssetTasks(Tasks);

			if (Task->GetObjects().Num() > 0)
			{
				ImportedObject = Task->GetObjects()[0];
			}
		}

		// Determine the resulting asset path
		FString ResultAssetPath;
		if (ImportedObject)
		{
			ResultAssetPath = ImportedObject->GetOutermost()->GetName();
		}

		if (ResultAssetPath.IsEmpty())
		{
			ResultAssetPath = PackagePath + TEXT("/") + FinalName;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"),     true);
		Result->SetStringField(TEXT("asset_path"),  ResultAssetPath);
		Result->SetStringField(TEXT("source_path"), SourcePath);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		if (ImportedObject)
		{
			UE_LOG(LogNGGBridge, Log, TEXT("HandleImportAsset: success asset='%s' src='%s'"),
				*ResultAssetPath, *SourcePath);
		}
		else
		{
			UE_LOG(LogNGGBridge, Warning, TEXT("HandleImportAsset: no object produced for src='%s' (path='%s')"),
				*SourcePath, *ResultAssetPath);
		}

		Callback(JsonOk(BodyStr));
		return false; // one-shot ticker
	}), 0.0f);

	return true;
}

// ============================================================================
// Handler: POST /editor/configure_anim_blueprint
//
// Wire an Animation Blueprint's state machine with animation sequences/blend
// spaces.  Creates states, assigns sequence/blend-space players, and adds
// transition rules based on variable conditions.
// ============================================================================

bool FNGGHttpServer::HandleConfigureAnimBlueprint(
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

	// Parse states array
	struct FStateInput
	{
		FString Name;
		FString AnimationPath;
		bool bLoop = true;
	};
	TArray<FStateInput> States;
	{
		const TArray<TSharedPtr<FJsonValue>>* StatesArr = nullptr;
		if (Body->TryGetArrayField(TEXT("states"), StatesArr) && StatesArr)
		{
			for (const auto& Val : *StatesArr)
			{
				const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
				if (!Obj) continue;
				FStateInput S;
				Obj->TryGetStringField(TEXT("name"), S.Name);
				Obj->TryGetStringField(TEXT("animation"), S.AnimationPath);
				Obj->TryGetBoolField(TEXT("loop"), S.bLoop);
				States.Add(MoveTemp(S));
			}
		}
	}

	// Parse transitions array
	struct FTransInput
	{
		FString From;
		FString To;
		FString Condition;
	};
	TArray<FTransInput> Transitions;
	{
		const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
		if (Body->TryGetArrayField(TEXT("transitions"), TransArr) && TransArr)
		{
			for (const auto& Val : *TransArr)
			{
				const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
				if (!Obj) continue;
				FTransInput T;
				Obj->TryGetStringField(TEXT("from"), T.From);
				Obj->TryGetStringField(TEXT("to"), T.To);
				Obj->TryGetStringField(TEXT("condition"), T.Condition);
				Transitions.Add(MoveTemp(T));
			}
		}
	}

	if (States.Num() == 0)
	{
		Callback(JsonError(400, TEXT("At least one state is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, States, Transitions]()
	{
		// 1. Load the AnimBlueprint asset
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
		if (!AnimBP)
		{
			// Try with object name appended
			FString AltPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("AnimBlueprint not found: %s"), *AssetPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: loaded %s"), *AnimBP->GetPathName());

		// 2. Find or create the state machine node in the AnimGraph
		UAnimGraphNode_StateMachine* StateMachineNode = nullptr;

		// Find the anim graph — typically named "AnimGraph"
		UEdGraph* AnimGraph = nullptr;
		for (UEdGraph* Graph : AnimBP->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
			{
				AnimGraph = Graph;
				break;
			}
		}
		if (!AnimGraph)
		{
			for (UEdGraph* Graph : AnimBP->UbergraphPages)
			{
				if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
				{
					AnimGraph = Graph;
					break;
				}
			}
		}

		if (!AnimGraph)
		{
			Callback(JsonError(500, TEXT("Could not find AnimGraph in the AnimBlueprint")));
			return;
		}

		// Find existing state machine node or create one
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			StateMachineNode = Cast<UAnimGraphNode_StateMachine>(Node);
			if (StateMachineNode) break;
		}

		if (!StateMachineNode)
		{
			StateMachineNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
			StateMachineNode->CreateNewGuid();
			StateMachineNode->NodePosX = 200;
			StateMachineNode->NodePosY = 0;
			AnimGraph->AddNode(StateMachineNode, false, false);
			StateMachineNode->AllocateDefaultPins();
			// PostPlacedNewNode creates the EditorStateMachineGraph
			StateMachineNode->PostPlacedNewNode();
			UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: created new state machine node"));
		}

		// 3. Get the state machine graph — initialize if missing
		UAnimationStateMachineGraph* SMGraph = StateMachineNode->EditorStateMachineGraph;
		if (!SMGraph)
		{
			// The node exists but its graph was never created — create it manually
			SMGraph = CastChecked<UAnimationStateMachineGraph>(
				FBlueprintEditorUtils::CreateNewGraph(
					StateMachineNode, NAME_None,
					UAnimationStateMachineGraph::StaticClass(),
					UAnimationStateMachineSchema::StaticClass()));
			StateMachineNode->EditorStateMachineGraph = SMGraph;
			SMGraph->OwnerAnimGraphNode = StateMachineNode;
			const UEdGraphSchema* SMSchema = SMGraph->GetSchema();
			if (SMSchema)
			{
				SMSchema->CreateDefaultNodesForGraph(*SMGraph);
			}
			if (AnimGraph->SubGraphs.Find(SMGraph) == INDEX_NONE)
			{
				AnimGraph->SubGraphs.Add(SMGraph);
			}
			UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: manually created state machine graph"));
		}
		if (!SMGraph)
		{
			Callback(JsonError(500, TEXT("State machine node has no EditorStateMachineGraph after initialization")));
			return;
		}

		// 4. Create state nodes
		TMap<FString, UAnimStateNode*> StateNodeMap;
		int32 PosX = 300;
		int32 PosY = 0;
		int32 StatesCreated = 0;

		for (const auto& StateInput : States)
		{
			// Check if state already exists
			UAnimStateNode* ExistingState = nullptr;
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				UAnimStateNode* SN = Cast<UAnimStateNode>(Node);
				if (SN && SN->GetStateName() == StateInput.Name)
				{
					ExistingState = SN;
					break;
				}
			}

			if (ExistingState)
			{
				StateNodeMap.Add(StateInput.Name, ExistingState);
				UE_LOG(LogNGGBridge, Log, TEXT("  State '%s' already exists, reusing"), *StateInput.Name);
				continue;
			}

			// Create new state node via schema action
			UAnimStateNode* TemplateState = NewObject<UAnimStateNode>(SMGraph);
			UAnimStateNode* NewState = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
				SMGraph, TemplateState, FVector2f(PosX, PosY), false);

			if (!NewState)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Failed to create state node '%s'"), *StateInput.Name);
				continue;
			}

			NewState->Modify();
			NewState->NodeComment = StateInput.Name;

			UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(NewState->BoundGraph);
			if (StateGraph)
			{
				StateGraph->Rename(*StateInput.Name, nullptr, REN_DontCreateRedirectors);
			}

			// Load the animation asset and create the appropriate player node
			UObject* AnimAsset = LoadObject<UObject>(nullptr, *StateInput.AnimationPath);
			if (!AnimAsset)
			{
				FString AltAnimPath = StateInput.AnimationPath + TEXT(".") + FPaths::GetBaseFilename(StateInput.AnimationPath);
				AnimAsset = LoadObject<UObject>(nullptr, *AltAnimPath);
			}

			if (AnimAsset && StateGraph)
			{
				// Find the result node in the state graph
				UAnimGraphNode_StateResult* ResultNode = nullptr;
				for (UEdGraphNode* GNode : StateGraph->Nodes)
				{
					ResultNode = Cast<UAnimGraphNode_StateResult>(GNode);
					if (ResultNode) break;
				}

				if (ResultNode)
				{
					UBlendSpace* BS = Cast<UBlendSpace>(AnimAsset);
					UAnimSequence* Seq = Cast<UAnimSequence>(AnimAsset);

					UAnimGraphNode_Base* PlayerNode = nullptr;

					if (BS)
					{
						UAnimGraphNode_BlendSpacePlayer* BSPlayer =
							NewObject<UAnimGraphNode_BlendSpacePlayer>(StateGraph);
						BSPlayer->CreateNewGuid();
						BSPlayer->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
						BSPlayer->NodePosX = ResultNode->NodePosX - 300;
						BSPlayer->NodePosY = ResultNode->NodePosY;
						StateGraph->AddNode(BSPlayer, false, false);
						BSPlayer->AllocateDefaultPins();
						BSPlayer->SetAnimationAsset(BS);
						BSPlayer->Node.SetLoop(StateInput.bLoop);
						PlayerNode = BSPlayer;
						UE_LOG(LogNGGBridge, Log, TEXT("  State '%s': BlendSpacePlayer -> %s"),
							*StateInput.Name, *BS->GetName());
					}
					else if (Seq)
					{
						UAnimGraphNode_SequencePlayer* SeqPlayer =
							NewObject<UAnimGraphNode_SequencePlayer>(StateGraph);
						SeqPlayer->CreateNewGuid();
						SeqPlayer->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
						SeqPlayer->NodePosX = ResultNode->NodePosX - 300;
						SeqPlayer->NodePosY = ResultNode->NodePosY;
						StateGraph->AddNode(SeqPlayer, false, false);
						SeqPlayer->AllocateDefaultPins();
						SeqPlayer->SetAnimationAsset(Seq);
						SeqPlayer->Node.SetLoopAnimation(StateInput.bLoop);
						PlayerNode = SeqPlayer;
						UE_LOG(LogNGGBridge, Log, TEXT("  State '%s': SequencePlayer -> %s"),
							*StateInput.Name, *Seq->GetName());
					}
					else
					{
						UE_LOG(LogNGGBridge, Warning,
							TEXT("  State '%s': asset '%s' is neither AnimSequence nor BlendSpace (class: %s)"),
							*StateInput.Name, *StateInput.AnimationPath,
							*AnimAsset->GetClass()->GetName());
					}

					// Connect player output pose pin to result node input
					if (PlayerNode)
					{
						// Find the first output pin on the player node
						UEdGraphPin* OutputPin = nullptr;
						for (UEdGraphPin* Pin : PlayerNode->Pins)
						{
							if (Pin->Direction == EGPD_Output)
							{
								OutputPin = Pin;
								break;
							}
						}
						// Find the first input pin on the result node
						UEdGraphPin* InputPin = nullptr;
						for (UEdGraphPin* Pin : ResultNode->Pins)
						{
							if (Pin->Direction == EGPD_Input)
							{
								InputPin = Pin;
								break;
							}
						}
						if (OutputPin && InputPin)
						{
							const UEdGraphSchema* Schema = StateGraph->GetSchema();
							if (Schema)
							{
								Schema->TryCreateConnection(OutputPin, InputPin);
								UE_LOG(LogNGGBridge, Log, TEXT("    Connected %s -> %s"),
									*OutputPin->GetName(), *InputPin->GetName());
							}
						}
						else
						{
							UE_LOG(LogNGGBridge, Warning,
								TEXT("    Could not find output/input pins to connect player to result"));
						}
					}
				}
			}
			else if (!AnimAsset)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  State '%s': animation not found: %s"),
					*StateInput.Name, *StateInput.AnimationPath);
			}

			StateNodeMap.Add(StateInput.Name, NewState);
			StatesCreated++;

			PosX += 250;
			PosY += 100;
		}

		// 5. Create transitions
		int32 TransitionsCreated = 0;
		for (const auto& Trans : Transitions)
		{
			UAnimStateNode** FromNodePtr = StateNodeMap.Find(Trans.From);
			UAnimStateNode** ToNodePtr = StateNodeMap.Find(Trans.To);

			if (!FromNodePtr || !*FromNodePtr)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Transition '%s' -> '%s': source state not found"),
					*Trans.From, *Trans.To);
				continue;
			}
			if (!ToNodePtr || !*ToNodePtr)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Transition '%s' -> '%s': target state not found"),
					*Trans.From, *Trans.To);
				continue;
			}

			UAnimStateTransitionNode* TemplateTransNode = NewObject<UAnimStateTransitionNode>(SMGraph);
			UAnimStateTransitionNode* TransNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
				SMGraph, TemplateTransNode,
				FVector2f(
					((*FromNodePtr)->NodePosX + (*ToNodePtr)->NodePosX) / 2.0f,
					((*FromNodePtr)->NodePosY + (*ToNodePtr)->NodePosY) / 2.0f),
				false);

			if (!TransNode)
			{
				UE_LOG(LogNGGBridge, Warning, TEXT("  Failed to create transition '%s' -> '%s'"),
					*Trans.From, *Trans.To);
				continue;
			}

			UEdGraphPin* FromOutput = (*FromNodePtr)->GetOutputPin();
			UEdGraphPin* ToInput = (*ToNodePtr)->GetInputPin();
			UEdGraphPin* TransInput = TransNode->GetInputPin();
			UEdGraphPin* TransOutput = TransNode->GetOutputPin();

			if (FromOutput && TransInput)
			{
				FromOutput->MakeLinkTo(TransInput);
			}
			if (TransOutput && ToInput)
			{
				TransOutput->MakeLinkTo(ToInput);
			}

			TransNode->NodeComment = Trans.Condition;

			UE_LOG(LogNGGBridge, Log, TEXT("  Transition: %s -> %s [%s]"),
				*Trans.From, *Trans.To, *Trans.Condition);
			TransitionsCreated++;
		}

		// 6. Connect the entry node to the first state
		if (States.Num() > 0)
		{
			UAnimStateNode** FirstStatePtr = StateNodeMap.Find(States[0].Name);
			if (FirstStatePtr && *FirstStatePtr)
			{
				UAnimStateEntryNode* EntryNode = SMGraph->EntryNode;
				if (EntryNode)
				{
					UEdGraphPin* EntryOutput = EntryNode->GetOutputPin();
					UEdGraphPin* FirstInput = (*FirstStatePtr)->GetInputPin();
					if (EntryOutput && FirstInput)
					{
						EntryOutput->BreakAllPinLinks();
						EntryOutput->MakeLinkTo(FirstInput);
						UE_LOG(LogNGGBridge, Log, TEXT("  Entry -> %s wired"), *States[0].Name);
					}
				}
			}
		}

		// 7. Compile the blueprint
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log, TEXT("ConfigureAnimBlueprint: done — %d states, %d transitions"),
			StatesCreated, TransitionsCreated);

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),              true);
		Resp->SetStringField(TEXT("asset_path"),           AssetPath);
		Resp->SetNumberField(TEXT("states_created"),       StatesCreated);
		Resp->SetNumberField(TEXT("transitions_created"),  TransitionsCreated);
		Resp->SetNumberField(TEXT("total_states"),         StateNodeMap.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_two_bone_ik
//
// Insert a Two Bone IK skeletal control node into an Animation Blueprint's
// AnimGraph, splice it between the current pose source and the Output Pose
// (UAnimGraphNode_Root), configure its bone+space properties, then compile.
//
// EffectorLocation, JointTargetLocation, and Alpha pins are left exposed for
// the caller to bind via Property Access in the editor (right-click pin -> Bind),
// since binding to pawn-owned variables requires walking a chain of K2 helper
// nodes that's better authored interactively than synthesised here.
//
// Body:
//   {
//     "anim_bp_path": "/Game/Path/ABP_X",            // required
//     "ik_bone": "hand_l",                            // required - bone the IK drives
//     "effector_location_space": "WorldSpace",        // optional - WorldSpace|ComponentSpace|ParentBoneSpace|BoneSpace
//     "joint_target_location_space": "ComponentSpace",// optional - same enum
//     "take_rotation_from_effector": true,            // optional - bTakeRotationFromEffectorSpace
//     "node_offset_x": -250,                          // optional - X offset from Output Pose node
//     "node_offset_y": 0                              // optional - Y offset from Output Pose node
//   }
// ============================================================================

namespace
{
	/** Translate a friendly enum string into EBoneControlSpace. Returns BCS_WorldSpace on miss. */
	static EBoneControlSpace ParseBoneControlSpace(const FString& Name, EBoneControlSpace Default = BCS_WorldSpace)
	{
		if (Name.Equals(TEXT("WorldSpace"),      ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("World"),           ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_WorldSpace"),  ESearchCase::IgnoreCase))      return BCS_WorldSpace;
		if (Name.Equals(TEXT("ComponentSpace"),  ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Component"),       ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_ComponentSpace"), ESearchCase::IgnoreCase))   return BCS_ComponentSpace;
		if (Name.Equals(TEXT("ParentBoneSpace"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("ParentBone"),      ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_ParentBoneSpace"), ESearchCase::IgnoreCase))  return BCS_ParentBoneSpace;
		if (Name.Equals(TEXT("BoneSpace"),       ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Bone"),            ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BCS_BoneSpace"),   ESearchCase::IgnoreCase))      return BCS_BoneSpace;
		return Default;
	}
}

bool FNGGHttpServer::HandleAnimAddTwoBoneIK(
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

	FString AssetPath, IKBoneName;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("ik_bone"),       IKBoneName);
	if (AssetPath.IsEmpty() || IKBoneName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'ik_bone' are required")));
		return true;
	}

	FString EffectorSpaceStr   = TEXT("WorldSpace");
	FString JointTargetSpaceStr = TEXT("ComponentSpace");
	bool    bTakeRotation       = true;
	double  OffsetX             = -250.0;
	double  OffsetY             = 0.0;
	FString EffectorTargetBone, JointTargetBone;
	double  JointTargetOffsetX = 0.0, JointTargetOffsetY = 0.0, JointTargetOffsetZ = 0.0;
	Body->TryGetStringField(TEXT("effector_location_space"),    EffectorSpaceStr);
	Body->TryGetStringField(TEXT("joint_target_location_space"), JointTargetSpaceStr);
	Body->TryGetBoolField  (TEXT("take_rotation_from_effector"), bTakeRotation);
	Body->TryGetNumberField(TEXT("node_offset_x"),               OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"),               OffsetY);
	Body->TryGetStringField(TEXT("effector_target_bone"),        EffectorTargetBone);
	Body->TryGetStringField(TEXT("joint_target_bone"),           JointTargetBone);
	{
		const TArray<TSharedPtr<FJsonValue>>* OffArr = nullptr;
		if (Body->TryGetArrayField(TEXT("joint_target_offset"), OffArr) && OffArr && OffArr->Num() >= 3)
		{
			JointTargetOffsetX = (*OffArr)[0]->AsNumber();
			JointTargetOffsetY = (*OffArr)[1]->AsNumber();
			JointTargetOffsetZ = (*OffArr)[2]->AsNumber();
		}
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_two_bone_ik bp='%s' bone='%s' effector_space='%s' joint_space='%s' take_rot=%d eff_target='%s' jt_target='%s' jt_off=[%.1f %.1f %.1f]"),
		*AssetPath, *IKBoneName, *EffectorSpaceStr, *JointTargetSpaceStr, bTakeRotation ? 1 : 0,
		*EffectorTargetBone, *JointTargetBone, JointTargetOffsetX, JointTargetOffsetY, JointTargetOffsetZ);

	const FString CapturedAssetPath = AssetPath;
	const FString CapturedBoneName  = IKBoneName;
	const EBoneControlSpace EffectorSpace   = ParseBoneControlSpace(EffectorSpaceStr,   BCS_WorldSpace);
	const EBoneControlSpace JointTargetSpace = ParseBoneControlSpace(JointTargetSpaceStr, BCS_ComponentSpace);
	const bool   CapturedTakeRotation = bTakeRotation;
	const int32  CapturedOffsetX = (int32)OffsetX;
	const int32  CapturedOffsetY = (int32)OffsetY;
	const FString CapturedEffectorTargetBone = EffectorTargetBone;
	const FString CapturedJointTargetBone    = JointTargetBone;
	const FVector CapturedJointTargetOffset(JointTargetOffsetX, JointTargetOffsetY, JointTargetOffsetZ);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapturedBoneName, EffectorSpace, JointTargetSpace,
		 CapturedTakeRotation, CapturedOffsetX, CapturedOffsetY,
		 CapturedEffectorTargetBone, CapturedJointTargetBone, CapturedJointTargetOffset]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapturedAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapturedAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapturedAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapturedAssetPath)));
			return;
		}

		// 2. Find the AnimGraph
		UEdGraph* AnimGraph = nullptr;
		for (UEdGraph* Graph : AnimBP->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == TEXT("AnimGraph")) { AnimGraph = Graph; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* Graph : AnimBP->UbergraphPages)
			{
				if (Graph && Graph->GetFName() == TEXT("AnimGraph")) { AnimGraph = Graph; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, TEXT("Could not find AnimGraph in the AnimBlueprint")));
			return;
		}

		// 3. Locate the Output Pose node (UAnimGraphNode_Root) and what's currently feeding it
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(Node);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		// Output Pose has exactly one input pin (Result). Find it and capture its current upstream link.
		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		UEdGraphPin* UpstreamPin = nullptr;
		FString      UpstreamNodeName;
		if (RootInputPin->LinkedTo.Num() > 0)
		{
			UpstreamPin = RootInputPin->LinkedTo[0];
			if (UpstreamPin && UpstreamPin->GetOwningNode())
			{
				UpstreamNodeName = UpstreamPin->GetOwningNode()->GetName();
			}
		}

		// 4. Spawn the Two Bone IK node
		UAnimGraphNode_TwoBoneIK* IKNode = NewObject<UAnimGraphNode_TwoBoneIK>(AnimGraph);
		IKNode->CreateNewGuid();
		IKNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		IKNode->NodePosX = RootNode->NodePosX + CapturedOffsetX;
		IKNode->NodePosY = RootNode->NodePosY + CapturedOffsetY;
		AnimGraph->AddNode(IKNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		IKNode->AllocateDefaultPins();

		// 5. Configure FAnimNode_TwoBoneIK runtime properties
		IKNode->Node.IKBone.BoneName              = FName(*CapturedBoneName);
		IKNode->Node.EffectorLocationSpace        = EffectorSpace;
		IKNode->Node.JointTargetLocationSpace     = JointTargetSpace;
		IKNode->Node.bTakeRotationFromEffectorSpace = CapturedTakeRotation ? 1 : 0;

		// Optional: bone-target mode for the effector — when supplied, the IK reads from a bone's
		// transform (FBoneSocketTarget) instead of using the EffectorLocation vector pin. This is
		// the tutorial's "Bone Space + Target Bone = ik_hand_*" pattern.
		if (!CapturedEffectorTargetBone.IsEmpty())
		{
			IKNode->Node.EffectorTarget.bUseSocket = false;
			IKNode->Node.EffectorTarget.BoneReference.BoneName = FName(*CapturedEffectorTargetBone);
		}
		// Same for the joint target — bone-driven elbow/knee direction.
		if (!CapturedJointTargetBone.IsEmpty())
		{
			IKNode->Node.JointTarget.bUseSocket = false;
			IKNode->Node.JointTarget.BoneReference.BoneName = FName(*CapturedJointTargetBone);
		}
		// Per-axis offset applied on top of the joint target (UE field is the standalone vector
		// JointTargetLocation; the tutorial uses Y=50 to push the elbow outward).
		if (!CapturedJointTargetOffset.IsZero())
		{
			IKNode->Node.JointTargetLocation = CapturedJointTargetOffset;
		}

		// 6. Splice the new node into the chain.
		// SkeletalControlBase exposes:
		//   - input pin "ComponentPose"  (FComponentSpacePoseLink, EGPD_Input)
		//   - output pose pin           (FComponentSpacePoseLink, EGPD_Output)
		// Plus property pins: EffectorLocation (vector), JointTargetLocation (vector), Alpha (float).
		// Identify the pose pins by name / direction / pose link struct type.
		UEdGraphPin* IKInputPosePin  = nullptr;
		UEdGraphPin* IKOutputPosePin = nullptr;
		for (UEdGraphPin* Pin : IKNode->Pins)
		{
			if (!Pin) continue;
			const bool bIsPoseStruct =
				Pin->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				Pin->PinType.PinCategory == TEXT("struct") && Pin->PinName == TEXT("ComponentPose");
			if (Pin->Direction == EGPD_Input && (Pin->PinName == TEXT("ComponentPose") || bIsPoseStruct))
			{
				IKInputPosePin = Pin;
			}
			else if (Pin->Direction == EGPD_Output && bIsPoseStruct)
			{
				IKOutputPosePin = Pin;
			}
		}
		if (!IKInputPosePin || !IKOutputPosePin)
		{
			Callback(JsonError(500, TEXT("Could not find ComponentPose input/output pins on the Two Bone IK node")));
			return;
		}

		const UEdGraphSchema* Schema = AnimGraph->GetSchema();
		if (!Schema)
		{
			Callback(JsonError(500, TEXT("AnimGraph has no schema")));
			return;
		}

		bool bSplicedUpstream = false;
		if (UpstreamPin)
		{
			RootInputPin->BreakAllPinLinks();
			Schema->TryCreateConnection(UpstreamPin, IKInputPosePin);
			bSplicedUpstream = true;
		}
		Schema->TryCreateConnection(IKOutputPosePin, RootInputPin);

		// 7. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_two_bone_ik: added IK node for bone '%s' (spliced_upstream=%d, upstream_was='%s')"),
			*CapturedBoneName, bSplicedUpstream ? 1 : 0, *UpstreamNodeName);

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),                 true);
		Resp->SetStringField(TEXT("asset_path"),         AnimBP->GetPathName());
		Resp->SetStringField(TEXT("ik_bone"),            CapturedBoneName);
		Resp->SetStringField(TEXT("ik_node_id"),         IKNode->NodeGuid.ToString());
		Resp->SetBoolField  (TEXT("spliced_upstream"),   bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty())
		{
			Resp->SetStringField(TEXT("upstream_was"),   UpstreamNodeName);
		}

		// Hint to the caller which pins still need binding in the editor.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		UnboundPins.Add(MakeShared<FJsonValueString>(TEXT("EffectorLocation")));
		UnboundPins.Add(MakeShared<FJsonValueString>(TEXT("JointTargetLocation")));
		UnboundPins.Add(MakeShared<FJsonValueString>(TEXT("Alpha")));
		Resp->SetArrayField(TEXT("unbound_pins"), UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Shared helper: load AnimBP and locate its AnimGraph + Output Pose node.
// Returns nullptr-out via the OutErr if anything goes wrong, with a 404-style message.
// ============================================================================

namespace
{
	struct FAnimGraphContext
	{
		UAnimBlueprint*       AnimBP   = nullptr;
		UEdGraph*             Graph    = nullptr;
		UAnimGraphNode_Root*  RootNode = nullptr;
		UEdGraphPin*          RootInputPin = nullptr;
	};

	static bool ResolveAnimGraphContext(const FString& AssetPath, FAnimGraphContext& Out, FString& OutErr)
	{
		Out.AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
		if (!Out.AnimBP)
		{
			const FString AltPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
			Out.AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!Out.AnimBP)
		{
			OutErr = FString::Printf(TEXT("AnimBlueprint not found: %s"), *AssetPath);
			return false;
		}

		for (UEdGraph* G : Out.AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == TEXT("AnimGraph")) { Out.Graph = G; break; }
		}
		if (!Out.Graph)
		{
			for (UEdGraph* G : Out.AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == TEXT("AnimGraph")) { Out.Graph = G; break; }
			}
		}
		if (!Out.Graph)
		{
			OutErr = TEXT("Could not find AnimGraph in the AnimBlueprint");
			return false;
		}

		for (UEdGraphNode* N : Out.Graph->Nodes)
		{
			Out.RootNode = Cast<UAnimGraphNode_Root>(N);
			if (Out.RootNode) break;
		}
		if (!Out.RootNode)
		{
			OutErr = TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph");
			return false;
		}

		for (UEdGraphPin* Pin : Out.RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { Out.RootInputPin = Pin; break; }
		}
		if (!Out.RootInputPin)
		{
			OutErr = TEXT("Output Pose node has no input pin");
			return false;
		}

		return true;
	}

	/** Locate the input ComponentPose pin and the output pose pin on a UAnimGraphNode_SkeletalControlBase node. */
	static void FindPosePins(UAnimGraphNode_Base* Node, UEdGraphPin*& OutIn, UEdGraphPin*& OutOut)
	{
		OutIn = nullptr; OutOut = nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P) continue;
			const bool bIsPoseStruct =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				(P->PinType.PinCategory == TEXT("struct") && P->PinName == TEXT("ComponentPose"));
			if (P->Direction == EGPD_Input && (P->PinName == TEXT("ComponentPose") || bIsPoseStruct))
			{
				OutIn = P;
			}
			else if (P->Direction == EGPD_Output && bIsPoseStruct)
			{
				OutOut = P;
			}
		}
	}

	/** Insert NewNode between (whatever feeds RootInputPin) and RootInputPin. Returns true if upstream was spliced. */
	static bool SpliceBeforeOutputPose(
		UEdGraph* Graph, UEdGraphPin* RootInputPin,
		UEdGraphPin* NewInPin, UEdGraphPin* NewOutPin,
		FString& OutUpstreamName)
	{
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema) return false;

		UEdGraphPin* UpstreamPin = nullptr;
		if (RootInputPin->LinkedTo.Num() > 0)
		{
			UpstreamPin = RootInputPin->LinkedTo[0];
			if (UpstreamPin && UpstreamPin->GetOwningNode())
			{
				OutUpstreamName = UpstreamPin->GetOwningNode()->GetName();
			}
		}

		bool bSpliced = false;
		if (UpstreamPin)
		{
			RootInputPin->BreakAllPinLinks();
			Schema->TryCreateConnection(UpstreamPin, NewInPin);
			bSpliced = true;
		}
		Schema->TryCreateConnection(NewOutPin, RootInputPin);
		return bSpliced;
	}
}

// ============================================================================
// Handler: POST /editor/anim/add_copy_bone
//
// Insert a Copy Bone skeletal control node into an AnimBP's AnimGraph,
// splice it before the Output Pose, configure source/target bone + flags.
//
// Body:
//   {
//     "anim_bp_path": "/Game/Path/ABP_X",        // required
//     "source_bone":  "ik_hand_l",                // required
//     "target_bone":  "VB_ik_hand_left_weapon_space", // required
//     "copy_translation": true,                   // optional, default true
//     "copy_rotation":    true,                   // optional, default true
//     "copy_scale":       false,                  // optional, default false
//     "control_space":    "ComponentSpace",       // optional - WorldSpace|ComponentSpace|ParentBoneSpace|BoneSpace
//     "node_offset_x":    -250,                   // optional offset from Output Pose
//     "node_offset_y":    150
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddCopyBone(
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

	FString AssetPath, SourceBone, TargetBone;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("source_bone"),  SourceBone);
	Body->TryGetStringField(TEXT("target_bone"),  TargetBone);
	if (AssetPath.IsEmpty() || SourceBone.IsEmpty() || TargetBone.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path', 'source_bone', and 'target_bone' are required")));
		return true;
	}

	bool bCopyTranslation = true, bCopyRotation = true, bCopyScale = false;
	FString ControlSpaceStr = TEXT("ComponentSpace");
	double  OffsetX = -250.0, OffsetY = 150.0;
	Body->TryGetBoolField  (TEXT("copy_translation"), bCopyTranslation);
	Body->TryGetBoolField  (TEXT("copy_rotation"),    bCopyRotation);
	Body->TryGetBoolField  (TEXT("copy_scale"),       bCopyScale);
	Body->TryGetStringField(TEXT("control_space"),    ControlSpaceStr);
	Body->TryGetNumberField(TEXT("node_offset_x"),    OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"),    OffsetY);

	const EBoneControlSpace ControlSpace = ParseBoneControlSpace(ControlSpaceStr, BCS_ComponentSpace);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_copy_bone bp='%s' src='%s' dst='%s' loc=%d rot=%d scale=%d space='%s'"),
		*AssetPath, *SourceBone, *TargetBone,
		bCopyTranslation ? 1 : 0, bCopyRotation ? 1 : 0, bCopyScale ? 1 : 0, *ControlSpaceStr);

	const FString CapturedAssetPath = AssetPath;
	const FString CapturedSrc       = SourceBone;
	const FString CapturedDst       = TargetBone;
	const bool    CapTranslation = bCopyTranslation, CapRotation = bCopyRotation, CapScale = bCopyScale;
	const int32   CapOffsetX = (int32)OffsetX, CapOffsetY = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapturedSrc, CapturedDst, CapTranslation, CapRotation, CapScale,
		 ControlSpace, CapOffsetX, CapOffsetY]()
	{
		FAnimGraphContext Ctx; FString Err;
		if (!ResolveAnimGraphContext(CapturedAssetPath, Ctx, Err))
		{
			Callback(JsonError(404, Err));
			return;
		}

		UAnimGraphNode_CopyBone* CopyBoneNode = NewObject<UAnimGraphNode_CopyBone>(Ctx.Graph);
		CopyBoneNode->CreateNewGuid();
		CopyBoneNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		CopyBoneNode->NodePosX = Ctx.RootNode->NodePosX + CapOffsetX;
		CopyBoneNode->NodePosY = Ctx.RootNode->NodePosY + CapOffsetY;
		Ctx.Graph->AddNode(CopyBoneNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		CopyBoneNode->AllocateDefaultPins();

		CopyBoneNode->Node.SourceBone.BoneName = FName(*CapturedSrc);
		CopyBoneNode->Node.TargetBone.BoneName = FName(*CapturedDst);
		CopyBoneNode->Node.bCopyTranslation    = CapTranslation;
		CopyBoneNode->Node.bCopyRotation       = CapRotation;
		CopyBoneNode->Node.bCopyScale          = CapScale;
		CopyBoneNode->Node.ControlSpace        = ControlSpace;

		UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
		FindPosePins(CopyBoneNode, InPin, OutPin);
		if (!InPin || !OutPin)
		{
			Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the Copy Bone node")));
			return;
		}

		FString UpstreamName;
		const bool bSpliced = SpliceBeforeOutputPose(Ctx.Graph, Ctx.RootInputPin, InPin, OutPin, UpstreamName);

		FKismetEditorUtilities::CompileBlueprint(Ctx.AnimBP);
		Ctx.AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_copy_bone: %s -> %s (spliced_upstream=%d, was='%s')"),
			*CapturedSrc, *CapturedDst, bSpliced ? 1 : 0, *UpstreamName);

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       Ctx.AnimBP->GetPathName());
		Resp->SetStringField(TEXT("source_bone"),      CapturedSrc);
		Resp->SetStringField(TEXT("target_bone"),      CapturedDst);
		Resp->SetStringField(TEXT("node_id"),          CopyBoneNode->NodeGuid.ToString());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSpliced);
		if (!UpstreamName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamName);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_hand_ik_retargeting
//
// Insert a Hand IK Retargeting skeletal control node before Output Pose.
// Used to retarget IK hand bones to FK hand bone positions so characters of
// different proportions can grip the same prop. Per-axis alpha defaults to (1,1,1).
//
// Body:
//   {
//     "anim_bp_path": "/Game/Path/ABP_X",       // required
//     "right_hand_fk": "hand_r",                 // required
//     "left_hand_fk":  "hand_l",                 // required
//     "right_hand_ik": "ik_hand_r",              // required
//     "left_hand_ik":  "ik_hand_l",              // required
//     "ik_bones_to_move": ["ik_hand_gun"],       // optional - array of bones the node will move
//     "hand_fk_weight": 0.5,                     // optional, default 0.5 (equal weight)
//     "per_axis_alpha": [1, 1, 1],               // optional, default [1,1,1]
//     "alpha_curve_name": "disable_hand_ik_retargeting", // optional
//     "alpha_scale": -1.0,                       // optional, default 1.0
//     "alpha_bias":  1.0,                        // optional, default 0.0
//     "node_offset_x": -250,
//     "node_offset_y": -150
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddHandIKRetargeting(
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

	FString AssetPath, RightFK, LeftFK, RightIK, LeftIK;
	Body->TryGetStringField(TEXT("anim_bp_path"),   AssetPath);
	Body->TryGetStringField(TEXT("right_hand_fk"),  RightFK);
	Body->TryGetStringField(TEXT("left_hand_fk"),   LeftFK);
	Body->TryGetStringField(TEXT("right_hand_ik"),  RightIK);
	Body->TryGetStringField(TEXT("left_hand_ik"),   LeftIK);
	if (AssetPath.IsEmpty() || RightFK.IsEmpty() || LeftFK.IsEmpty() || RightIK.IsEmpty() || LeftIK.IsEmpty())
	{
		Callback(JsonError(400, TEXT("anim_bp_path, right_hand_fk, left_hand_fk, right_hand_ik, left_hand_ik are required")));
		return true;
	}

	TArray<FString> IKBonesToMove;
	const TArray<TSharedPtr<FJsonValue>>* IKBonesArr = nullptr;
	if (Body->TryGetArrayField(TEXT("ik_bones_to_move"), IKBonesArr) && IKBonesArr)
	{
		for (const auto& V : *IKBonesArr) { IKBonesToMove.Add(V->AsString()); }
	}

	double HandFKWeight = 0.5;
	Body->TryGetNumberField(TEXT("hand_fk_weight"), HandFKWeight);

	double AxX = 1.0, AxY = 1.0, AxZ = 1.0;
	{
		const TArray<TSharedPtr<FJsonValue>>* AxArr = nullptr;
		if (Body->TryGetArrayField(TEXT("per_axis_alpha"), AxArr) && AxArr && AxArr->Num() >= 3)
		{
			AxX = (*AxArr)[0]->AsNumber();
			AxY = (*AxArr)[1]->AsNumber();
			AxZ = (*AxArr)[2]->AsNumber();
		}
	}

	FString AlphaCurveName;
	double  AlphaScale = 1.0, AlphaBias = 0.0;
	Body->TryGetStringField(TEXT("alpha_curve_name"), AlphaCurveName);
	Body->TryGetNumberField(TEXT("alpha_scale"),       AlphaScale);
	Body->TryGetNumberField(TEXT("alpha_bias"),        AlphaBias);

	double OffsetX = -250.0, OffsetY = -150.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_hand_ik_retargeting bp='%s' R(FK='%s' IK='%s') L(FK='%s' IK='%s') ikbones=%d"),
		*AssetPath, *RightFK, *RightIK, *LeftFK, *LeftIK, IKBonesToMove.Num());

	const FString CapturedAssetPath = AssetPath;
	const FString CapRightFK = RightFK, CapLeftFK = LeftFK, CapRightIK = RightIK, CapLeftIK = LeftIK;
	const TArray<FString> CapIKBones = IKBonesToMove;
	const float CapHandFKWeight = (float)HandFKWeight;
	const FVector CapPerAxis(AxX, AxY, AxZ);
	const FString CapAlphaCurveName = AlphaCurveName;
	const float CapAlphaScale = (float)AlphaScale, CapAlphaBias = (float)AlphaBias;
	const int32 CapOffsetX = (int32)OffsetX, CapOffsetY = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapRightFK, CapLeftFK, CapRightIK, CapLeftIK, CapIKBones,
		 CapHandFKWeight, CapPerAxis, CapAlphaCurveName, CapAlphaScale, CapAlphaBias,
		 CapOffsetX, CapOffsetY]()
	{
		FAnimGraphContext Ctx; FString Err;
		if (!ResolveAnimGraphContext(CapturedAssetPath, Ctx, Err))
		{
			Callback(JsonError(404, Err));
			return;
		}

		UAnimGraphNode_HandIKRetargeting* IKRetNode = NewObject<UAnimGraphNode_HandIKRetargeting>(Ctx.Graph);
		IKRetNode->CreateNewGuid();
		IKRetNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		IKRetNode->NodePosX = Ctx.RootNode->NodePosX + CapOffsetX;
		IKRetNode->NodePosY = Ctx.RootNode->NodePosY + CapOffsetY;
		Ctx.Graph->AddNode(IKRetNode, false, false);
		IKRetNode->AllocateDefaultPins();

		IKRetNode->Node.RightHandFK.BoneName = FName(*CapRightFK);
		IKRetNode->Node.LeftHandFK.BoneName  = FName(*CapLeftFK);
		IKRetNode->Node.RightHandIK.BoneName = FName(*CapRightIK);
		IKRetNode->Node.LeftHandIK.BoneName  = FName(*CapLeftIK);
		IKRetNode->Node.HandFKWeight         = CapHandFKWeight;
		IKRetNode->Node.PerAxisAlpha         = CapPerAxis;

		IKRetNode->Node.IKBonesToMove.Empty();
		for (const FString& BoneName : CapIKBones)
		{
			FBoneReference Ref;
			Ref.BoneName = FName(*BoneName);
			IKRetNode->Node.IKBonesToMove.Add(Ref);
		}

		// Alpha config: switch to Curve input if a curve name was supplied; otherwise leave as Float.
		if (!CapAlphaCurveName.IsEmpty())
		{
			IKRetNode->Node.AlphaInputType = EAnimAlphaInputType::Curve;
			IKRetNode->Node.AlphaCurveName = FName(*CapAlphaCurveName);
			IKRetNode->Node.AlphaScaleBias.Scale = CapAlphaScale;
			IKRetNode->Node.AlphaScaleBias.Bias  = CapAlphaBias;
		}

		UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
		FindPosePins(IKRetNode, InPin, OutPin);
		if (!InPin || !OutPin)
		{
			Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the Hand IK Retargeting node")));
			return;
		}

		FString UpstreamName;
		const bool bSpliced = SpliceBeforeOutputPose(Ctx.Graph, Ctx.RootInputPin, InPin, OutPin, UpstreamName);

		FKismetEditorUtilities::CompileBlueprint(Ctx.AnimBP);
		Ctx.AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_hand_ik_retargeting: spliced_upstream=%d was='%s' alpha_curve='%s'"),
			bSpliced ? 1 : 0, *UpstreamName, *CapAlphaCurveName);

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       Ctx.AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          IKRetNode->NodeGuid.ToString());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSpliced);
		if (!UpstreamName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamName);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_layered_bone_blend
//
// Insert a Layered Bone Blend node into an AnimBP's AnimGraph and (optionally)
// splice the existing upstream pose source into the new node's BasePose.
// Blend Pose 0 (and any additional blend poses) are LEFT UNCONNECTED — the
// caller is expected to feed them via /bp/connect_pins or by adding a
// SequencePlayer with /editor/anim/add_sequence_player and connecting that.
//
// Body:
//   {
//     "anim_bp_path":   "/Game/Path/ABP_X",            // required
//     "graph":          "AnimGraph",                    // optional, default "AnimGraph"
//     "branch_filters": [                               // one entry per blend pose (BranchFilter mode)
//       { "bone_name": "spine_03", "blend_depth": 1 }
//     ],
//     "blend_weights":  [1.0],                          // optional, one per blend pose; default all 1.0
//     "blend_mode":     "BranchFilter",                 // optional - BranchFilter|BoneMask. Default BranchFilter.
//     "alpha":          1.0,                            // reserved (FAnimNode_LayeredBoneBlend has no plain Alpha)
//     "splice_before_output": true,                     // optional, default true
//     "node_offset_x":  -250,                           // optional offset from Output Pose
//     "node_offset_y":  -250
//   }
// ============================================================================

namespace
{
	/** Translate friendly enum string to ELayeredBoneBlendMode. Defaults to BranchFilter. */
	static ELayeredBoneBlendMode ParseLayeredBoneBlendMode(const FString& Name)
	{
		if (Name.Equals(TEXT("BlendMask"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BoneMask"),  ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Mask"),      ESearchCase::IgnoreCase))
		{
			return ELayeredBoneBlendMode::BlendMask;
		}
		return ELayeredBoneBlendMode::BranchFilter;
	}

	/** Find the BasePose input pin and the output pose pin on a LayeredBoneBlend node (FPoseLink, not FComponentSpacePoseLink). */
	static void FindLayeredBoneBlendBasePins(UAnimGraphNode_Base* Node, UEdGraphPin*& OutBaseIn, UEdGraphPin*& OutPoseOut)
	{
		OutBaseIn = nullptr; OutPoseOut = nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (!P) continue;
			const bool bIsPoseStruct =
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct();
			if (P->Direction == EGPD_Input && (P->PinName == TEXT("BasePose") || (bIsPoseStruct && !OutBaseIn)))
			{
				// Prefer an exact "BasePose" match, otherwise the first input pose pin.
				if (P->PinName == TEXT("BasePose")) { OutBaseIn = P; }
				else if (!OutBaseIn) { OutBaseIn = P; }
			}
			else if (P->Direction == EGPD_Output && bIsPoseStruct && !OutPoseOut)
			{
				OutPoseOut = P;
			}
		}
	}
}

bool FNGGHttpServer::HandleAnimAddLayeredBoneBlend(
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

	FString AssetPath;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' is required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	FString BlendModeStr = TEXT("BranchFilter");
	Body->TryGetStringField(TEXT("blend_mode"), BlendModeStr);

	bool bSpliceBeforeOutput = true;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSpliceBeforeOutput);

	double Alpha = 1.0;
	Body->TryGetNumberField(TEXT("alpha"), Alpha); // reserved; not currently mapped to a node field

	double OffsetX = -250.0, OffsetY = -250.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	// Parse branch_filters: array of { bone_name, blend_depth }.
	struct FBranchFilterIn { FString Bone; int32 Depth = 1; };
	TArray<FBranchFilterIn> BranchFilters;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("branch_filters"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !Obj->IsValid()) continue;
				FBranchFilterIn BF;
				(*Obj)->TryGetStringField(TEXT("bone_name"), BF.Bone);
				double D = 1.0;
				if ((*Obj)->TryGetNumberField(TEXT("blend_depth"), D)) { BF.Depth = (int32)D; }
				BranchFilters.Add(BF);
			}
		}
	}

	// Parse blend_weights (optional). If shorter than branch_filters we'll pad with 1.0.
	TArray<double> Weights;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("blend_weights"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr) { Weights.Add(V->AsNumber()); }
		}
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_layered_bone_blend bp='%s' graph='%s' mode='%s' filters=%d weights=%d splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BlendModeStr, BranchFilters.Num(), Weights.Num(),
		bSpliceBeforeOutput ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath = AssetPath;
	const FString CapGraphName = GraphName;
	const ELayeredBoneBlendMode CapBlendMode = ParseLayeredBoneBlendMode(BlendModeStr);
	const bool   CapSplice = bSpliceBeforeOutput;
	const int32  CapOffsetX = (int32)OffsetX, CapOffsetY = (int32)OffsetY;
	const TArray<FBranchFilterIn> CapBranchFilters = BranchFilters;
	const TArray<double> CapWeights = Weights;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBlendMode, CapSplice,
		 CapOffsetX, CapOffsetY, CapBranchFilters, CapWeights]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 3. Locate the Output Pose root node and its input pin (used for splicing)
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 4. Spawn the LayeredBoneBlend node
		UAnimGraphNode_LayeredBoneBlend* LBBNode = NewObject<UAnimGraphNode_LayeredBoneBlend>(AnimGraph);
		LBBNode->CreateNewGuid();
		LBBNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		LBBNode->NodePosX = RootNode->NodePosX + CapOffsetX;
		LBBNode->NodePosY = RootNode->NodePosY + CapOffsetY;

		// 5. Configure the runtime FAnimNode_LayeredBoneBlend BEFORE AllocateDefaultPins
		// so the per-blend-pose pins are created in the right shape.
		// The default constructor (UAnimGraphNode_LayeredBoneBlend) calls Node.AddFirstPose(),
		// so the array sizes already start at 1 (BlendWeights, BlendPoses, LayerSetup).
		LBBNode->Node.BlendMode = CapBlendMode;

		const int32 RequestedPoses = FMath::Max(1, CapBranchFilters.Num());
		// Grow arrays to RequestedPoses (constructor seeded 1)
		while (LBBNode->Node.BlendPoses.Num() < RequestedPoses)
		{
			LBBNode->Node.AddPose();
		}
		// Defensive: if branch_filters had MORE entries than RequestedPoses (shouldn't happen)
		// or fewer, ensure the layer setup container is the right size.
		LBBNode->Node.BlendWeights.SetNum(RequestedPoses);
		if (CapBlendMode == ELayeredBoneBlendMode::BranchFilter)
		{
			LBBNode->Node.LayerSetup.SetNum(RequestedPoses);
		}

		// Apply branch filters (one BranchFilter per blend pose entry, matching the tutorial pattern).
		if (CapBlendMode == ELayeredBoneBlendMode::BranchFilter)
		{
			for (int32 i = 0; i < CapBranchFilters.Num() && i < LBBNode->Node.LayerSetup.Num(); ++i)
			{
				FInputBlendPose& IBP = LBBNode->Node.LayerSetup[i];
				IBP.BranchFilters.Reset();
				FBranchFilter BF;
				BF.BoneName   = FName(*CapBranchFilters[i].Bone);
				BF.BlendDepth = CapBranchFilters[i].Depth;
				IBP.BranchFilters.Add(BF);
			}
		}

		// Apply blend weights, padding with 1.0 if the array was shorter than RequestedPoses.
		for (int32 i = 0; i < LBBNode->Node.BlendWeights.Num(); ++i)
		{
			LBBNode->Node.BlendWeights[i] = (i < CapWeights.Num()) ? (float)CapWeights[i] : 1.0f;
		}

		AnimGraph->AddNode(LBBNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		LBBNode->AllocateDefaultPins();

		// 6. Splice: locate BasePose input + output pose pin
		UEdGraphPin* BaseInPin = nullptr;
		UEdGraphPin* OutPosePin = nullptr;
		FindLayeredBoneBlendBasePins(LBBNode, BaseInPin, OutPosePin);
		if (!BaseInPin || !OutPosePin)
		{
			Callback(JsonError(500, TEXT("Could not find BasePose / output pose pins on the Layered Bone Blend node")));
			return;
		}

		const UEdGraphSchema* Schema = AnimGraph->GetSchema();
		if (!Schema)
		{
			Callback(JsonError(500, TEXT("AnimGraph has no schema")));
			return;
		}

		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		UEdGraphPin* UpstreamPin = nullptr;
		if (RootInputPin->LinkedTo.Num() > 0)
		{
			UpstreamPin = RootInputPin->LinkedTo[0];
			if (UpstreamPin && UpstreamPin->GetOwningNode())
			{
				UpstreamNodeName = UpstreamPin->GetOwningNode()->GetName();
			}
		}

		if (CapSplice)
		{
			if (UpstreamPin)
			{
				RootInputPin->BreakAllPinLinks();
				Schema->TryCreateConnection(UpstreamPin, BaseInPin);
				bSplicedUpstream = true;
			}
			Schema->TryCreateConnection(OutPosePin, RootInputPin);
		}

		// 7. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_layered_bone_blend: added LBB (poses=%d) spliced=%d upstream_was='%s'"),
			LBBNode->Node.BlendPoses.Num(), bSplicedUpstream ? 1 : 0, *UpstreamNodeName);

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          LBBNode->NodeGuid.ToString());
		Resp->SetNumberField(TEXT("blend_pose_count"), LBBNode->Node.BlendPoses.Num());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);

		// Tell the caller which pose pins still need wiring on this node.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (int32 i = 0; i < LBBNode->Node.BlendPoses.Num(); ++i)
		{
			UnboundPins.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("BlendPose_%d"), i)));
		}
		Resp->SetArrayField(TEXT("unbound_pins"), UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_sequence_player
//
// Spawn a UAnimGraphNode_SequencePlayer in an AnimBP's AnimGraph, configured
// to play a specific UAnimSequence. By default this does NOT splice into the
// pose chain — it's a leaf the caller wires up later via /bp/connect_pins
// (typically into a LayeredBoneBlend's BlendPose input).
//
// Body:
//   {
//     "anim_bp_path":   "/Game/Path/ABP_X",                    // required
//     "graph":          "AnimGraph",                            // optional, default "AnimGraph"
//     "sequence":       "/Game/Anims/MM_Idle.MM_Idle",          // required - hard ref to UAnimSequence
//     "loop":           true,                                   // optional, default true
//     "play_rate":      1.0,                                    // optional, default 1.0
//     "start_position": 0.0,                                    // optional, default 0.0
//     "node_offset_x":  -500,                                   // optional offset from Output Pose
//     "node_offset_y":  -250
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddSequencePlayer(
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

	FString AssetPath, SequencePath;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("sequence"),     SequencePath);
	if (AssetPath.IsEmpty() || SequencePath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'sequence' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	bool   bLoop         = true;
	double PlayRate      = 1.0;
	double StartPosition = 0.0;
	double OffsetX       = -500.0;
	double OffsetY       = -250.0;
	Body->TryGetBoolField  (TEXT("loop"),           bLoop);
	Body->TryGetNumberField(TEXT("play_rate"),      PlayRate);
	Body->TryGetNumberField(TEXT("start_position"), StartPosition);
	Body->TryGetNumberField(TEXT("node_offset_x"),  OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"),  OffsetY);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_sequence_player bp='%s' graph='%s' seq='%s' loop=%d rate=%.2f start=%.2f off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *SequencePath, bLoop ? 1 : 0, PlayRate, StartPosition, OffsetX, OffsetY);

	const FString CapAssetPath  = AssetPath;
	const FString CapGraphName  = GraphName;
	const FString CapSequence   = SequencePath;
	const bool    CapLoop       = bLoop;
	const float   CapPlayRate   = (float)PlayRate;
	const float   CapStartPos   = (float)StartPosition;
	const int32   CapOffsetX    = (int32)OffsetX;
	const int32   CapOffsetY    = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapSequence, CapLoop, CapPlayRate, CapStartPos,
		 CapOffsetX, CapOffsetY]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Resolve the AnimSequence asset
		UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *CapSequence);
		if (!Sequence)
		{
			const FString AltPath = CapSequence + TEXT(".") + FPaths::GetBaseFilename(CapSequence);
			Sequence = LoadObject<UAnimSequence>(nullptr, *AltPath);
		}
		if (!Sequence)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimSequence not found: %s"), *CapSequence)));
			return;
		}

		// 3. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 4. Locate the Output Pose node so we can position the new node relative to it
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		const int32 BaseX = RootNode ? RootNode->NodePosX : 0;
		const int32 BaseY = RootNode ? RootNode->NodePosY : 0;

		// 5. Spawn the SequencePlayer node and configure its runtime FAnimNode_SequencePlayer.
		// Setters live on the base FAnimNode_SequencePlayerBase and forward to the right backing field.
		UAnimGraphNode_SequencePlayer* SPNode = NewObject<UAnimGraphNode_SequencePlayer>(AnimGraph);
		SPNode->CreateNewGuid();
		SPNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		SPNode->NodePosX = BaseX + CapOffsetX;
		SPNode->NodePosY = BaseY + CapOffsetY;
		AnimGraph->AddNode(SPNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		SPNode->AllocateDefaultPins();

		SPNode->Node.SetSequence     (Sequence);
		SPNode->Node.SetLoopAnimation(CapLoop);
		SPNode->Node.SetPlayRate     (CapPlayRate);
		SPNode->Node.SetStartPosition(CapStartPos);

		// 6. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_sequence_player: spawned SequencePlayer for '%s' (loop=%d rate=%.2f start=%.2f)"),
			*Sequence->GetPathName(), CapLoop ? 1 : 0, CapPlayRate, CapStartPos);

		// 7. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),         true);
		Resp->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),    SPNode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("sequence"),   Sequence->GetPathName());
		Resp->SetBoolField  (TEXT("loop"),       CapLoop);
		Resp->SetNumberField(TEXT("play_rate"),  CapPlayRate);
		Resp->SetNumberField(TEXT("start_position"), CapStartPos);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_modify_bone
//
// Insert a Transform (Modify) Bone (UAnimGraphNode_ModifyBone) into an AnimBP's
// AnimGraph. Optionally splice it before the Output Pose so it modifies whatever
// pose chain is currently feeding the root.
//
// Use case for ThrowBomb: rotate spine_03 in component space (RotationMode=Add)
// so the upper body follows the player's aim direction — the whole arm + the
// rifle pose rotate together, which makes the launcher naturally point at the
// crosshair target. The Rotation pin is left unwired so the AnimBP can drive it
// from a computed FRotator each frame (e.g. via BlueprintThreadSafeUpdateAnimation).
//
// Body:
//   {
//     "anim_bp_path":      "/Game/Path/ABP_X",        // required
//     "graph":             "AnimGraph",                // optional, default "AnimGraph"
//     "bone_name":         "spine_03",                 // required - bone to modify
//     "translation_mode":  "Ignore",                   // Ignore|Add|Replace, default Ignore
//     "rotation_mode":     "Add",                      // Ignore|Add|Replace, default Add
//     "scale_mode":        "Ignore",                   // default Ignore
//     "translation_space": "ComponentSpace",           // WorldSpace|ComponentSpace|ParentBoneSpace|BoneSpace, default ComponentSpace
//     "rotation_space":    "ComponentSpace",           // default ComponentSpace
//     "scale_space":       "ComponentSpace",           // default ComponentSpace
//     "translation":       [0, 0, 0],                  // default value baked into the node (overridden if pin is wired)
//     "rotation":          [0, 0, 0],                  // pitch, yaw, roll
//     "scale":             [1, 1, 1],
//     "alpha":             1.0,
//     "splice_before_output": false,                   // default false (caller wires)
//     "node_offset_x":     -250,
//     "node_offset_y":     0
//   }
//
// Response:
//   { "ok": true, "asset_path": "...", "node_id": "<guid>",
//     "bone": "spine_03", "spliced_upstream": bool, "upstream_was": "...",
//     "unbound_pins": ["Translation", "Rotation", "Scale", "Alpha"] }
// ============================================================================

namespace
{
	/** Translate "Ignore"/"Add"/"Replace" (or BMM_*) into EBoneModificationMode. */
	static EBoneModificationMode ParseBoneModificationMode(const FString& Name, EBoneModificationMode Default = BMM_Ignore)
	{
		if (Name.Equals(TEXT("Ignore"),     ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BMM_Ignore"), ESearchCase::IgnoreCase))   return BMM_Ignore;
		if (Name.Equals(TEXT("Replace"),         ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Replace Existing"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BMM_Replace"),     ESearchCase::IgnoreCase)) return BMM_Replace;
		if (Name.Equals(TEXT("Add"),               ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Additive"),          ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Add to Existing"),   ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("BMM_Additive"),      ESearchCase::IgnoreCase)) return BMM_Additive;
		return Default;
	}

	/** Parse [x,y,z] FVector from a JSON array field; returns Default on miss. */
	static FVector ParseJsonVec3(const TSharedPtr<FJsonObject>& Body, const TCHAR* Field, const FVector& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Body->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3) return Default;
		return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	}

	/** Parse [pitch,yaw,roll] FRotator from a JSON array field; returns Default on miss. */
	static FRotator ParseJsonRot3(const TSharedPtr<FJsonObject>& Body, const TCHAR* Field, const FRotator& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Body->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() < 3) return Default;
		return FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
	}
}

bool FNGGHttpServer::HandleAnimAddModifyBone(
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

	FString AssetPath, BoneName;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("bone_name"),    BoneName);
	if (AssetPath.IsEmpty() || BoneName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'bone_name' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	FString TranslationModeStr = TEXT("Ignore");
	FString RotationModeStr    = TEXT("Add");
	FString ScaleModeStr       = TEXT("Ignore");
	Body->TryGetStringField(TEXT("translation_mode"), TranslationModeStr);
	Body->TryGetStringField(TEXT("rotation_mode"),    RotationModeStr);
	Body->TryGetStringField(TEXT("scale_mode"),       ScaleModeStr);

	FString TranslationSpaceStr = TEXT("ComponentSpace");
	FString RotationSpaceStr    = TEXT("ComponentSpace");
	FString ScaleSpaceStr       = TEXT("ComponentSpace");
	Body->TryGetStringField(TEXT("translation_space"), TranslationSpaceStr);
	Body->TryGetStringField(TEXT("rotation_space"),    RotationSpaceStr);
	Body->TryGetStringField(TEXT("scale_space"),       ScaleSpaceStr);

	const FVector  Translation = ParseJsonVec3(Body, TEXT("translation"), FVector::ZeroVector);
	const FRotator Rotation    = ParseJsonRot3(Body, TEXT("rotation"),    FRotator::ZeroRotator);
	const FVector  Scale       = ParseJsonVec3(Body, TEXT("scale"),       FVector::OneVector);

	double AlphaIn = 1.0;
	Body->TryGetNumberField(TEXT("alpha"), AlphaIn);

	bool bSplice = false;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSplice);

	double OffsetX = -250.0, OffsetY = 0.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	const EBoneModificationMode TranslationMode = ParseBoneModificationMode(TranslationModeStr, BMM_Ignore);
	const EBoneModificationMode RotationMode    = ParseBoneModificationMode(RotationModeStr,    BMM_Additive);
	const EBoneModificationMode ScaleMode       = ParseBoneModificationMode(ScaleModeStr,       BMM_Ignore);

	const EBoneControlSpace TranslationSpace = ParseBoneControlSpace(TranslationSpaceStr, BCS_ComponentSpace);
	const EBoneControlSpace RotationSpace    = ParseBoneControlSpace(RotationSpaceStr,    BCS_ComponentSpace);
	const EBoneControlSpace ScaleSpace       = ParseBoneControlSpace(ScaleSpaceStr,       BCS_ComponentSpace);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_modify_bone bp='%s' graph='%s' bone='%s' tmode='%s' rmode='%s' smode='%s' "
			 "tspace='%s' rspace='%s' sspace='%s' splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BoneName,
		*TranslationModeStr, *RotationModeStr, *ScaleModeStr,
		*TranslationSpaceStr, *RotationSpaceStr, *ScaleSpaceStr,
		bSplice ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath  = AssetPath;
	const FString CapGraphName  = GraphName;
	const FString CapBoneName   = BoneName;
	const FVector  CapTranslation = Translation;
	const FRotator CapRotation    = Rotation;
	const FVector  CapScale       = Scale;
	const float   CapAlpha      = (float)AlphaIn;
	const bool    CapSplice     = bSplice;
	const int32   CapOffsetX    = (int32)OffsetX;
	const int32   CapOffsetY    = (int32)OffsetY;
	// Modes / spaces are POD enums — capture by value.

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBoneName,
		 CapTranslation, CapRotation, CapScale, CapAlpha, CapSplice,
		 CapOffsetX, CapOffsetY,
		 TranslationMode, RotationMode, ScaleMode,
		 TranslationSpace, RotationSpace, ScaleSpace]()
	{
		// 1. Load the AnimBlueprint
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 3. Locate the Output Pose root node (used for positioning + optional splice)
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 4. Spawn the ModifyBone node, configure runtime fields, then allocate pins.
		// Setting Node fields BEFORE AllocateDefaultPins is the safe pattern (matches
		// how LayeredBoneBlend / TwoBoneIK seed their per-blend / per-bone arrays).
		UAnimGraphNode_ModifyBone* MBNode = NewObject<UAnimGraphNode_ModifyBone>(AnimGraph);
		MBNode->CreateNewGuid();
		MBNode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		MBNode->NodePosX = RootNode->NodePosX + CapOffsetX;
		MBNode->NodePosY = RootNode->NodePosY + CapOffsetY;

		MBNode->Node.BoneToModify.BoneName = FName(*CapBoneName);
		MBNode->Node.TranslationMode = TranslationMode;
		MBNode->Node.RotationMode    = RotationMode;
		MBNode->Node.ScaleMode       = ScaleMode;
		MBNode->Node.TranslationSpace = TranslationSpace;
		MBNode->Node.RotationSpace    = RotationSpace;
		MBNode->Node.ScaleSpace       = ScaleSpace;
		MBNode->Node.Translation = CapTranslation;
		MBNode->Node.Rotation    = CapRotation;
		MBNode->Node.Scale       = CapScale;
		// Alpha lives on the SkeletalControlBase; default of 1.0 is fine for a wired-pin workflow.
		MBNode->Node.Alpha       = CapAlpha;

		AnimGraph->AddNode(MBNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		MBNode->AllocateDefaultPins();

		// 5. Optional splice: insert MBNode between (whatever feeds RootInputPin) and RootInputPin.
		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		{
			// FindPosePins handles SkeletalControlBase-style ComponentPose pins.
			UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
			FindPosePins(MBNode, InPin, OutPin);
			if (CapSplice)
			{
				if (!InPin || !OutPin)
				{
					Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the Modify Bone node — cannot splice")));
					return;
				}
				bSplicedUpstream = SpliceBeforeOutputPose(AnimGraph, RootInputPin, InPin, OutPin, UpstreamNodeName);
			}
		}

		// 6. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		// 7. Walk the spawned node's pins so we report the *actual* runtime pin names
		// the caller will need for /bp/connect_pins. ModifyBone declares Translation,
		// Rotation, and Scale as PinShownByDefault, plus the SkeletalControlBase Alpha
		// system. (We hit a name mismatch with LayeredBoneBlend last time — better to
		// surface real names than guess.) We list every input pin that is NOT a pose
		// link and not already connected.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (UEdGraphPin* P : MBNode->Pins)
		{
			if (!P || P->Direction != EGPD_Input) continue;
			const bool bIsPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose");
			if (bIsPose) continue;
			if (P->LinkedTo.Num() > 0) continue; // skip already-connected pins
			UnboundPins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_modify_bone: added MB on bone='%s' (spliced=%d upstream='%s' unbound=%d)"),
			*CapBoneName, bSplicedUpstream ? 1 : 0, *UpstreamNodeName, UnboundPins.Num());

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          MBNode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("bone"),             CapBoneName);
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);
		Resp->SetArrayField (TEXT("unbound_pins"),     UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_look_at
//
// Insert a LookAt skeletal control node (UAnimGraphNode_LookAt /
// FAnimNode_LookAt) into an AnimBP's AnimGraph and (optionally) splice it
// before the Output Pose. Unlike a manual ModifyBone+Rotation approach, LookAt
// solves "given a bone, a local axis, and a world target, compute the rotation
// that aligns that axis through the target" — which correctly accounts for the
// spatial offset between the controlled bone (e.g. spine_03) and the gun.
//
// The LookAtLocation pin is left UNCONNECTED so the AnimBP can drive it from
// BlueprintThreadSafeUpdateAnimation each frame (read GetAimWorldTarget() into
// an AnimBP variable, then bp_connect_pins it to LookAtLocation).
//
// Body:
//   {
//     "anim_bp_path":           "/Game/Path/ABP_X",   // required
//     "graph":                  "AnimGraph",          // optional, default "AnimGraph"
//     "bone_to_modify":         "spine_03",           // required
//     "look_at_axis":           [1,0,0],              // local axis on bone_to_modify; default [1,0,0]
//     "look_at_axis_local":     true,                 // optional, default true
//     "look_at_target_bone":    "",                   // optional - if set, LookAt follows that bone
//     "look_at_socket":         "",                   // optional - socket on look_at_target_bone (mutually exclusive with bone)
//     "look_at_location":       [0,0,0],              // world-space target (when no target bone). Bind via bp_connect_pins.
//     "look_at_location_space": "WorldSpace",         // World|Component|ParentBone|Bone, default WorldSpace
//     "look_at_clamp":          0.0,                  // 0 = no clamp; >0 = max angle in degrees
//     "interpolation_type":     "Linear",             // None|Linear|Cubic|Sinusoidal|EaseInOutExponent2..5
//     "interpolation_time":     0.1,                  // smoothing time (seconds)
//     "alpha":                  1.0,
//     "splice_before_output":   true,                 // default true
//     "node_offset_x":          -250,
//     "node_offset_y":          0
//   }
//
// Response:
//   { "ok": true, "asset_path": "...", "node_id": "<guid>",
//     "bone": "spine_03", "spliced_upstream": bool, "upstream_was": "...",
//     "unbound_pins": [...] }   // actual runtime input pin names
// ============================================================================

namespace
{
	/** Translate "Linear"/"Cubic"/"Sinusoidal"/"EaseInOutExponent[2-5]"/"None" into EInterpolationBlend::Type. */
	static EInterpolationBlend::Type ParseInterpolationBlend(const FString& Name, EInterpolationBlend::Type Default = EInterpolationBlend::Linear)
	{
		// "None" maps to Linear with 0 interpolation_time at the call site; here we treat it as Linear.
		if (Name.Equals(TEXT("None"),     ESearchCase::IgnoreCase))           return EInterpolationBlend::Linear;
		if (Name.Equals(TEXT("Linear"),   ESearchCase::IgnoreCase))           return EInterpolationBlend::Linear;
		if (Name.Equals(TEXT("Cubic"),    ESearchCase::IgnoreCase))           return EInterpolationBlend::Cubic;
		if (Name.Equals(TEXT("Sinusoidal"), ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("Sine"),     ESearchCase::IgnoreCase))           return EInterpolationBlend::Sinusoidal;
		if (Name.Equals(TEXT("EaseInOut"),         ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("EaseInOutExponent2"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent2;
		if (Name.Equals(TEXT("EaseInOutExponent3"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent3;
		if (Name.Equals(TEXT("EaseInOutExponent4"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent4;
		if (Name.Equals(TEXT("EaseInOutExponent5"), ESearchCase::IgnoreCase)) return EInterpolationBlend::EaseInOutExponent5;
		return Default;
	}
}

bool FNGGHttpServer::HandleAnimAddLookAt(
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

	FString AssetPath, BoneToModify;
	Body->TryGetStringField(TEXT("anim_bp_path"),    AssetPath);
	Body->TryGetStringField(TEXT("bone_to_modify"),  BoneToModify);
	if (AssetPath.IsEmpty() || BoneToModify.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'bone_to_modify' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	// Look-at axis on the controlled bone. Default to [1,0,0]; UE5 mannequin spine
	// forward axis is typically [0,1,0] (Y) — caller should pass that explicitly when known.
	const FVector LookAtAxisVec = ParseJsonVec3(Body, TEXT("look_at_axis"), FVector(1.0, 0.0, 0.0));
	bool bLookAtAxisLocal = true;
	Body->TryGetBoolField(TEXT("look_at_axis_local"), bLookAtAxisLocal);

	// Optional target — bone name OR socket name. Mutually exclusive (socket wins if both set).
	FString LookAtTargetBone, LookAtSocket;
	Body->TryGetStringField(TEXT("look_at_target_bone"), LookAtTargetBone);
	Body->TryGetStringField(TEXT("look_at_socket"),      LookAtSocket);

	// World-space (or other-space) target offset. Most callers will leave this at zero
	// and bind the LookAtLocation pin via /bp/connect_pins to a Get of an AnimBP variable.
	const FVector LookAtLocation = ParseJsonVec3(Body, TEXT("look_at_location"), FVector::ZeroVector);
	FString LookAtLocationSpaceStr = TEXT("WorldSpace");
	Body->TryGetStringField(TEXT("look_at_location_space"), LookAtLocationSpaceStr);

	double LookAtClampIn       = 0.0;
	double InterpolationTimeIn = 0.1;
	double AlphaIn             = 1.0;
	Body->TryGetNumberField(TEXT("look_at_clamp"),      LookAtClampIn);
	Body->TryGetNumberField(TEXT("interpolation_time"), InterpolationTimeIn);
	Body->TryGetNumberField(TEXT("alpha"),              AlphaIn);

	FString InterpolationTypeStr = TEXT("Linear");
	Body->TryGetStringField(TEXT("interpolation_type"), InterpolationTypeStr);

	bool bSplice = true;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSplice);

	double OffsetX = -250.0, OffsetY = 0.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	const EInterpolationBlend::Type InterpolationType = ParseInterpolationBlend(InterpolationTypeStr, EInterpolationBlend::Linear);
	// LookAtLocation space — the AnimNode itself doesn't have a space enum (LookAtLocation
	// is plain world-space when no target bone is set, otherwise local to that bone). We
	// accept a string for caller-side clarity / future expansion but currently only log it.

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_look_at bp='%s' graph='%s' bone='%s' axis=[%.2f,%.2f,%.2f] axis_local=%d "
			 "target_bone='%s' socket='%s' loc_space='%s' clamp=%.1f interp='%s' time=%.3f alpha=%.2f splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BoneToModify,
		LookAtAxisVec.X, LookAtAxisVec.Y, LookAtAxisVec.Z, bLookAtAxisLocal ? 1 : 0,
		*LookAtTargetBone, *LookAtSocket, *LookAtLocationSpaceStr,
		LookAtClampIn, *InterpolationTypeStr, InterpolationTimeIn, AlphaIn,
		bSplice ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath          = AssetPath;
	const FString CapGraphName          = GraphName;
	const FString CapBoneToModify       = BoneToModify;
	const FVector CapLookAtAxisVec      = LookAtAxisVec;
	const bool    CapLookAtAxisLocal    = bLookAtAxisLocal;
	const FString CapLookAtTargetBone   = LookAtTargetBone;
	const FString CapLookAtSocket       = LookAtSocket;
	const FVector CapLookAtLocation     = LookAtLocation;
	const FString CapLookAtLocationSpace = LookAtLocationSpaceStr;
	const float   CapLookAtClamp        = (float)LookAtClampIn;
	const float   CapInterpolationTime  = (float)InterpolationTimeIn;
	const float   CapAlpha              = (float)AlphaIn;
	const bool    CapSplice             = bSplice;
	const int32   CapOffsetX            = (int32)OffsetX;
	const int32   CapOffsetY            = (int32)OffsetY;
	// InterpolationType is a POD enum — capture by value.

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBoneToModify,
		 CapLookAtAxisVec, CapLookAtAxisLocal,
		 CapLookAtTargetBone, CapLookAtSocket,
		 CapLookAtLocation, CapLookAtLocationSpace,
		 CapLookAtClamp, CapInterpolationTime, CapAlpha,
		 CapSplice, CapOffsetX, CapOffsetY,
		 InterpolationType]()
	{
		// 1. Load the AnimBlueprint (with .BaseFilename fallback)
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 3. Locate the Output Pose root node
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 4. Spawn the LookAt node, configure runtime fields, then allocate pins.
		// Setting Node fields BEFORE AllocateDefaultPins is the safe pattern (matches
		// LayeredBoneBlend / TwoBoneIK / ModifyBone seeding).
		UAnimGraphNode_LookAt* LANode = NewObject<UAnimGraphNode_LookAt>(AnimGraph);
		LANode->CreateNewGuid();
		LANode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		LANode->NodePosX = RootNode->NodePosX + CapOffsetX;
		LANode->NodePosY = RootNode->NodePosY + CapOffsetY;

		LANode->Node.BoneToModify.BoneName = FName(*CapBoneToModify);

		// FAxis: the runtime expects a normalized axis. The constructor normalizes; we
		// also assign bInLocalSpace explicitly.
		FAxis Axis;
		Axis.Axis = CapLookAtAxisVec.GetSafeNormal();
		Axis.bInLocalSpace = CapLookAtAxisLocal;
		LANode->Node.LookAt_Axis = Axis;

		// Target — socket wins over bone if both supplied, matching the FBoneSocketTarget
		// "bUseSocket" semantics.
		FBoneSocketTarget Target;
		if (!CapLookAtSocket.IsEmpty())
		{
			Target.bUseSocket = true;
			Target.SocketReference.SocketName = FName(*CapLookAtSocket);
			// If a target bone was also given it's stored on the BoneReference for completeness.
			if (!CapLookAtTargetBone.IsEmpty())
			{
				Target.BoneReference.BoneName = FName(*CapLookAtTargetBone);
			}
		}
		else if (!CapLookAtTargetBone.IsEmpty())
		{
			Target.bUseSocket = false;
			Target.BoneReference.BoneName = FName(*CapLookAtTargetBone);
		}
		else
		{
			// No target bone/socket → LookAtLocation is interpreted as world-space.
			Target.bUseSocket = false;
		}
		LANode->Node.LookAtTarget = Target;

		LANode->Node.LookAtLocation     = CapLookAtLocation;
		LANode->Node.LookAtClamp        = CapLookAtClamp;
		LANode->Node.InterpolationType  = InterpolationType;
		LANode->Node.InterpolationTime  = CapInterpolationTime;
		LANode->Node.Alpha              = CapAlpha;
		LANode->Node.bUseLookUpAxis     = false;

		AnimGraph->AddNode(LANode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		LANode->AllocateDefaultPins();

		// LookAtLocation, LookAtClamp, InterpolationType, InterpolationTime are declared as
		// PinHiddenByDefault on FAnimNode_LookAt — without flipping ShowPinForProperties they
		// can only be set via Details panel, not driven by graph wires. Force-show LookAtLocation
		// (the most common runtime-driven pin — the AnimBP feeds AimWorldTarget into it) and
		// reconstruct so the pin actually materializes.
		bool bReconstructed = false;
		for (FOptionalPinFromProperty& OptPin : LANode->ShowPinForProperties)
		{
			if (OptPin.PropertyName == TEXT("LookAtLocation") && !OptPin.bShowPin)
			{
				OptPin.bShowPin = true;
				bReconstructed = true;
			}
		}
		if (bReconstructed)
		{
			LANode->ReconstructNode();
		}

		// 5. Optional splice: insert LANode between (whatever feeds RootInputPin) and RootInputPin.
		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		{
			UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
			FindPosePins(LANode, InPin, OutPin);
			if (CapSplice)
			{
				if (!InPin || !OutPin)
				{
					Callback(JsonError(500, TEXT("Could not find ComponentPose pins on the LookAt node — cannot splice")));
					return;
				}
				bSplicedUpstream = SpliceBeforeOutputPose(AnimGraph, RootInputPin, InPin, OutPin, UpstreamNodeName);
			}
		}

		// 6. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		// 7. Walk the spawned node's pins so we report the *actual* runtime pin names.
		// LookAt declares LookAtLocation, InterpolationType, LookAtClamp, InterpolationTime,
		// InterpolationTriggerThreashold (sic) as PinHiddenByDefault — they only exist as pins
		// when the user un-hides them. But Alpha (from SkeletalControlBase) is always present.
		// We list every input pin that is NOT a pose link and not already connected.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (UEdGraphPin* P : LANode->Pins)
		{
			if (!P || P->Direction != EGPD_Input) continue;
			const bool bIsPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose");
			if (bIsPose) continue;
			if (P->LinkedTo.Num() > 0) continue;
			UnboundPins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_look_at: added LookAt on bone='%s' (spliced=%d upstream='%s' unbound=%d)"),
			*CapBoneToModify, bSplicedUpstream ? 1 : 0, *UpstreamNodeName, UnboundPins.Num());

		// 8. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          LANode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("bone"),             CapBoneToModify);
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);
		Resp->SetArrayField (TEXT("unbound_pins"),     UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/add_aim_offset_blend_space
//
// Spawn a UAnimGraphNode_RotationOffsetBlendSpace ("AimOffset Player") in an
// AnimBP's AnimGraph and (by default) splice it before the Output Pose so it
// applies an authored aim-offset blend space (UAimOffsetBlendSpace /
// UAimOffsetBlendSpace1D) on top of whatever pose currently feeds the root.
//
// This is the proper UE5 shooter-pattern aim system: the blend space supplies
// pre-authored "aim up / center / down" (and optionally left/right) per-bone
// rotation deltas. Driving it via a normalized pitch/yaw avoids the per-frame
// FRotator hacks that hand-rolled ModifyBone(spine_03) approaches need.
//
// The X / Y / Alpha pins are LEFT UNCONNECTED so the AnimBP can drive them
// from BlueprintThreadSafeUpdateAnimation (e.g. a normalized aim-pitch float
// from the character's AimRelativeRotation). For a 1D Y-only blend space
// (like AO_Rifle), only Y matters; X (and Z, if exposed) can stay at default.
//
// Body:
//   {
//     "anim_bp_path":         "/Game/Path/ABP_X",                         // required
//     "graph":                "AnimGraph",                                 // optional, default "AnimGraph"
//     "blend_space":          "/Game/.../AO_Rifle.AO_Rifle",               // required - hard ref to UAimOffsetBlendSpace[1D]
//     "alpha":                1.0,                                          // baked default; pin can override
//     "splice_before_output": true,                                         // default true
//     "node_offset_x":        -250,                                         // default
//     "node_offset_y":        0
//   }
//
// Response:
//   { "ok": true, "asset_path": "...", "node_id": "<guid>",
//     "blend_space": "...", "spliced_upstream": bool, "upstream_was": "...",
//     "unbound_pins": ["X","Y","Alpha", ...] }
// ============================================================================

bool FNGGHttpServer::HandleAnimAddAimOffsetBlendSpace(
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

	FString AssetPath, BlendSpacePath;
	Body->TryGetStringField(TEXT("anim_bp_path"), AssetPath);
	Body->TryGetStringField(TEXT("blend_space"),  BlendSpacePath);
	if (AssetPath.IsEmpty() || BlendSpacePath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'blend_space' are required")));
		return true;
	}

	FString GraphName = TEXT("AnimGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);

	double AlphaIn = 1.0;
	Body->TryGetNumberField(TEXT("alpha"), AlphaIn);

	bool bSplice = true;
	Body->TryGetBoolField(TEXT("splice_before_output"), bSplice);

	double OffsetX = -250.0, OffsetY = 0.0;
	Body->TryGetNumberField(TEXT("node_offset_x"), OffsetX);
	Body->TryGetNumberField(TEXT("node_offset_y"), OffsetY);

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/anim/add_aim_offset_blend_space bp='%s' graph='%s' bs='%s' alpha=%.2f splice=%d off=[%.0f,%.0f]"),
		*AssetPath, *GraphName, *BlendSpacePath, AlphaIn, bSplice ? 1 : 0, OffsetX, OffsetY);

	const FString CapAssetPath     = AssetPath;
	const FString CapGraphName     = GraphName;
	const FString CapBlendSpacePath = BlendSpacePath;
	const float   CapAlpha         = (float)AlphaIn;
	const bool    CapSplice        = bSplice;
	const int32   CapOffsetX       = (int32)OffsetX;
	const int32   CapOffsetY       = (int32)OffsetY;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapAssetPath, CapGraphName, CapBlendSpacePath,
		 CapAlpha, CapSplice, CapOffsetX, CapOffsetY]()
	{
		// 1. Load the AnimBlueprint (with .BaseFilename fallback)
		UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *CapAssetPath);
		if (!AnimBP)
		{
			const FString AltPath = CapAssetPath + TEXT(".") + FPaths::GetBaseFilename(CapAssetPath);
			AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AltPath);
		}
		if (!AnimBP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("AnimBlueprint not found: %s"), *CapAssetPath)));
			return;
		}

		// 2. Resolve the BlendSpace asset (must be a UBlendSpace; AimOffset[1D] are subclasses).
		UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, *CapBlendSpacePath);
		if (!BlendSpace)
		{
			const FString AltPath = CapBlendSpacePath + TEXT(".") + FPaths::GetBaseFilename(CapBlendSpacePath);
			BlendSpace = LoadObject<UBlendSpace>(nullptr, *AltPath);
		}
		if (!BlendSpace)
		{
			Callback(JsonError(404, FString::Printf(TEXT("BlendSpace not found: %s"), *CapBlendSpacePath)));
			return;
		}
		// Warn (don't reject) if it's not actually an AimOffset — the runtime node will compile
		// the same way, but the validator on UAnimGraphNode_RotationOffsetBlendSpace will error.
		const bool bIsAimOffset =
			BlendSpace->IsA(UAimOffsetBlendSpace::StaticClass()) ||
			BlendSpace->IsA(UAimOffsetBlendSpace1D::StaticClass());
		if (!bIsAimOffset)
		{
			UE_LOG(LogNGGBridge, Warning,
				TEXT("/editor/anim/add_aim_offset_blend_space: '%s' is a UBlendSpace but NOT a UAimOffsetBlendSpace[1D] — the AnimBP compiler will reject this."),
				*BlendSpace->GetPathName());
		}

		// 3. Find the requested graph
		UEdGraph* AnimGraph = nullptr;
		const FName WantedName(*CapGraphName);
		for (UEdGraph* G : AnimBP->FunctionGraphs)
		{
			if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
		}
		if (!AnimGraph)
		{
			for (UEdGraph* G : AnimBP->UbergraphPages)
			{
				if (G && G->GetFName() == WantedName) { AnimGraph = G; break; }
			}
		}
		if (!AnimGraph)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Could not find graph '%s' in the AnimBlueprint"), *CapGraphName)));
			return;
		}

		// 4. Locate the Output Pose root node and its input pin
		UAnimGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* N : AnimGraph->Nodes)
		{
			RootNode = Cast<UAnimGraphNode_Root>(N);
			if (RootNode) break;
		}
		if (!RootNode)
		{
			Callback(JsonError(500, TEXT("Could not find Output Pose (UAnimGraphNode_Root) in AnimGraph")));
			return;
		}

		UEdGraphPin* RootInputPin = nullptr;
		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input) { RootInputPin = Pin; break; }
		}
		if (!RootInputPin)
		{
			Callback(JsonError(500, TEXT("Output Pose node has no input pin")));
			return;
		}

		// 5. Spawn the RotationOffsetBlendSpace node and seed runtime fields BEFORE pin allocation,
		// so the BlendSpace property and Alpha default are present when AllocateDefaultPins reads them.
		// Node.SetBlendSpace() is the public virtual setter on FAnimNode_BlendSpacePlayerBase
		// (X/Y/PlayRate/StartPosition are private FoldProperty fields driven via the AnimGraph
		// FoldedProperties system — they materialise as PinShownByDefault input pins on the node
		// without needing direct FAnimNode access).
		UAnimGraphNode_RotationOffsetBlendSpace* AONode = NewObject<UAnimGraphNode_RotationOffsetBlendSpace>(AnimGraph);
		AONode->CreateNewGuid();
		AONode->PostPlacedNewNode(); // creates default UAnimGraphNodeBinding_Base sub-object — without this the node compiles ErrorType=4 and runs as a no-op
		AONode->NodePosX = RootNode->NodePosX + CapOffsetX;
		AONode->NodePosY = RootNode->NodePosY + CapOffsetY;

		AONode->Node.SetBlendSpace(BlendSpace);
		AONode->Node.Alpha = CapAlpha;

		AnimGraph->AddNode(AONode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		AONode->AllocateDefaultPins();

		// 6. Splice: locate BasePose input + output pose pin.
		// The RotationOffsetBlendSpace BasePose lives on FAnimNode_RotationOffsetBlendSpace itself
		// (not via SkeletalControlBase), so the input pin is named "BasePose" — distinct from
		// FindPosePins' "ComponentPose"/PoseLink heuristic. Walk pins manually.
		UEdGraphPin* BaseInPin = nullptr;
		UEdGraphPin* OutPosePin = nullptr;
		for (UEdGraphPin* P : AONode->Pins)
		{
			if (!P) continue;
			const bool bIsLocalPose =
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct();
			const bool bIsCSPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct();
			if (P->Direction == EGPD_Input && (bIsLocalPose || bIsCSPose ||
				P->PinName == TEXT("BasePose") || P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose")))
			{
				BaseInPin = P;
			}
			else if (P->Direction == EGPD_Output && (bIsLocalPose || bIsCSPose))
			{
				OutPosePin = P;
			}
		}
		if (CapSplice && (!BaseInPin || !OutPosePin))
		{
			Callback(JsonError(500, TEXT("Could not find BasePose / output pose pins on the AimOffset node — cannot splice")));
			return;
		}

		bool bSplicedUpstream = false;
		FString UpstreamNodeName;
		if (CapSplice)
		{
			bSplicedUpstream = SpliceBeforeOutputPose(AnimGraph, RootInputPin, BaseInPin, OutPosePin, UpstreamNodeName);
		}

		// 7. Compile + dirty
		FKismetEditorUtilities::CompileBlueprint(AnimBP);
		AnimBP->MarkPackageDirty();

		// 8. Walk pins post-allocation to report the *actual* runtime pin names so callers can
		// pass them to /bp/connect_pins without guessing. Skip pose links and already-connected
		// pins (the spliced BasePose). Includes X, Y, Alpha (PinShownByDefault) and any others
		// that materialised — exact names depend on the editor's GET_MEMBER_NAME_STRING_CHECKED
		// expansion against FAnimNode_BlendSpacePlayer / FAnimNode_RotationOffsetBlendSpace.
		TArray<TSharedPtr<FJsonValue>> UnboundPins;
		for (UEdGraphPin* P : AONode->Pins)
		{
			if (!P || P->Direction != EGPD_Input) continue;
			const bool bIsPose =
				P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
				P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() ||
				P->PinName == TEXT("BasePose") || P->PinName == TEXT("ComponentPose") || P->PinName == TEXT("Pose");
			if (bIsPose) continue;
			if (P->LinkedTo.Num() > 0) continue;
			UnboundPins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/add_aim_offset_blend_space: added AimOffset for '%s' (spliced=%d upstream='%s' unbound=%d)"),
			*BlendSpace->GetPathName(), bSplicedUpstream ? 1 : 0, *UpstreamNodeName, UnboundPins.Num());

		// 9. Build response
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),               true);
		Resp->SetStringField(TEXT("asset_path"),       AnimBP->GetPathName());
		Resp->SetStringField(TEXT("node_id"),          AONode->NodeGuid.ToString());
		Resp->SetStringField(TEXT("blend_space"),      BlendSpace->GetPathName());
		Resp->SetBoolField  (TEXT("spliced_upstream"), bSplicedUpstream);
		if (!UpstreamNodeName.IsEmpty()) Resp->SetStringField(TEXT("upstream_was"), UpstreamNodeName);
		Resp->SetArrayField (TEXT("unbound_pins"),     UnboundPins);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/skeleton/add_virtual_bone
//
// Add a virtual bone to a USkeleton. Virtual bones live in the parent bone's
// reference frame and are positioned at the target bone's transform — useful
// for "follow the weapon" rigs (parent=weapon_r, target=ik_hand_l).
//
// Body:
//   {
//     "skeleton_path": "/Game/.../SK_Mannequin",   // required
//     "source_bone":   "weapon_r",                 // required - parent (where the VB lives)
//     "target_bone":   "ik_hand_l",                // required - VB tracks this bone's position
//     "vb_name":       "VB_ik_hand_left_weapon_space" // optional - explicit name. If omitted, UE auto-names
//   }
// ============================================================================

bool FNGGHttpServer::HandleSkeletonAddVirtualBone(
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

	FString SkeletonPath, SourceBone, TargetBone, ExplicitName;
	Body->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	Body->TryGetStringField(TEXT("source_bone"),   SourceBone);
	Body->TryGetStringField(TEXT("target_bone"),   TargetBone);
	Body->TryGetStringField(TEXT("vb_name"),       ExplicitName);

	if (SkeletonPath.IsEmpty() || SourceBone.IsEmpty() || TargetBone.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'skeleton_path', 'source_bone', and 'target_bone' are required")));
		return true;
	}

	UE_LOG(LogNGGBridge, Log,
		TEXT("/editor/skeleton/add_virtual_bone path='%s' src='%s' dst='%s' name='%s'"),
		*SkeletonPath, *SourceBone, *TargetBone, *ExplicitName);

	const FString CapturedPath = SkeletonPath;
	const FName   CapturedSrc(*SourceBone);
	const FName   CapturedDst(*TargetBone);
	const FString CapturedName = ExplicitName;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedPath, CapturedSrc, CapturedDst, CapturedName]()
	{
		USkeleton* Skel = LoadObject<USkeleton>(nullptr, *CapturedPath);
		if (!Skel)
		{
			const FString Alt = CapturedPath + TEXT(".") + FPaths::GetBaseFilename(CapturedPath);
			Skel = LoadObject<USkeleton>(nullptr, *Alt);
		}
		if (!Skel)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Skeleton not found at '%s'"), *CapturedPath)));
			return;
		}

		// Validate the source/target bones exist before attempting the add.
		if (Skel->GetReferenceSkeleton().FindBoneIndex(CapturedSrc) == INDEX_NONE)
		{
			Callback(JsonError(400, FString::Printf(TEXT("Source bone '%s' not found in skeleton"), *CapturedSrc.ToString())));
			return;
		}
		if (Skel->GetReferenceSkeleton().FindBoneIndex(CapturedDst) == INDEX_NONE)
		{
			Callback(JsonError(400, FString::Printf(TEXT("Target bone '%s' not found in skeleton"), *CapturedDst.ToString())));
			return;
		}

		FName CreatedName;
		bool  bOk = false;
		if (!CapturedName.IsEmpty())
		{
			CreatedName = FName(*CapturedName);
			bOk = Skel->AddNewNamedVirtualBone(CapturedSrc, CapturedDst, CreatedName);
		}
		else
		{
			bOk = Skel->AddNewVirtualBone(CapturedSrc, CapturedDst, CreatedName);
		}

		if (!bOk)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("AddNewVirtualBone failed for src='%s' dst='%s' (already exists?)"),
				*CapturedSrc.ToString(), *CapturedDst.ToString())));
			return;
		}

		// Note: AddNewVirtualBone(...) already broadcasts the change internally.
		// HandleVirtualBoneChanges() is not exported from Engine, so we don't call it here.
		Skel->MarkPackageDirty();
		Skel->PostEditChange();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/skeleton/add_virtual_bone: created '%s' on '%s'"),
			*CreatedName.ToString(), *Skel->GetPathName());

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),            true);
		Resp->SetStringField(TEXT("skeleton_path"), Skel->GetPathName());
		Resp->SetStringField(TEXT("source_bone"),   CapturedSrc.ToString());
		Resp->SetStringField(TEXT("target_bone"),   CapturedDst.ToString());
		Resp->SetStringField(TEXT("vb_name"),       CreatedName.ToString());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/anim/delete_node
//
// Delete a node from an AnimBP's AnimGraph by GUID. If reconnect_pose=true
// (the default), the upstream pose-link feeding the node's input is rewired
// to the consumer of the node's output, keeping the chain intact. This is
// what lets you remove a stale skeletal control without breaking the graph.
//
// Body:
//   {
//     "anim_bp_path":   "/Game/Path/ABP_X",  // required
//     "node_id":        "1D75784643A7BBE329E596BA3875B8F8",  // required - GUID
//     "reconnect_pose": true                  // optional, default true
//   }
// ============================================================================

bool FNGGHttpServer::HandleAnimDeleteNode(
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

	FString AssetPath, NodeIdStr;
	bool    bReconnectPose = true;
	Body->TryGetStringField(TEXT("anim_bp_path"),   AssetPath);
	Body->TryGetStringField(TEXT("node_id"),        NodeIdStr);
	Body->TryGetBoolField  (TEXT("reconnect_pose"), bReconnectPose);
	if (AssetPath.IsEmpty() || NodeIdStr.IsEmpty())
	{
		Callback(JsonError(400, TEXT("'anim_bp_path' and 'node_id' are required")));
		return true;
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeIdStr, NodeGuid))
	{
		// FGuid::Parse expects the canonical 32-hex-with-hyphens form. The MCP responses
		// emit the hex without hyphens. Try the fixed-length parser too.
		if (!FGuid::ParseExact(NodeIdStr, EGuidFormats::Digits, NodeGuid))
		{
			Callback(JsonError(400, FString::Printf(TEXT("Invalid GUID '%s'"), *NodeIdStr)));
			return true;
		}
	}

	UE_LOG(LogNGGBridge, Log, TEXT("/editor/anim/delete_node bp='%s' node='%s' reconnect=%d"),
		*AssetPath, *NodeIdStr, bReconnectPose ? 1 : 0);

	const FString CapturedAssetPath = AssetPath;
	const FGuid   CapturedGuid      = NodeGuid;
	const bool    CapturedReconnect = bReconnectPose;

	AsyncTask(ENamedThreads::GameThread,
		[Callback, CapturedAssetPath, CapturedGuid, CapturedReconnect]()
	{
		FAnimGraphContext Ctx; FString Err;
		if (!ResolveAnimGraphContext(CapturedAssetPath, Ctx, Err))
		{
			Callback(JsonError(404, Err));
			return;
		}

		// Find the node by GUID
		UEdGraphNode* Target = nullptr;
		for (UEdGraphNode* N : Ctx.Graph->Nodes)
		{
			if (N && N->NodeGuid == CapturedGuid) { Target = N; break; }
		}
		if (!Target)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Node with id '%s' not found in AnimGraph"), *CapturedGuid.ToString())));
			return;
		}

		const FString DeletedClassName = Target->GetClass()->GetName();
		const FString DeletedNodeName  = Target->GetName();

		// Optionally reconnect the pose chain: find this node's input/output pose pins,
		// link the upstream of the input to the downstream of the output.
		bool bReconnected = false;
		if (CapturedReconnect)
		{
			UEdGraphPin* InPin = nullptr; UEdGraphPin* OutPin = nullptr;
			// Try the SkeletalControlBase / standard pose pin pattern first.
			if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Target))
			{
				FindPosePins(AnimNode, InPin, OutPin);
			}
			// Fallback: any input/output pose-typed pin.
			if (!InPin || !OutPin)
			{
				for (UEdGraphPin* P : Target->Pins)
				{
					if (!P) continue;
					const bool bPose =
						P->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct() ||
						P->PinType.PinSubCategoryObject == FPoseLink::StaticStruct();
					if (bPose && P->Direction == EGPD_Input  && !InPin)  InPin  = P;
					if (bPose && P->Direction == EGPD_Output && !OutPin) OutPin = P;
				}
			}

			UEdGraphPin* UpstreamOut  = (InPin  && InPin->LinkedTo.Num()  > 0) ? InPin->LinkedTo[0]  : nullptr;
			TArray<UEdGraphPin*> Downstreams;
			if (OutPin) { Downstreams = OutPin->LinkedTo; }

			if (UpstreamOut && Downstreams.Num() > 0)
			{
				const UEdGraphSchema* Schema = Ctx.Graph->GetSchema();
				if (Schema)
				{
					for (UEdGraphPin* DS : Downstreams)
					{
						if (DS) Schema->TryCreateConnection(UpstreamOut, DS);
					}
					bReconnected = true;
				}
			}
		}

		// Break all links on the target's pins, then remove the node.
		Target->BreakAllNodeLinks();
		Ctx.Graph->RemoveNode(Target);

		FKismetEditorUtilities::CompileBlueprint(Ctx.AnimBP);
		Ctx.AnimBP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log,
			TEXT("/editor/anim/delete_node: removed '%s' (class=%s) reconnected=%d"),
			*DeletedNodeName, *DeletedClassName, bReconnected ? 1 : 0);

		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("ok"),                true);
		Resp->SetStringField(TEXT("asset_path"),        Ctx.AnimBP->GetPathName());
		Resp->SetStringField(TEXT("deleted_node"),      DeletedNodeName);
		Resp->SetStringField(TEXT("deleted_class"),     DeletedClassName);
		Resp->SetBoolField  (TEXT("reconnected_pose"),  bReconnected);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp, Writer);
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

// ---------------------------------------------------------------------------
// POST /editor/create_material_instance
// ---------------------------------------------------------------------------
bool FNGGHttpServer::HandleCreateMaterialInstance(
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

	FString AssetPath;     Body->TryGetStringField(TEXT("asset_path"),      AssetPath);
	FString ParentMatPath; Body->TryGetStringField(TEXT("parent_material"), ParentMatPath);

	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}
	if (ParentMatPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("parent_material is required")));
		return true;
	}

	// Parse scalar parameter overrides
	struct FScalarParam { FString Name; float Value; };
	TArray<FScalarParam> ScalarParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* ScalarArr;
		if (Body->TryGetArrayField(TEXT("scalar_params"), ScalarArr))
		{
			for (const auto& Elem : *ScalarArr)
			{
				auto Obj = Elem->AsObject();
				if (Obj.IsValid())
				{
					FScalarParam P;
					// TryGet* tolerates missing/wrong-typed sub-fields instead
					// of crashing in GetStringField/GetNumberField.
					Obj->TryGetStringField(TEXT("name"), P.Name);
					double Value = 0.0;
					Obj->TryGetNumberField(TEXT("value"), Value);
					P.Value = static_cast<float>(Value);
					ScalarParams.Add(P);
				}
			}
		}
	}

	// Parse vector parameter overrides
	struct FVectorParam { FString Name; FLinearColor Value; };
	TArray<FVectorParam> VectorParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* VectorArr;
		if (Body->TryGetArrayField(TEXT("vector_params"), VectorArr))
		{
			for (const auto& Elem : *VectorArr)
			{
				auto Obj = Elem->AsObject();
				if (Obj.IsValid())
				{
					FVectorParam P;
					Obj->TryGetStringField(TEXT("name"), P.Name);
					const TSharedPtr<FJsonObject>* ValObj;
					if (Obj->TryGetObjectField(TEXT("value"), ValObj))
					{
						double Rd = 0, Gd = 0, Bd = 0, Ad = 1;
						(*ValObj)->TryGetNumberField(TEXT("r"), Rd);
						(*ValObj)->TryGetNumberField(TEXT("g"), Gd);
						(*ValObj)->TryGetNumberField(TEXT("b"), Bd);
						(*ValObj)->TryGetNumberField(TEXT("a"), Ad);
						P.Value = FLinearColor((float)Rd, (float)Gd, (float)Bd, (float)Ad);
					}
					VectorParams.Add(P);
				}
			}
		}
	}

	// Parse texture parameter overrides ({ name, texture_path })
	struct FTextureParam { FString Name; FString TexturePath; };
	TArray<FTextureParam> TextureParams;
	{
		const TArray<TSharedPtr<FJsonValue>>* TexArr;
		if (Body->TryGetArrayField(TEXT("texture_params"), TexArr))
		{
			for (const auto& Elem : *TexArr)
			{
				auto Obj = Elem->AsObject();
				if (Obj.IsValid())
				{
					FTextureParam P;
					Obj->TryGetStringField(TEXT("name"), P.Name);
					Obj->TryGetStringField(TEXT("texture_path"), P.TexturePath);
					if (!P.Name.IsEmpty() && !P.TexturePath.IsEmpty())
					{
						TextureParams.Add(P);
					}
				}
			}
		}
	}

	FString BlueprintPath, ComponentName;
	Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
	Body->TryGetStringField(TEXT("component_name"), ComponentName);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, ParentMatPath, ScalarParams, VectorParams, TextureParams, BlueprintPath, ComponentName]()
	{
		// Load parent material
		UMaterialInterface* ParentMat = LoadObject<UMaterialInterface>(nullptr, *ParentMatPath);
		if (!ParentMat)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Parent material not found: '%s'"), *ParentMatPath)));
			return;
		}

		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		PackagePath = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

		// Load existing or create new MI
		UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(nullptr, *AssetPath);
		bool bAlreadyExisted = (MI != nullptr);

		if (!MI)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = ParentMat;
			UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
			MI = Cast<UMaterialInstanceConstant>(NewAsset);
		}

		if (!MI)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create material instance at '%s'"), *AssetPath)));
			return;
		}

		// Update parent if it changed
		if (MI->Parent != ParentMat)
		{
			MI->SetParentEditorOnly(ParentMat);
		}

		// Apply scalar parameter overrides
		for (const auto& P : ScalarParams)
		{
			MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*P.Name), P.Value);
		}

		// Apply vector parameter overrides
		for (const auto& P : VectorParams)
		{
			MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*P.Name), P.Value);
		}

		// Apply texture parameter overrides
		int32 TextureParamsSet = 0;
		for (const auto& P : TextureParams)
		{
			UTexture* Tex = LoadObject<UTexture>(nullptr, *P.TexturePath);
			if (Tex)
			{
				MI->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(*P.Name), Tex);
				++TextureParamsSet;
			}
		}

		MI->PostEditChange();
		MI->MarkPackageDirty();

		// Optional: apply MI to a Blueprint CDO component
		bool bAppliedToBlueprint = false;
		if (!BlueprintPath.IsEmpty())
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
			if (BP && BP->GeneratedClass)
			{
				UObject* CDO = BP->GeneratedClass->GetDefaultObject();
				if (CDO)
				{
					const FString CompName = ComponentName.IsEmpty() ? TEXT("StaticMeshComponent0") : ComponentName;
					for (TFieldIterator<FObjectProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
					{
						if (PropIt->GetName() == CompName || PropIt->GetFName() == *CompName)
						{
							UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(PropIt->GetObjectPropertyValue_InContainer(CDO));
							if (SMC)
							{
								SMC->SetMaterial(0, MI);
								bAppliedToBlueprint = true;
							}
							break;
						}
					}
					if (bAppliedToBlueprint)
					{
						FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),              true);
		Resp->SetStringField(TEXT("asset_path"),           AssetPath);
		Resp->SetStringField(TEXT("parent_material"),      ParentMatPath);
		Resp->SetBoolField  (TEXT("already_existed"),      bAlreadyExisted);
		Resp->SetNumberField(TEXT("scalar_params_set"),    ScalarParams.Num());
		Resp->SetNumberField(TEXT("vector_params_set"),    VectorParams.Num());
		Resp->SetNumberField(TEXT("texture_params_set"),   TextureParamsSet);
		Resp->SetBoolField  (TEXT("applied_to_blueprint"), bAppliedToBlueprint);

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/read_material
// Body: { "asset_path": "/Game/.../M_Foo" }
// Inspects a UMaterial or UMaterialInstance and reports its domain, blend mode,
// shading model, parent (instances only), and every scalar / vector / texture
// parameter with its current value. The editor is the source of truth — use
// this before SetVectorParameterValue calls to confirm the parameter exists.
// ============================================================================

bool FNGGHttpServer::HandleReadMaterial(
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

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath]()
	{
		UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
		if (!Mat)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Material not found: '%s'"), *AssetPath)));
			return;
		}

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),    true);
		Resp->SetStringField(TEXT("asset_path"), AssetPath);

		// Is it an instance? Report parent + class.
		if (UMaterialInstance* MInst = Cast<UMaterialInstance>(Mat))
		{
			Resp->SetStringField(TEXT("class"), TEXT("MaterialInstance"));
			Resp->SetStringField(TEXT("parent"),
				MInst->Parent ? MInst->Parent->GetPathName() : TEXT(""));
		}
		else
		{
			Resp->SetStringField(TEXT("class"), TEXT("Material"));
		}

		// Domain / blend mode / shading model via enum reflection (stable names).
		if (const UMaterial* BaseMat = Mat->GetMaterial())
		{
			if (const UEnum* E = StaticEnum<EMaterialDomain>())
				Resp->SetStringField(TEXT("domain"), E->GetNameStringByValue((int64)BaseMat->MaterialDomain));
		}
		if (const UEnum* E = StaticEnum<EBlendMode>())
			Resp->SetStringField(TEXT("blend_mode"), E->GetNameStringByValue((int64)Mat->GetBlendMode()));
		if (const UEnum* E = StaticEnum<EMaterialShadingModel>())
			Resp->SetStringField(TEXT("shading_model"),
				E->GetNameStringByValue((int64)Mat->GetShadingModels().GetFirstShadingModel()));

		// Scalar parameters
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Mat->GetAllScalarParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				float Value = 0.f;
				Mat->GetScalarParameterValue(Info, Value);
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"),  Info.Name.ToString());
				P->SetNumberField(TEXT("value"), Value);
				Arr.Add(MakeShared<FJsonValueObject>(P));
			}
			Resp->SetArrayField(TEXT("scalar_params"), Arr);
		}

		// Vector parameters
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Mat->GetAllVectorParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				FLinearColor Value = FLinearColor::Black;
				Mat->GetVectorParameterValue(Info, Value);
				TSharedPtr<FJsonObject> Val = MakeShared<FJsonObject>();
				Val->SetNumberField(TEXT("r"), Value.R);
				Val->SetNumberField(TEXT("g"), Value.G);
				Val->SetNumberField(TEXT("b"), Value.B);
				Val->SetNumberField(TEXT("a"), Value.A);
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"),  Info.Name.ToString());
				P->SetObjectField(TEXT("value"), Val);
				Arr.Add(MakeShared<FJsonValueObject>(P));
			}
			Resp->SetArrayField(TEXT("vector_params"), Arr);
		}

		// Texture parameters
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Mat->GetAllTextureParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& Info : Infos)
			{
				UTexture* Tex = nullptr;
				Mat->GetTextureParameterValue(Info, Tex);
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"),         Info.Name.ToString());
				P->SetStringField(TEXT("texture_path"), Tex ? Tex->GetPathName() : TEXT(""));
				Arr.Add(MakeShared<FJsonValueObject>(P));
			}
			Resp->SetArrayField(TEXT("texture_params"), Arr);
		}

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/create_float_curve
// Body: { "asset_path": "/Game/.../Curve_X",
//         "keys": [ { "time": 0.0, "value": 0.0 }, ... ],
//         "linear": true }   // linear (default) or constant (stepped) interp
// Creates / overwrites a UCurveFloat — the scalar counterpart of a color curve
// (intensity / speed / alpha over time, sampled with GetFloatValue).
// ============================================================================

bool FNGGHttpServer::HandleCreateFloatCurve(
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

	bool bLinear = true;
	Body->TryGetBoolField(TEXT("linear"), bLinear);

	struct FFloatKeyIn { float Time = 0.f, Value = 0.f; };
	TArray<FFloatKeyIn> Keys;
	const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
	if (Body->TryGetArrayField(TEXT("keys"), KeysArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *KeysArr)
		{
			const TSharedPtr<FJsonObject>* K = nullptr;
			if (V->TryGetObject(K) && K)
			{
				FFloatKeyIn Key;
				double D;
				if ((*K)->TryGetNumberField(TEXT("time"),  D)) Key.Time  = (float)D;
				if ((*K)->TryGetNumberField(TEXT("value"), D)) Key.Value = (float)D;
				Keys.Add(Key);
			}
		}
	}
	if (Keys.Num() == 0)
	{
		Callback(JsonError(400, TEXT("keys[] is required — each { time, value }")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Keys, bLinear]()
	{
		FString PackagePath, AssetName;
		AssetPath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		UCurveFloat* Curve = LoadObject<UCurveFloat>(nullptr, *AssetPath);
		const bool bAlreadyExisted = (Curve != nullptr);
		if (!Curve)
		{
			UPackage* Package = CreatePackage(*AssetPath);
			if (!Package)
			{
				Callback(JsonError(500, FString::Printf(TEXT("Failed to create package for '%s'"), *AssetPath)));
				return;
			}
			Curve = NewObject<UCurveFloat>(Package, FName(*AssetName),
				RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Curve);
		}
		if (!Curve)
		{
			Callback(JsonError(500, FString::Printf(TEXT("Failed to create curve at '%s'"), *AssetPath)));
			return;
		}

		Curve->FloatCurve.Reset();
		const ERichCurveInterpMode Interp = bLinear ? RCIM_Linear : RCIM_Constant;
		for (const FFloatKeyIn& K : Keys)
		{
			const FKeyHandle H = Curve->FloatCurve.AddKey(K.Time, K.Value);
			Curve->FloatCurve.SetKeyInterpMode(H, Interp);
		}

		Curve->MarkPackageDirty();

		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),         true);
		Resp->SetStringField(TEXT("asset_path"),      AssetPath);
		Resp->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Resp->SetNumberField(TEXT("key_count"),       Keys.Num());

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonCreated(RespBody));
	});

	return true;
}

// ============================================================================
// Handler: POST /editor/read_curve
// Body: { "asset_path": "/Game/.../Curve_X" }
// Reads a UCurveFloat or UCurveLinearColor and returns its keys. Float curves
// return [{ time, value }]; color curves return [{ time, r, g, b, a }].
// ============================================================================

bool FNGGHttpServer::HandleReadCurve(
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

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath]()
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetBoolField  (TEXT("success"),    true);
		Resp->SetStringField(TEXT("asset_path"), AssetPath);

		if (UCurveLinearColor* Color = LoadObject<UCurveLinearColor>(nullptr, *AssetPath))
		{
			Resp->SetStringField(TEXT("type"), TEXT("color"));
			TArray<TSharedPtr<FJsonValue>> Arr;
			// Channel 0 (R) drives the key times; sample all channels at each time.
			for (const FRichCurveKey& Key : Color->FloatCurves[0].Keys)
			{
				const FLinearColor C = Color->GetLinearColorValue(Key.Time);
				TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetNumberField(TEXT("time"), Key.Time);
				K->SetNumberField(TEXT("r"),    C.R);
				K->SetNumberField(TEXT("g"),    C.G);
				K->SetNumberField(TEXT("b"),    C.B);
				K->SetNumberField(TEXT("a"),    C.A);
				Arr.Add(MakeShared<FJsonValueObject>(K));
			}
			Resp->SetArrayField(TEXT("keys"), Arr);
		}
		else if (UCurveFloat* Float = LoadObject<UCurveFloat>(nullptr, *AssetPath))
		{
			Resp->SetStringField(TEXT("type"), TEXT("float"));
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FRichCurveKey& Key : Float->FloatCurve.Keys)
			{
				TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetNumberField(TEXT("time"),  Key.Time);
				K->SetNumberField(TEXT("value"), Key.Value);
				Arr.Add(MakeShared<FJsonValueObject>(K));
			}
			Resp->SetArrayField(TEXT("keys"), Arr);
		}
		else
		{
			Callback(JsonError(404, FString::Printf(TEXT("Curve not found (float or color): '%s'"), *AssetPath)));
			return;
		}

		FString RespBody;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RespBody);
		FJsonSerializer::Serialize(Resp.ToSharedRef(), Writer);
		Callback(JsonOk(RespBody));
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

	// Arbitrary code execution must never be reachable unauthenticated. When no
	// AuthToken is configured the listener is open on all interfaces, so refuse
	// to run rather than handing anyone on the network a Python REPL into the
	// editor. (Read/asset routes stay open; only this destructive route gates.)
	if (AuthToken.IsEmpty())
	{
		Callback(JsonError(403,
			TEXT("exec_python is disabled: no AuthToken configured. Set one in DefaultEngine.ini under [UnrealNGGMCP] AuthToken=<token> to enable arbitrary-code routes.")));
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

// ============================================================================
// Local helpers for the asset-creation handlers below
// ============================================================================
namespace NGGAssetCreatePriv
{
	/** Serialize a JSON object to a string. */
	static FString SerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** Split a /Game/Foo/Bar asset path into PackagePath="/Game/Foo" + AssetName="Bar". */
	static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
	{
		int32 LastSlash = INDEX_NONE;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash)) return false;
		OutPackagePath = AssetPath.Left(LastSlash);
		OutAssetName   = AssetPath.Mid(LastSlash + 1);
		return !OutAssetName.IsEmpty();
	}

	/** Save the package containing Asset to disk. Returns true on success. */
	static bool SavePackageForAsset(UObject* Asset)
	{
		if (!Asset) return false;
		UPackage* Pkg = Asset->GetOutermost();
		if (!Pkg) return false;
		Pkg->MarkPackageDirty();
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags     = SAVE_NoError;
		Args.Error         = GError;
		return UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
	}
}
using namespace NGGAssetCreatePriv;

// ============================================================================
// Handler: POST /asset/create_blueprint_struct
// Body: { asset_path, fields: [{name, type, default_value?}, ...], save? }
// Creates a UUserDefinedStruct with the supplied fields. Refuses if the asset
// already exists. Reuses NGGBpPriv::ResolveVariablePinType to map type strings.
// ============================================================================
bool FNGGHttpServer::HandleCreateBlueprintStruct(
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

	FString AssetPath;
	Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}
	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	struct FFieldSpec
	{
		FString Name;
		FString TypeStr;
		FString DefaultValue;
	};
	TArray<FFieldSpec> Fields;
	const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
	if (Body->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr)
	{
		for (const auto& V : *FieldsArr)
		{
			const TSharedPtr<FJsonObject>& Obj = V->AsObject();
			if (!Obj.IsValid()) continue;
			FFieldSpec F;
			Obj->TryGetStringField(TEXT("name"),          F.Name);
			Obj->TryGetStringField(TEXT("type"),          F.TypeStr);
			Obj->TryGetStringField(TEXT("default_value"), F.DefaultValue);
			if (!F.Name.IsEmpty() && !F.TypeStr.IsEmpty())
			{
				Fields.Add(MoveTemp(F));
			}
		}
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Fields, bSave]()
	{
		FString PackagePath, AssetName;
		if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}

		// Already exists?
		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
		if (UUserDefinedStruct* Existing = LoadObject<UUserDefinedStruct>(nullptr, *FullObjectPath))
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField  (TEXT("success"),         true);
			Out->SetBoolField  (TEXT("already_existed"), true);
			Out->SetStringField(TEXT("asset_path"),      AssetPath);
			Callback(JsonOk(NGGAssetCreatePriv::SerializeJson(Out)));
			return;
		}

		UPackage* Pkg = CreatePackage(*AssetPath);
		if (!Pkg)
		{
			Callback(JsonError(500, FString::Printf(TEXT("CreatePackage failed for '%s'"), *AssetPath)));
			return;
		}

		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
			Pkg, FName(*AssetName), RF_Public | RF_Standalone);
		if (!Struct)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateUserDefinedStruct failed for '%s'"), *AssetPath)));
			return;
		}

		// CreateUserDefinedStruct seeds a default Boolean variable. Capture its
		// guid so we can remove it once user-supplied fields land — gives the
		// caller a clean struct with only the fields they asked for.
		FGuid SeededGuid;
		{
			const TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(Struct);
			if (Vars.Num() > 0) SeededGuid = Vars.Last().VarGuid;
		}

		TArray<TSharedPtr<FJsonValue>> AddedJson;

		for (const FFieldSpec& F : Fields)
		{
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!NGGBpPriv::ResolveVariablePinType(F.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/asset/create_blueprint_struct: skipping field '%s' — invalid type '%s' (%s)"),
					*F.Name, *F.TypeStr, *TypeErr);
				continue;
			}

			if (!FStructureEditorUtils::AddVariable(Struct, PinType))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/asset/create_blueprint_struct: AddVariable failed for '%s'"), *F.Name);
				continue;
			}

			// AddVariable assigns an autogenerated name; rename to the requested one
			// using the guid from the just-added entry.
			FGuid NewGuid;
			{
				TArray<FStructVariableDescription>& Vars = FStructureEditorUtils::GetVarDesc(Struct);
				if (Vars.Num() > 0) NewGuid = Vars.Last().VarGuid;
			}

			if (NewGuid.IsValid())
			{
				if (!FStructureEditorUtils::RenameVariable(Struct, NewGuid, F.Name))
				{
					UE_LOG(LogNGGBridge, Warning,
						TEXT("/asset/create_blueprint_struct: RenameVariable failed for '%s'"), *F.Name);
				}

				if (!F.DefaultValue.IsEmpty())
				{
					if (!FStructureEditorUtils::ChangeVariableDefaultValue(Struct, NewGuid, F.DefaultValue))
					{
						UE_LOG(LogNGGBridge, Warning,
							TEXT("/asset/create_blueprint_struct: default '%s' rejected for field '%s'"),
							*F.DefaultValue, *F.Name);
					}
				}
			}

			AddedJson.Add(MakeShared<FJsonValueString>(F.Name));
		}

		// Strip the seeded default field once any user fields are present.
		if (SeededGuid.IsValid() && Fields.Num() > 0)
		{
			FStructureEditorUtils::RemoveVariable(Struct, SeededGuid);
		}

		Pkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Struct);

		bool bSaved = false;
		if (bSave)
		{
			bSaved = SavePackageForAsset(Struct);
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/asset/create_blueprint_struct: %s fields=%d saved=%d"),
			*AssetPath, AddedJson.Num(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetBoolField  (TEXT("already_existed"), false);
		Out->SetStringField(TEXT("asset_path"),      AssetPath);
		Out->SetArrayField (TEXT("fields_added"),    AddedJson);
		Out->SetBoolField  (TEXT("saved"),           bSaved);
		Callback(JsonCreated(NGGAssetCreatePriv::SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /asset/create_blueprint_enum
// Body: { asset_path, entries: [{name, display_name?, tooltip?}, ...], save? }
// Creates a UUserDefinedEnum. The "name" field is used as the display name —
// internally UE prefixes with "EnumName::" so callers should not replicate that.
// ============================================================================
bool FNGGHttpServer::HandleCreateBlueprintEnum(
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

	FString AssetPath;
	Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}
	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	struct FEntrySpec
	{
		FString Name;
		FString DisplayName;
		FString Tooltip;
	};
	TArray<FEntrySpec> Entries;
	const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
	if (Body->TryGetArrayField(TEXT("entries"), EntriesArr) && EntriesArr)
	{
		for (const auto& V : *EntriesArr)
		{
			const TSharedPtr<FJsonObject>& Obj = V->AsObject();
			if (!Obj.IsValid()) continue;
			FEntrySpec E;
			Obj->TryGetStringField(TEXT("name"),         E.Name);
			Obj->TryGetStringField(TEXT("display_name"), E.DisplayName);
			Obj->TryGetStringField(TEXT("tooltip"),      E.Tooltip);
			if (!E.Name.IsEmpty()) Entries.Add(MoveTemp(E));
		}
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Entries, bSave]()
	{
		FString PackagePath, AssetName;
		if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}

		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
		if (UUserDefinedEnum* Existing = LoadObject<UUserDefinedEnum>(nullptr, *FullObjectPath))
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField  (TEXT("success"),         true);
			Out->SetBoolField  (TEXT("already_existed"), true);
			Out->SetStringField(TEXT("asset_path"),      AssetPath);
			Callback(JsonOk(NGGAssetCreatePriv::SerializeJson(Out)));
			return;
		}

		UPackage* Pkg = CreatePackage(*AssetPath);
		if (!Pkg)
		{
			Callback(JsonError(500, FString::Printf(TEXT("CreatePackage failed for '%s'"), *AssetPath)));
			return;
		}

		UEnum* RawEnum = FEnumEditorUtils::CreateUserDefinedEnum(
			Pkg, FName(*AssetName), RF_Public | RF_Standalone);
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(RawEnum);
		if (!Enum)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateUserDefinedEnum failed for '%s'"), *AssetPath)));
			return;
		}

		TArray<TSharedPtr<FJsonValue>> AddedJson;
		for (const FEntrySpec& E : Entries)
		{
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);

			// The new entry is appended last; index = NumEnums() - 2 (the trailing
			// _MAX entry occupies the final slot).
			const int32 NumEnums = Enum->NumEnums();
			const int32 NewIndex = NumEnums >= 2 ? NumEnums - 2 : NumEnums - 1;
			if (NewIndex < 0)
			{
				continue;
			}

			const FString DisplayStr = E.DisplayName.IsEmpty() ? E.Name : E.DisplayName;
			FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(DisplayStr));

			if (!E.Tooltip.IsEmpty())
			{
				Enum->SetMetaData(TEXT("ToolTip"), *E.Tooltip, NewIndex);
			}

			AddedJson.Add(MakeShared<FJsonValueString>(E.Name));
		}

		Pkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Enum);

		bool bSaved = false;
		if (bSave)
		{
			bSaved = SavePackageForAsset(Enum);
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/asset/create_blueprint_enum: %s entries=%d saved=%d"),
			*AssetPath, AddedJson.Num(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetBoolField  (TEXT("already_existed"), false);
		Out->SetStringField(TEXT("asset_path"),      AssetPath);
		Out->SetArrayField (TEXT("entries_added"),   AddedJson);
		Out->SetBoolField  (TEXT("saved"),           bSaved);
		Callback(JsonCreated(NGGAssetCreatePriv::SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Internal: build a function graph on a Blueprint with the given inputs/outputs.
// Used by HandleCreateBlueprintInterface (and conceptually by HandleBpCreateFunction,
// which lives in NGGBlueprintGraph.cpp). Sets is_pure / is_const / category if
// requested. Returns false (with reason in OutError) on any failure.
//
// Pre-condition: must run on the Game Thread.
// ============================================================================
namespace NGGAssetCreatePriv
{
	struct FFnPinSpec
	{
		FString Name;
		FString TypeStr;
	};

	static bool BuildFunctionGraph(
		UBlueprint* BP,
		const FString& FunctionName,
		const TArray<FFnPinSpec>& Inputs,
		const TArray<FFnPinSpec>& Outputs,
		bool bIsPure,
		bool bIsConst,
		const FString& Category,
		FString& OutError,
		int32& OutInputsAdded,
		int32& OutOutputsAdded)
	{
		OutError.Empty();
		OutInputsAdded = 0;
		OutOutputsAdded = 0;

		if (!BP) { OutError = TEXT("blueprint is null"); return false; }
		if (FunctionName.IsEmpty()) { OutError = TEXT("function name is empty"); return false; }

		// Refuse if a graph with this name already exists.
		const FName FnName(*FunctionName);
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetFName() == FnName)
			{
				OutError = FString::Printf(
					TEXT("function '%s' already exists on blueprint"), *FunctionName);
				return false;
			}
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FnName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph) { OutError = TEXT("CreateNewGraph returned null"); return false; }

		// AddFunctionGraph<UClass> creates entry/result terminator nodes.
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated*/true, /*Sig*/nullptr);

		// Locate the entry node (must exist after AddFunctionGraph).
		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* N : NewGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N))
			{
				EntryNode = E;
				break;
			}
		}
		if (!EntryNode)
		{
			OutError = TEXT("function entry node was not created");
			return false;
		}

		// Apply flags + category metadata
		int32 ExtraFlags = 0;
		if (bIsPure)  ExtraFlags |= FUNC_BlueprintPure;
		if (bIsConst) ExtraFlags |= FUNC_Const;
		if (ExtraFlags) EntryNode->AddExtraFlags(ExtraFlags);

		if (!Category.IsEmpty())
		{
			EntryNode->MetaData.Category = FText::FromString(Category);
		}

		// Inputs: created on the Entry node as Output pins (data flows out of the entry).
		for (const FFnPinSpec& In : Inputs)
		{
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!NGGBpPriv::ResolveVariablePinType(In.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("BuildFunctionGraph: skipping input '%s' — invalid type '%s' (%s)"),
					*In.Name, *In.TypeStr, *TypeErr);
				continue;
			}
			if (EntryNode->CreateUserDefinedPin(FName(*In.Name), PinType, EGPD_Output, /*bUseUniqueName*/false))
			{
				++OutInputsAdded;
			}
		}

		// Outputs: located on the Result node as Input pins. The result node is
		// auto-created when the function has any return values; for safety we
		// instantiate one if missing.
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* N : NewGraph->Nodes)
		{
			if (UK2Node_FunctionResult* R = Cast<UK2Node_FunctionResult>(N))
			{
				ResultNode = R;
				break;
			}
		}
		if (!ResultNode && Outputs.Num() > 0)
		{
			ResultNode = NewObject<UK2Node_FunctionResult>(NewGraph);
			ResultNode->CreateNewGuid();
			ResultNode->PostPlacedNewNode();
			ResultNode->AllocateDefaultPins();
			NewGraph->AddNode(ResultNode, /*bUserAction*/false, /*bSelectNewNode*/false);
		}

		for (const FFnPinSpec& Out : Outputs)
		{
			if (!ResultNode) break;
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!NGGBpPriv::ResolveVariablePinType(Out.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("BuildFunctionGraph: skipping output '%s' — invalid type '%s' (%s)"),
					*Out.Name, *Out.TypeStr, *TypeErr);
				continue;
			}
			if (ResultNode->CreateUserDefinedPin(FName(*Out.Name), PinType, EGPD_Input, /*bUseUniqueName*/false))
			{
				++OutOutputsAdded;
			}
		}

		return true;
	}
}

// ============================================================================
// Handler: POST /asset/create_blueprint_interface
// Body: { asset_path, functions: [{name, inputs:[], outputs:[]}, ...], save? }
// Creates a UInterface-based Blueprint with the given function members.
// Compile is mandatory — interface BPs must compile to expose functions.
// ============================================================================
bool FNGGHttpServer::HandleCreateBlueprintInterface(
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

	FString AssetPath;
	Body->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path is required")));
		return true;
	}
	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	struct FFnSpec
	{
		FString Name;
		TArray<FFnPinSpec> Inputs;
		TArray<FFnPinSpec> Outputs;
	};
	TArray<FFnSpec> Functions;

	auto ParsePins = [](const TArray<TSharedPtr<FJsonValue>>* Arr, TArray<FFnPinSpec>& Out)
	{
		if (!Arr) return;
		for (const auto& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) continue;
			FFnPinSpec P;
			O->TryGetStringField(TEXT("name"), P.Name);
			O->TryGetStringField(TEXT("type"), P.TypeStr);
			if (!P.Name.IsEmpty() && !P.TypeStr.IsEmpty())
			{
				Out.Add(MoveTemp(P));
			}
		}
	};

	const TArray<TSharedPtr<FJsonValue>>* FnArr = nullptr;
	if (Body->TryGetArrayField(TEXT("functions"), FnArr) && FnArr)
	{
		for (const auto& V : *FnArr)
		{
			const TSharedPtr<FJsonObject>& Obj = V->AsObject();
			if (!Obj.IsValid()) continue;
			FFnSpec F;
			Obj->TryGetStringField(TEXT("name"), F.Name);
			if (F.Name.IsEmpty()) continue;
			const TSharedPtr<FJsonObject>* IgnoredObj = nullptr; (void)IgnoredObj;
			const TArray<TSharedPtr<FJsonValue>>* InsArr  = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* OutsArr = nullptr;
			Obj->TryGetArrayField(TEXT("inputs"),  InsArr);
			Obj->TryGetArrayField(TEXT("outputs"), OutsArr);
			ParsePins(InsArr,  F.Inputs);
			ParsePins(OutsArr, F.Outputs);
			Functions.Add(MoveTemp(F));
		}
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Functions, bSave]()
	{
		FString PackagePath, AssetName;
		if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}

		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
		if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *FullObjectPath))
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField  (TEXT("success"),         true);
			Out->SetBoolField  (TEXT("already_existed"), true);
			Out->SetStringField(TEXT("asset_path"),      AssetPath);
			Callback(JsonOk(NGGAssetCreatePriv::SerializeJson(Out)));
			return;
		}

		UPackage* Pkg = CreatePackage(*AssetPath);
		if (!Pkg)
		{
			Callback(JsonError(500, FString::Printf(TEXT("CreatePackage failed for '%s'"), *AssetPath)));
			return;
		}

		UBlueprint* IfaceBP = FKismetEditorUtilities::CreateBlueprint(
			UInterface::StaticClass(),
			Pkg,
			FName(*AssetName),
			EBlueprintType::BPTYPE_Interface,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("CreateBlueprintInterface")));
		if (!IfaceBP)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateBlueprint(Interface) failed for '%s'"), *AssetPath)));
			return;
		}

		TArray<TSharedPtr<FJsonValue>> AddedJson;
		for (const FFnSpec& F : Functions)
		{
			FString FnErr;
			int32 InCount = 0, OutCount = 0;
			if (BuildFunctionGraph(IfaceBP, F.Name, F.Inputs, F.Outputs,
				/*bIsPure*/false, /*bIsConst*/false, FString(),
				FnErr, InCount, OutCount))
			{
				AddedJson.Add(MakeShared<FJsonValueString>(F.Name));
			}
			else
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/asset/create_blueprint_interface: function '%s' failed (%s)"),
					*F.Name, *FnErr);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(IfaceBP);

		// Compile is mandatory for interface BPs.
		FCompilerResultsLog Compile;
		Compile.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(IfaceBP, EBlueprintCompileOptions::None, &Compile);
		const bool bCompiled = Compile.NumErrors == 0;

		Pkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(IfaceBP);

		bool bSaved = false;
		if (bSave)
		{
			bSaved = SavePackageForAsset(IfaceBP);
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/asset/create_blueprint_interface: %s functions=%d compiled=%d saved=%d"),
			*AssetPath, AddedJson.Num(), bCompiled ? 1 : 0, bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField   (TEXT("success"),         true);
		Out->SetBoolField   (TEXT("already_existed"), false);
		Out->SetStringField (TEXT("asset_path"),      AssetPath);
		Out->SetArrayField  (TEXT("functions_added"), AddedJson);
		Out->SetBoolField   (TEXT("compiled"),        bCompiled);
		Out->SetNumberField (TEXT("compile_errors"),  Compile.NumErrors);
		Out->SetBoolField   (TEXT("saved"),           bSaved);
		Callback(JsonCreated(NGGAssetCreatePriv::SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /asset/create_anim_blueprint
// Body: { asset_path, target_skeleton, parent_class?, save? }
// Creates a UAnimBlueprint bound to the given skeleton. parent_class defaults
// to /Script/Engine.AnimInstance.
// ============================================================================
bool FNGGHttpServer::HandleCreateAnimBlueprint(
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

	FString AssetPath, SkeletonPath, ParentClassPath;
	Body->TryGetStringField(TEXT("asset_path"),       AssetPath);
	Body->TryGetStringField(TEXT("target_skeleton"),  SkeletonPath);
	Body->TryGetStringField(TEXT("parent_class"),     ParentClassPath);
	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	if (AssetPath.IsEmpty() || SkeletonPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("asset_path and target_skeleton are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, SkeletonPath, ParentClassPath, bSave]()
	{
		FString PackagePath, AssetName;
		if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}

		// Already exists?
		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
		if (UAnimBlueprint* Existing = LoadObject<UAnimBlueprint>(nullptr, *FullObjectPath))
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField  (TEXT("success"),         true);
			Out->SetBoolField  (TEXT("already_existed"), true);
			Out->SetStringField(TEXT("asset_path"),      AssetPath);
			Callback(JsonOk(NGGAssetCreatePriv::SerializeJson(Out)));
			return;
		}

		// Skeleton lookup — accept both "/Game/Foo/SK_Foo" and "/Game/Foo/SK_Foo.SK_Foo".
		USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
		if (!Skeleton)
		{
			FString Alt = SkeletonPath;
			FString Tail;
			if (SkeletonPath.Split(TEXT("/"), nullptr, &Tail, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				Alt = SkeletonPath + TEXT(".") + Tail;
			}
			Skeleton = LoadObject<USkeleton>(nullptr, *Alt);
		}
		if (!Skeleton)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("target_skeleton not found: %s"), *SkeletonPath)));
			return;
		}

		// Parent class — default to UAnimInstance, allow override via path or short name.
		UClass* ParentClass = UAnimInstance::StaticClass();
		if (!ParentClassPath.IsEmpty())
		{
			UClass* Found = FindObject<UClass>(nullptr, *ParentClassPath);
			if (!Found)
			{
				Found = LoadObject<UClass>(nullptr, *ParentClassPath);
			}
			if (Found && Found->IsChildOf(UAnimInstance::StaticClass()))
			{
				ParentClass = Found;
			}
			else if (Found)
			{
				Callback(JsonError(400, FString::Printf(
					TEXT("parent_class '%s' is not a UAnimInstance subclass"), *ParentClassPath)));
				return;
			}
			// If not found at all, silently fall back to UAnimInstance (logged below).
			if (!Found)
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/asset/create_anim_blueprint: parent_class '%s' not found, using UAnimInstance"),
					*ParentClassPath);
			}
		}

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>(GetTransientPackage());
		Factory->TargetSkeleton = Skeleton;
		Factory->ParentClass    = ParentClass;

		UObject* NewAsset = AT.CreateAsset(AssetName, PackagePath, UAnimBlueprint::StaticClass(), Factory);
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(NewAsset);
		if (!AnimBP)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("AssetTools failed to create AnimBlueprint at '%s'"), *AssetPath)));
			return;
		}

		AnimBP->MarkPackageDirty();

		bool bSaved = false;
		if (bSave)
		{
			bSaved = SavePackageForAsset(AnimBP);
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/asset/create_anim_blueprint: %s skeleton=%s parent=%s saved=%d"),
			*AssetPath, *Skeleton->GetName(), *ParentClass->GetName(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetBoolField  (TEXT("already_existed"), false);
		Out->SetStringField(TEXT("asset_path"),      AssetPath);
		Out->SetStringField(TEXT("target_skeleton"), Skeleton->GetPathName());
		Out->SetStringField(TEXT("parent_class"),    ParentClass->GetPathName());
		Out->SetBoolField  (TEXT("saved"),           bSaved);
		Callback(JsonCreated(NGGAssetCreatePriv::SerializeJson(Out)));
	});

	return true;
}
