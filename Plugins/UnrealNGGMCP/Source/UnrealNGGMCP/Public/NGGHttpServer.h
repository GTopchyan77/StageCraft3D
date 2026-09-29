// Copyright 2025-2026 NGG. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "UObject/StrongObjectPtr.h"
// TStrongObjectPtr<UDynamicMesh> in MeshHandles requires the full type (calls AddRef via templates).
#include "UDynamicMesh.h"
// ENGINE_MAJOR_VERSION / ENGINE_MINOR_VERSION / ENGINE_PATCH_VERSION, reported by GET /health.
#include "Runtime/Launch/Resources/Version.h"

/**
 * Plugin version, reported by GET /health. Keep in sync with the
 * "VersionName" field in UnrealNGGMCP.uplugin.
 */
#define NGG_PLUGIN_VERSION "1.0"

/**
 * Bridge wire-protocol version, reported by GET /health.
 *
 * Bump this when a change would break an older sidecar: a route removed or
 * renamed, a request field that becomes required, or a response shape that
 * changes meaning. Adding a route, or adding an optional field, does not need
 * a bump — the sidecar tolerates both.
 *
 * The sidecar warns when it sees a protocol it does not recognise, so this is
 * the mechanism that turns "plugin and sidecar are out of step" into one clear
 * message instead of a scattering of odd failures.
 */
#define NGG_BRIDGE_PROTOCOL 1

class IHttpRouter;

/**
 * FNGGHttpServer
 *
 * Wraps the UE5 HTTPServer module to expose a REST API that MCP clients can
 * call to drive editor asset operations.  All handlers dispatch work back onto
 * the Game Thread so that UnrealEd APIs are called safely.
 *
 * Endpoints:
 *   GET  /health
 *   GET  /assets/list?path=...
 *   GET  /assets/get?path=...
 *   POST /assets/create
 *   POST /assets/set_property
 *   POST /editor/save_all
 *   POST /editor/reimport
 *   GET  /gameplay_tags/list
 *   POST /editor/create_blueprint
 *   POST /editor/create_widget_blueprint
 *   POST /editor/set_blueprint_defaults
 *   POST /editor/set_world_settings
 *   POST /editor/spawn_actor_in_level
 *   POST /editor/set_component_defaults
 *   POST /editor/create_level
 *   GET  /editor/list_actors
 *   POST /editor/update_actor
 *   POST /editor/delete_actor
 *   POST /editor/style_widgets
 *   POST /editor/remove_widget_from_blueprint
 *   POST /editor/reparent_widget
 *   POST /editor/rename_widget
 *   POST /editor/compile_widget_blueprint
 *   POST /editor/create_material
 *   POST /assets/delete
 *   POST /editor/open_level
 *   POST /input/configure_imc
 *   POST /editor/create_niagara_system
 *   POST /editor/configure_niagara_system
 *   POST /editor/set_niagara_emitter_params
 *   POST /editor/configure_anim_blueprint
 *   POST /editor/anim/add_two_bone_ik
 *   POST /editor/anim/add_copy_bone
 *   POST /editor/anim/add_hand_ik_retargeting
 *   POST /editor/anim/add_layered_bone_blend
 *   POST /editor/anim/add_sequence_player
 *   POST /editor/anim/add_modify_bone
 *   POST /editor/anim/add_look_at
 *   POST /editor/anim/add_aim_offset_blend_space
 *   POST /editor/skeleton/add_virtual_bone
 *   POST /editor/anim/delete_node
 *   POST /editor/set_level_environment
 *   POST /editor/create_material_instance
 *   POST /editor/exec_python
 *   POST /mesh/create
 *   POST /mesh/append_primitive
 *   POST /mesh/boolean
 *   POST /mesh/transform
 *   POST /mesh/deform
 *   POST /mesh/remesh
 *   POST /mesh/bake_static
 *   POST /mesh/delete_handle
 *   POST /mesh/add_socket
 *   POST /asset/create_blueprint_struct
 *   POST /asset/create_blueprint_enum
 *   POST /asset/create_blueprint_interface
 *   POST /asset/create_anim_blueprint
 *   POST /bp/create_function
 *   POST /bp/create_macro
 *   GET  /bp/list_variables
 *   GET  /bp/get_selection
 *   POST /bt/create_tree
 *   POST /bt/create_blackboard
 *   POST /bt/add_blackboard_keys
 *   POST /bt/add_logic
 *   GET  /bt/read_tree
 *   POST /gas/setup_actor
 *   POST /gas/create_attribute_set
 *   POST /gas/create_ability
 *   POST /gas/create_effect
 *   POST /gas/configure_asc
 *   GET  /gas/read_setup
 *
 * Extension endpoints (e.g. the /adl/exercise/... routes) are registered dynamically by
 * optional companion modules via RegisterDynamicRoute().
 */
class UNREALNGGMCP_API FNGGHttpServer : public TSharedFromThis<FNGGHttpServer>
{
public:
	explicit FNGGHttpServer(uint32 InPort = 6776);
	~FNGGHttpServer();

	/** Start listening.  Returns false if the port is already in use. */
	bool Start();

	/** Stop listening and release the port. */
	void Stop();

	bool IsRunning() const { return bRunning; }
	uint32 GetPort()  const { return Port; }

	// -----------------------------------------------------------------------
	// Extension API — for use by companion modules (e.g. UnrealNGGMCP_ADL)
	// -----------------------------------------------------------------------

	/**
	 * Register an HTTP route from an extension module.
	 * Call this from another module's StartupModule() after UnrealNGGMCP has started.
	 * The handler is wrapped with the same auth check as built-in routes.
	 * All extension routes are automatically unregistered when the server stops.
	 */
	void RegisterDynamicRoute(const FString& Path, EHttpServerRequestVerbs Verb, FHttpRequestHandler Handler);

	// -----------------------------------------------------------------------
	// Response helpers (public so extension modules can build responses)
	// -----------------------------------------------------------------------
	static TUniquePtr<FHttpServerResponse> JsonOk     (const FString& JsonBody);
	static TUniquePtr<FHttpServerResponse> JsonError  (int32 Code, const FString& Message);
	static TUniquePtr<FHttpServerResponse> JsonCreated(const FString& JsonBody);

	// -----------------------------------------------------------------------
	// Body parsing helpers (public so extension modules can parse requests)
	// -----------------------------------------------------------------------
	static bool ParseJsonBody(const FHttpServerRequest& Req, TSharedPtr<FJsonObject>& OutObj, FString& OutError);
	static FString GetQueryParam(const FHttpServerRequest& Req, const FString& Key);

	// -----------------------------------------------------------------------
	// Asset helpers (public so extension modules can serialise UObjects)
	// All must be called on the Game Thread.
	// -----------------------------------------------------------------------

	/** Serialise a UObject's UPROPERTY fields to a JSON object via FProperty reflection. */
	static TSharedPtr<FJsonObject> UObjectToJson(UObject* Obj);

	/** Serialise a single FProperty value to a JSON value. */
	static TSharedPtr<FJsonValue> PropertyToJson(const FProperty* Prop, const void* ContainerPtr);

	/** Apply a JSON scalar/string to a named UPROPERTY on a UObject. Returns error string or empty. */
	static FString SetPropertyFromJson(UObject* Obj, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value);

	/**
	 * Constant-time string equality for auth-token comparison.
	 * Does not early-out on the first differing byte, so it does not leak token
	 * length or content via timing side channels. Compares lengths first, then
	 * XOR-accumulates over every byte before testing the accumulator.
	 */
	static bool ConstantTimeEquals(const FString& A, const FString& B);

	/**
	 * True if the request originated from this machine (127.0.0.0/8, ::1, or an
	 * IPv4-mapped loopback address such as ::ffff:127.0.0.1).
	 *
	 * UE binds this listener to localhost by default (FHttpServerListenerConfig::
	 * BindAddress), so out of the box the socket is unreachable from off-machine.
	 * That default is overridable from ini — [HTTPServer.Listeners] with
	 * DefaultBindAddress=any or +ListenerOverrides=(Port=6776, BindAddress=any)
	 * binds 0.0.0.0 and exposes the bridge on every interface. This check is what
	 * keeps that configuration from silently handing the editor to the network:
	 * the socket still accepts, but every route refuses unless the peer is local.
	 *
	 * A request with no resolvable peer address cannot be proven local and is
	 * therefore treated as remote.
	 */
	static bool IsLoopbackPeer(const FHttpServerRequest& Req);

private:
	// -----------------------------------------------------------------------
	// Route registration helpers
	// -----------------------------------------------------------------------
	void RegisterRoutes();
	void UnregisterRoutes();

	// -----------------------------------------------------------------------
	// Auth-token setup — called from Start(), before any route is registered
	// -----------------------------------------------------------------------

	/**
	 * First-run setup for a project that has no [UnrealNGGMCP] AuthToken at all:
	 * mints a random one, writes it into <Project>/Config/UserEngine.ini and
	 * adopts it for this session. That file is one of the two the MCP sidecar
	 * looks in for its bearer token, so a plugin dropped into a fresh project
	 * comes up with exec_python and the lifecycle routes working instead of
	 * refusing every call.
	 *
	 * UserEngine.ini rather than DefaultEngine.ini because it is gitignored:
	 * DefaultEngine.ini is tracked, and a token written there ends up in the
	 * repository history.
	 *
	 * Leaves the bridge unauthenticated (as before) if the project opted out via
	 * AutoProvisionAuthToken=false, or if the ini could not be written — a token
	 * the sidecar cannot read would only turn "some routes disabled" into "every
	 * request 401".
	 */
	void ProvisionAuthToken();

	/**
	 * Logs a warning when the active token is in neither
	 * <Project>/Config/UserEngine.ini nor <Project>/Config/DefaultEngine.ini —
	 * i.e. it came from elsewhere in the config hierarchy, where the sidecar
	 * will not find it.
	 */
	void WarnIfProjectIniTokenDiffers() const;

	// -----------------------------------------------------------------------
	// Handler declarations — each returns true to signal the response is sent.
	// UE5 HTTP server uses a callback: bool Handler(const FHttpServerRequest&, const FHttpResultCallback&)
	// -----------------------------------------------------------------------
	bool HandleHealth        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleProjectInfo   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsList    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsGet     (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsCreate  (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsDuplicate(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsSetProp      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsSetMapEntries(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleEditorSaveAll          (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleEditorReimport         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGameplayTagsList       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateBlueprint        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateWidgetBlueprint  (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSetBlueprintDefaults   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleReparentBlueprint      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSetWorldSettings       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSpawnActorInLevel      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSetComponentDefaults   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGetComponentDefaults   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAddComponentToBlueprint(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleRemoveComponentFromBlueprint(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateLevel            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleListActors             (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleUpdateActor            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleDeleteActor            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleStyleWidgets           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGetWidgetTree          (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleRemoveWidgetFromBlueprint(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleReparentWidget         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleRenameWidget           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCompileWidgetBlueprint (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateMaterial         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateColorCurve       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateFloatCurve       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleReadCurve              (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleReadMaterial           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAssetsDelete           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleOpenLevel              (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBatch                  (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleInputConfigureIMC      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleEditorShutdown         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleEditorBuildAndRun      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleEditorKillAndRestart   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateNiagaraSystem       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleConfigureNiagaraSystem    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSetNiagaraEmitterParams   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAddWidgetToBlueprint      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleImportAsset               (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleConfigureAnimBlueprint    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddTwoBoneIK          (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddCopyBone           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddHandIKRetargeting  (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddLayeredBoneBlend   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddSequencePlayer     (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddModifyBone         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddLookAt             (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimAddAimOffsetBlendSpace(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSkeletonAddVirtualBone    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleAnimDeleteNode            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSetLevelEnvironment       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateMaterialInstance    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreatePostProcessMaterial (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleSpawnPostProcessVolume    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleExecPython                (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// ---- Geometry Script mesh composition endpoints (NGGMeshGeometryScript.cpp) ----
	bool HandleMeshCreate            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshAppendPrimitive   (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshBoolean           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshTransform         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshDeform            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshRemesh            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshBakeStatic        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshDeleteHandle      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleMeshAddSocket         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// ---- Blueprint graph authoring endpoints (NGGBlueprintGraph.cpp) ----
	bool HandleBpAddNode             (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpConnectPins         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpCompile             (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpAddLogic            (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpReadGraph           (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpGetSelection        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpDeleteNode          (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpRefreshAllNodes     (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpAddInterface        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpImplementInterfaceFunction(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpLint                (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpLintProject         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpCreateVariable      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpListVariables       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpCreateFunction      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpCreateMacro         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpDeleteFunction      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpDeleteVariable      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBpHealWorldContext    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// ---- Asset creation: structs/enums/interfaces/anim BPs (NGGHttpServer.cpp) -----
	bool HandleCreateBlueprintStruct     (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateBlueprintEnum       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateBlueprintInterface  (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleCreateAnimBlueprint       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// ---- Behavior Tree authoring (NGGBehaviorTree.cpp) — Phase 1: asset creation ----
	bool HandleBtCreateTree              (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBtCreateBlackboard        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBtAddBlackboardKeys       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	// ---- Behavior Tree authoring (NGGBehaviorTree.cpp) — Phase 2: graph wiring ----
	bool HandleBtAddLogic                (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleBtReadTree                (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// ---- StateTree authoring (NGGStateTree.cpp) — Phase 1: read ----
	bool HandleStateTreeReadTree         (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	// ---- StateTree authoring (NGGStateTree.cpp) — Phase 2: repoint a node's class ----
	bool HandleStateTreeRepointNode      (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	// ---- Gameplay Ability System (NGGGameplayAbilitySystem.cpp) ----
	bool HandleGasSetupActor       (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGasCreateAttributeSet(const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGasCreateAbility    (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGasCreateEffect     (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGasConfigureAsc     (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);
	bool HandleGasReadSetup        (const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete);

	/**
	 * Transient map of user-supplied node IDs to node FGuids, keyed on
	 * "<BlueprintPath>::<NodeId>". Populated by /bp/add_node and /bp/add_logic
	 * so callers can reference nodes by a human-friendly string in follow-up
	 * /bp/connect_pins calls. Entries carry a timestamp and are evicted after
	 * 10 minutes to keep the map bounded. Only touched on the Game Thread.
	 */
	struct FBpNodeIdEntry
	{
		FGuid    NodeGuid;
		double   CreatedAtSeconds = 0.0;
	};
	TMap<FString, FBpNodeIdEntry> BpNodeIdMap;

	/**
	 * Transient UDynamicMesh working objects keyed by client-chosen string handles.
	 * Populated by /mesh/create, consumed by /mesh/append_primitive, /mesh/boolean,
	 * etc. TStrongObjectPtr keeps the mesh alive across HTTP calls (and across GCs)
	 * until /mesh/delete_handle — or the server shuts down (destructor clears map).
	 * Only touched from the Game Thread inside mesh handler AsyncTasks.
	 */
	TMap<FString, TStrongObjectPtr<UDynamicMesh>> MeshHandles;

	uint32  Port;
	/**
	 * Bearer token every route requires. Empty = auth disabled, which also
	 * disables exec_python and the lifecycle routes. Read in Start() from
	 * [UnrealNGGMCP] AuthToken; if the project has never configured one,
	 * ProvisionAuthToken generates and persists it instead.
	 */
	FString AuthToken;
	bool    bRunning = false;

	/**
	 * When false (the default), every route refuses requests whose peer is not
	 * loopback. Set via [UnrealNGGMCP] AllowRemoteClients=true in
	 * DefaultEngine.ini for the deliberate remote-access case (a second machine,
	 * a container, CI). Read once in Start(), before any route is registered.
	 */
	bool    bAllowRemoteClients = false;

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
};
