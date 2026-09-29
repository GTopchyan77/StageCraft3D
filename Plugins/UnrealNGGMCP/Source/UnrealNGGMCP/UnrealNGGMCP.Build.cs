// Copyright 2025-2026 NGG. All Rights Reserved.

using UnrealBuildTool;

public class UnrealNGGMCP : ModuleRules
{
	public UnrealNGGMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This is an Editor-only module. Enforce that at build time.
		if (Target.Type != TargetType.Editor)
		{
			// Editor-only module — the uplugin Type=Editor ensures this is never
			// compiled for non-editor targets. Return early so Rider project-file
			// generation (which scans all Build.cs files) does not throw.
			return;
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// Built-in UE5 HTTP server (IHttpRouter, FHttpServerModule)
			"HTTPServer",
			// JSON serialisation (TJsonReader, FJsonSerializer, FJsonObjectConverter)
			"Json",
			"JsonUtilities",
			// Gameplay Tags (FGameplayTag listing)
			"GameplayTags",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// IPluginManager — locates this plugin's own folder, whatever it was
			// renamed to on the way into the project (see FNGGPluginPaths).
			"Projects",
			// Editor-side asset creation, management, reimport
			"UnrealEd",
			// Niagara particle system creation (UNiagaraSystem)
			"Niagara",
			// Material creation and recompilation (UMaterialEditingLibrary, UMaterialExpressionConstant3Vector)
			"MaterialEditor",
			"AssetTools",
			"AssetRegistry",
			// Slate / SlateCore needed transitively by UnrealEd headers
			"Slate",
			"SlateCore",
			// Blueprint creation and editing (FKismetEditorUtilities, FBlueprintEditorUtils)
			"Kismet",
			// Blueprint recompilation (RecompileBlueprint)
			"KismetCompiler",
			// Blueprint reparenting (UBlueprintEditorLibrary::ReparentBlueprint)
			"BlueprintEditorLibrary",
			// Widget Blueprint creation (UWidgetBlueprintFactory)
			"UMGEditor",
			// Widget types for tree population (UCanvasPanel, UButton, UTextBlock, etc.)
			"UMG",
			// Animation Blueprint editor APIs (UAnimGraphNode_StateMachine, state nodes, etc.)
			"AnimGraph",
			"AnimGraphRuntime",
			"BlueprintGraph",
			// Behavior Tree runtime: UBehaviorTree, UBlackboardData, UBlackboardKeyType_*
			"AIModule",
			// AI graph nodes (UAIGraphNode parent of UBehaviorTreeGraphNode_*)
			"AIGraph",
			// Behavior Tree editor: UBehaviorTreeFactory, UBlackboardDataFactory,
			// UBehaviorTreeGraph, UBehaviorTreeGraphNode_Composite/Task/Decorator/Service/Root
			"BehaviorTreeEditor",
			// StateTree: UStateTree (runtime) + UStateTreeEditorData / UStateTreeState /
			// FStateTreeEditorNode (editor authoring tree) for the /statetree/* routes.
			"StateTreeModule",
			"StateTreeEditorModule",
			// FStateTreeCompilerLog -> FStateTreeBindableStructDesc derives from
			// FPropertyBindingBindableStructDescriptor; its dtor lives here (repoint_node compile).
			"PropertyBindingUtils",
			// Gameplay Ability System (UAbilitySystemComponent, UAttributeSet,
			// UGameplayAbility, UGameplayEffect, FGameplayAttribute, EGameplayModOp)
			"GameplayAbilities",
			// FKey, EKeys — InputCore exports FKey::IsValid() and the key constants
			"InputCore",
			// Enhanced Input — UInputMappingContext, UInputAction, UInputModifier subclasses
			"EnhancedInput",
			// Python scripting — IPythonScriptPlugin, FPythonCommandEx (exec_python endpoint)
			"PythonScriptPlugin",
			// ---- Geometry Script (mesh composition endpoints) -----------------
			// Blueprint/C++ function libraries for primitive append, boolean,
			// deform, remesh, transform, static mesh bake.
			"GeometryScriptingCore",
			// Editor-only helpers: CreateNewStaticMeshAssetFromMesh etc.
			"GeometryScriptingEditor",
			// UDynamicMesh / UDynamicMeshComponent
			"GeometryFramework",
			// FDynamicMesh3 types
			"GeometryCore",
			// FMeshDescription / FStaticMeshAttributes (for static-mesh baking)
			"MeshDescription",
			"StaticMeshDescription",
			// FDynamicMesh <-> FMeshDescription conversion helpers
			"MeshConversion",
			// ---- Claude Chat editor window ------------------------------------
			// IWebSocket client used to talk to the ngg-sidecar daemon
			"WebSockets",
			// HTTP client for sidecar /health probe and shutdown request
			"HTTP",
			// TCP socket probe used by IsSidecarAlive (avoids HTTP re-entrancy crash
			// when called from inside FTSTicker callbacks)
			"Sockets",
			// Toolbar / menu extensions (UToolMenus)
			"ToolMenus",
			// Workspace menu categories for dockable tab
			"WorkspaceMenuStructure",
			// FAppStyle / FEditorStyle brushes for Slate styling
			"EditorStyle",
			// Content browser selection query (FContentBrowserModule::GetSelectedAssets)
			"ContentBrowser",
			// ULevelEditorSubsystem::NewLevel — /editor/create_level
			"LevelEditor",
			// PNG encoding for clipboard image paste (IImageWrapperModule)
			"ImageWrapper",
		});
	}
}
