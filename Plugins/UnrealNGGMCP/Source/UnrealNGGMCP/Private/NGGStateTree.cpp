// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGStateTree.cpp
//
// Implementation of the /statetree/* HTTP endpoints that expose Unreal Engine 5
// StateTree authoring (the UStateTreeEditorData tree) to MCP clients.
//
// Phase 1 — read only:
//   GET /statetree/read_tree?state_tree=/Game/AI/ST_Foo
//     Returns the authoring state hierarchy (states -> enter_conditions / tasks /
//     transitions / children), and for every task/condition/evaluator node its
//     wrapped class path. This closes the StateTree inspection gap (the asset is
//     an opaque binary; no bridge route read it before) and lets a caller verify
//     a node's class — the basis for the Phase 2 /statetree/repoint_node route.
//
// Threading contract (same as the rest of the bridge):
//   - All UObject access runs on the Game Thread via AsyncTask.
//   - The handler returns true synchronously after dispatching; OnComplete fires
//     from inside the GT lambda.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- HTTP / JSON ----------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ---- Engine ---------------------------------------------------------------
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"   // NewObject, FindFirstObject
#include "StructUtils/InstancedStruct.h"
#include "Engine/Engine.h"            // GEngine->CopyPropertiesForUnrelatedObjects
#include "FileHelpers.h"              // UEditorLoadingAndSavingUtils

// ---- StateTree runtime + editor data --------------------------------------
#include "StateTree.h"                 // UStateTree
#include "StateTreeEditorData.h"       // UStateTreeEditorData
#include "StateTreeState.h"            // UStateTreeState, FStateTreeEditorNode, FStateTreeTransition
#include "StateTreeTypes.h"            // FStateTreeStateLink, EStateTreeTransitionTrigger, EStateTreeStateType
#include "StateTreeEditingSubsystem.h" // UStateTreeEditingSubsystem::CompileStateTree / ValidateStateTree
#include "StateTreeCompilerLog.h"      // FStateTreeCompilerLog
#include "StateTreeEditorPropertyBindings.h" // FStateTreeEditorPropertyBindings (GetMutableBindings)
#include "StateTreePropertyBindings.h"       // FStateTreePropertyPathBinding
#include "PropertyBindingPath.h"             // FPropertyBindingPath / FPropertyBindingPathSegment

// ============================================================================
// Local helpers
// ============================================================================
namespace NGGStPriv
{
	static FString StSerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/**
	 * Resolve a UStateTree's editor data (the authoring tree) via reflection. EditorData is an editor-only
	 * member; reading it by property keeps this access-modifier- and WITH_EDITORONLY_DATA-safe.
	 */
	static UStateTreeEditorData* GetEditorData(UStateTree* StateTree)
	{
		if (!StateTree) return nullptr;
		if (FObjectProperty* P = CastField<FObjectProperty>(StateTree->GetClass()->FindPropertyByName(TEXT("EditorData"))))
		{
			return Cast<UStateTreeEditorData>(P->GetObjectPropertyValue_InContainer(StateTree));
		}
		return nullptr;
	}

	/**
	 * Read the wrapped node class path off an editor node's Node struct. For BP/C++ task/condition/evaluator
	 * nodes the Node struct is an FStateTreeBlueprint*Wrapper whose only FClassProperty is TaskClass /
	 * ConditionClass / EvaluatorClass — so a generic FClassProperty scan recovers the class without including
	 * the wrapper headers. Native (non-BP) nodes have no such property and return empty.
	 */
	static FString ReadNodeClassPath(const FStateTreeEditorNode& EdNode)
	{
		const UScriptStruct* NodeStruct = EdNode.Node.GetScriptStruct();
		const uint8* Mem = EdNode.Node.GetMemory();
		if (!NodeStruct || !Mem) return FString();
		for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
		{
			if (FClassProperty* CP = CastField<FClassProperty>(*It))
			{
				if (UObject* ClassObj = CP->GetObjectPropertyValue_InContainer(Mem))
				{
					if (UClass* C = Cast<UClass>(ClassObj)) return C->GetPathName();
				}
			}
		}
		return FString();
	}

	/**
	 * Find the single FClassProperty on an editor node's Node struct — i.e. the BP wrapper's
	 * TaskClass / ConditionClass / EvaluatorClass. Returns nullptr for native (non-wrapper) nodes.
	 */
	static FClassProperty* FindNodeClassProperty(const FStateTreeEditorNode& EdNode)
	{
		const UScriptStruct* NodeStruct = EdNode.Node.GetScriptStruct();
		if (!NodeStruct) return nullptr;
		for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
		{
			if (FClassProperty* CP = CastField<FClassProperty>(*It)) return CP;
		}
		return nullptr;
	}

	/** Resolve a UClass from a full object path (/Script/... or /Game/....C), a bare name, or a name needing the _C suffix. */
	static UClass* ResolveClass(const FString& In)
	{
		if (In.IsEmpty()) return nullptr;
		if (UClass* C = LoadObject<UClass>(nullptr, *In)) return C;
		if (UClass* C = FindFirstObject<UClass>(*In, EFindFirstObjectOptions::None)) return C;
		if (!In.EndsWith(TEXT("_C")))
		{
			if (UClass* C = FindFirstObject<UClass>(*(In + TEXT("_C")), EFindFirstObjectOptions::None)) return C;
		}
		return nullptr;
	}

	/** Does this node's wrapped class match `FromMatcher` — by full path, by class name, or by name without the BP _C suffix? */
	static bool NodeClassMatches(const FStateTreeEditorNode& EdNode, const FString& FromMatcher)
	{
		const FString Path = ReadNodeClassPath(EdNode);
		if (Path.IsEmpty()) return false;
		if (Path == FromMatcher) return true;

		FString ClassName = Path;
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) ClassName = Path.RightChop(Dot + 1);
		if (ClassName == FromMatcher) return true;
		if (ClassName == FromMatcher + TEXT("_C")) return true;
		return false;
	}

	/**
	 * Repoint one editor node's wrapped class to ToClass and migrate its instance data by name.
	 * BP/C++ task & condition instance data lives in FStateTreeEditorNode::InstanceObject (an instance
	 * of the wrapped class). We set the wrapper's class property, spawn a fresh instance of ToClass
	 * outered to the owning state, and copy matching properties across with the engine's reinstancing
	 * helper — so per-node overrides (e.g. CooldownName) survive the class swap.
	 * Returns true if the node was a wrapper and got repointed; OutOldClass receives the prior class path.
	 * OutRenames (optional) receives oldName->newName for any de-spaced property migration, so the caller
	 * can fix up StateTree property-binding path segments that referenced the old (spaced) property names.
	 */
	static bool RepointNode(FStateTreeEditorNode& EdNode, UClass* ToClass, UObject* InstanceOuter,
		FString& OutOldClass, TMap<FName, FName>* OutRenames = nullptr)
	{
		FClassProperty* CP = FindNodeClassProperty(EdNode);
		if (!CP) return false;
		uint8* Mem = EdNode.Node.GetMutableMemory();
		if (!Mem) return false;

		if (UObject* OldClassObj = CP->GetObjectPropertyValue_InContainer(Mem))
		{
			OutOldClass = OldClassObj->GetPathName();
		}

		CP->SetObjectPropertyValue_InContainer(Mem, ToClass);

		UObject* OldInstance = EdNode.InstanceObject;
		UObject* NewInstance = NewObject<UObject>(InstanceOuter, ToClass);
		if (!NewInstance) { return false; }
		if (OldInstance && NewInstance && GEngine)
		{
			GEngine->CopyPropertiesForUnrelatedObjects(OldInstance, NewInstance);

			// CopyPropertiesForUnrelatedObjects matches by exact FName. Blueprint variables frequently
			// carry SPACES in their FName (e.g. "Search Box Extents") which a C++ identifier can't
			// reproduce, so those overrides would be silently dropped. Second pass: for each property on
			// the new (C++) class, if an old property's space-stripped name matches and the types are
			// compatible, copy the value across. This preserves configured literals through the rename.
			for (TFieldIterator<FProperty> NewIt(NewInstance->GetClass()); NewIt; ++NewIt)
			{
				FProperty* NewProp = *NewIt;
				const FString NewName = NewProp->GetName();
				for (TFieldIterator<FProperty> OldIt(OldInstance->GetClass()); OldIt; ++OldIt)
				{
					FProperty* OldProp = *OldIt;
					const FString OldName = OldProp->GetName();
					if (OldName == NewName) break;                      // exact match already migrated
					if (OldName.Replace(TEXT(" "), TEXT("")) != NewName) continue; // only spaced->despaced pairs
					if (!OldProp->SameType(NewProp)) continue;          // compatible type only
					// Drive the copy from the DESTINATION property: the new (C++) layout owns the
					// target memory, so its copy routine is the one guaranteed to match it.
					NewProp->CopyCompleteValue(
						NewProp->ContainerPtrToValuePtr<void>(NewInstance),
						OldProp->ContainerPtrToValuePtr<void>(OldInstance));
					if (OutRenames)
					{
						OutRenames->Add(OldProp->GetFName(), NewProp->GetFName());
					}
					break;
				}
			}
		}
		EdNode.InstanceObject = NewInstance;
		return true;
	}

	/** Join a binding path's segments into a dotted string (e.g. "Smart Object" or "Struct.Member"). */
	static FString PathToString(const FPropertyBindingPath& Path)
	{
		FString Out;
		for (const FPropertyBindingPathSegment& Seg : Path.GetSegments())
		{
			if (!Out.IsEmpty()) Out += TEXT(".");
			Out += Seg.GetName().ToString();
		}
		return Out;
	}

	/** Serialize a single editor node (task / condition / evaluator): display name, id, node struct, wrapped class. */
	static TSharedRef<FJsonObject> SerializeEdNode(const FStateTreeEditorNode& EdNode)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("name"), EdNode.GetName().ToString());
		J->SetStringField(TEXT("id"),   EdNode.ID.ToString());
		const UScriptStruct* NodeStruct = EdNode.Node.GetScriptStruct();
		J->SetStringField(TEXT("node_struct"), NodeStruct ? NodeStruct->GetName() : FString());
		const FString ClassPath = ReadNodeClassPath(EdNode);
		if (!ClassPath.IsEmpty())
		{
			J->SetStringField(TEXT("class"), ClassPath);
		}
		// The node's configured values (BP/C++ task & condition instance data lives in InstanceObject).
		// Dumping them lets a caller inspect literals like CooldownName and verify a repoint kept them.
		if (EdNode.InstanceObject)
		{
			if (TSharedPtr<FJsonObject> Inst = FNGGHttpServer::UObjectToJson(EdNode.InstanceObject))
			{
				J->SetObjectField(TEXT("instance"), Inst);
			}
		}
		return J;
	}

	static FString EnumValueName(const UEnum* Enum, int64 Value)
	{
		return Enum ? Enum->GetNameStringByValue(Value) : FString();
	}

	/** Serialize a state recursively: name/type + enter_conditions/tasks/transitions/children. */
	static TSharedRef<FJsonObject> SerializeState(UStateTreeState* State)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		if (!State) return J;

		J->SetStringField(TEXT("name"), State->Name.ToString());
		J->SetStringField(TEXT("id"),   State->ID.ToString());
		J->SetStringField(TEXT("type"), EnumValueName(StaticEnum<EStateTreeStateType>(), static_cast<int64>(State->Type)));
		J->SetBoolField  (TEXT("enabled"), State->bEnabled);

		// Enter conditions.
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FStateTreeEditorNode& C : State->EnterConditions)
			{
				Arr.Add(MakeShared<FJsonValueObject>(SerializeEdNode(C)));
			}
			J->SetArrayField(TEXT("enter_conditions"), Arr);
		}

		// Tasks (the single-task slot first when set, then the task array).
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			if (State->SingleTask.Node.IsValid())
			{
				Arr.Add(MakeShared<FJsonValueObject>(SerializeEdNode(State->SingleTask)));
			}
			for (const FStateTreeEditorNode& T : State->Tasks)
			{
				Arr.Add(MakeShared<FJsonValueObject>(SerializeEdNode(T)));
			}
			J->SetArrayField(TEXT("tasks"), Arr);
		}

		// Transitions (trigger + target + any transition conditions).
		{
			const UEnum* TriggerEnum = StaticEnum<EStateTreeTransitionTrigger>();
			const UEnum* LinkEnum    = StaticEnum<EStateTreeTransitionType>();
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FStateTreeTransition& Tr : State->Transitions)
			{
				TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
				TJ->SetStringField(TEXT("trigger"),   EnumValueName(TriggerEnum, static_cast<int64>(Tr.Trigger)));
				TJ->SetStringField(TEXT("to"),        Tr.State.Name.ToString());
				TJ->SetStringField(TEXT("link_type"), EnumValueName(LinkEnum, static_cast<int64>(Tr.State.LinkType)));
				TJ->SetBoolField  (TEXT("enabled"),   Tr.bTransitionEnabled);
				if (Tr.Conditions.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> CArr;
					for (const FStateTreeEditorNode& C : Tr.Conditions)
					{
						CArr.Add(MakeShared<FJsonValueObject>(SerializeEdNode(C)));
					}
					TJ->SetArrayField(TEXT("conditions"), CArr);
				}
				Arr.Add(MakeShared<FJsonValueObject>(TJ));
			}
			J->SetArrayField(TEXT("transitions"), Arr);
		}

		// Children (recurse).
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (UStateTreeState* Child : State->Children)
			{
				if (Child)
				{
					Arr.Add(MakeShared<FJsonValueObject>(SerializeState(Child)));
				}
			}
			J->SetArrayField(TEXT("children"), Arr);
		}

		return J;
	}
}

using namespace NGGStPriv;

// ============================================================================
// Handler: GET /statetree/read_tree?state_tree=/Game/AI/ST_Foo
// (asset_path accepted as an alias). Emits the authoring state hierarchy.
// ============================================================================
bool FNGGHttpServer::HandleStateTreeReadTree(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	FString STPath = GetQueryParam(Req, TEXT("state_tree"));
	if (STPath.IsEmpty())
	{
		STPath = GetQueryParam(Req, TEXT("asset_path"));
	}
	if (STPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("?state_tree=/Game/... (or ?asset_path=) is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, STPath]()
	{
		FString FullObjectPath = STPath;
		if (!FullObjectPath.Contains(TEXT(".")))
		{
			const FString BaseName = FPaths::GetBaseFilename(STPath);
			FullObjectPath = STPath + TEXT(".") + BaseName;
		}

		UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *FullObjectPath);
		if (!StateTree)
		{
			Callback(JsonError(404, FString::Printf(TEXT("UStateTree not found at '%s'"), *STPath)));
			return;
		}

		UStateTreeEditorData* EditorData = GetEditorData(StateTree);
		if (!EditorData)
		{
			Callback(JsonError(500, TEXT("StateTree has no UStateTreeEditorData — cannot read the authoring tree")));
			return;
		}

		TArray<TSharedPtr<FJsonValue>> StatesJson;
		for (UStateTreeState* Root : EditorData->SubTrees)
		{
			if (Root)
			{
				StatesJson.Add(MakeShared<FJsonValueObject>(SerializeState(Root)));
			}
		}

		UE_LOG(LogNGGBridge, Log, TEXT("/statetree/read_tree: %s roots=%d"), *STPath, StatesJson.Num());

		// Property bindings (source -> target paths). Lets a caller see how node outputs feed downstream
		// inputs — essential for validating a repoint of any output-producing task.
		TArray<TSharedPtr<FJsonValue>> BindingsJson;
		if (const FStateTreeEditorPropertyBindings* Bindings = EditorData->GetPropertyEditorBindings())
		{
			for (const FStateTreePropertyPathBinding& B : Bindings->GetBindings())
			{
				TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
				TSharedRef<FJsonObject> Src = MakeShared<FJsonObject>();
				Src->SetStringField(TEXT("struct_id"), B.GetSourcePath().GetStructID().ToString());
				Src->SetStringField(TEXT("path"),      PathToString(B.GetSourcePath()));
				TSharedRef<FJsonObject> Tgt = MakeShared<FJsonObject>();
				Tgt->SetStringField(TEXT("struct_id"), B.GetTargetPath().GetStructID().ToString());
				Tgt->SetStringField(TEXT("path"),      PathToString(B.GetTargetPath()));
				BJ->SetObjectField(TEXT("source"), Src);
				BJ->SetObjectField(TEXT("target"), Tgt);
				BindingsJson.Add(MakeShared<FJsonValueObject>(BJ));
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),    true);
		Out->SetStringField(TEXT("state_tree"), STPath);
		Out->SetArrayField (TEXT("states"),     StatesJson);
		Out->SetArrayField (TEXT("bindings"),   BindingsJson);
		Callback(JsonOk(StSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /statetree/repoint_node
//   Body: { "state_tree": "/Game/AI/ST_Foo",
//           "from_class":  "STC_CheckCooldown_C" | "/Game/AI/STC_CheckCooldown.STC_CheckCooldown_C",
//           "to_class":    "/Script/GameAnimationSample.GameAnimCheckCooldownCondition",
//           "compile": true, "save": false, "dry_run": false }
//
// Rewrites every task/condition/evaluator node whose wrapped class matches `from_class`
// to point at `to_class`, migrating the node's instance properties by name. This is the
// clean alternative to a CoreRedirect: the stored class reference is actually rewritten,
// so there is no perpetual stale-package warning. With dry_run=true it only reports the
// nodes that WOULD be repointed (no mutation). After repointing it runs the editor's
// validate + compile passes, marks the package dirty, and optionally saves.
// ============================================================================
bool FNGGHttpServer::HandleStateTreeRepointNode(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, ParseErr.IsEmpty() ? TEXT("invalid or missing JSON body") : ParseErr));
		return true;
	}

	FString STPath, FromClass, ToClassIn;
	Body->TryGetStringField(TEXT("state_tree"), STPath);
	Body->TryGetStringField(TEXT("from_class"), FromClass);
	Body->TryGetStringField(TEXT("to_class"),   ToClassIn);

	bool bCompile = true;  Body->TryGetBoolField(TEXT("compile"),  bCompile);
	bool bSave    = false; Body->TryGetBoolField(TEXT("save"),     bSave);
	bool bDryRun  = false; Body->TryGetBoolField(TEXT("dry_run"),  bDryRun);

	if (STPath.IsEmpty() || FromClass.IsEmpty() || (ToClassIn.IsEmpty() && !bDryRun))
	{
		Callback(JsonError(400, TEXT("body requires: state_tree, from_class, to_class (to_class optional only when dry_run=true)")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread,
		[Callback, STPath, FromClass, ToClassIn, bCompile, bSave, bDryRun]()
	{
		FString FullObjectPath = STPath;
		if (!FullObjectPath.Contains(TEXT(".")))
		{
			const FString BaseName = FPaths::GetBaseFilename(STPath);
			FullObjectPath = STPath + TEXT(".") + BaseName;
		}

		UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *FullObjectPath);
		if (!StateTree)
		{
			Callback(JsonError(404, FString::Printf(TEXT("UStateTree not found at '%s'"), *STPath)));
			return;
		}

		UStateTreeEditorData* EditorData = GetEditorData(StateTree);
		if (!EditorData)
		{
			Callback(JsonError(500, TEXT("StateTree has no UStateTreeEditorData — cannot edit the authoring tree")));
			return;
		}

		UClass* ToClass = nullptr;
		if (!bDryRun)
		{
			ToClass = ResolveClass(ToClassIn);
			if (!ToClass)
			{
				Callback(JsonError(404, FString::Printf(TEXT("to_class could not be resolved: '%s'"), *ToClassIn)));
				return;
			}
		}

		if (!bDryRun)
		{
			StateTree->Modify();
			EditorData->Modify();
		}

		TArray<TSharedPtr<FJsonValue>> Repointed;
		int32 Count = 0;
		// Per repointed node: oldName->newName for any de-spaced property migration, keyed by node ID.
		// Used after the walk to fix up the property-binding path segments that referenced those names.
		TMap<FGuid, TMap<FName, FName>> NodeRenames;

		TFunction<void(UStateTreeState*)> Walk = [&](UStateTreeState* State)
		{
			if (!State) return;

			auto Process = [&](FStateTreeEditorNode& Node, const TCHAR* Kind)
			{
				if (!NodeClassMatches(Node, FromClass)) return;

				FString OldClass;
				TMap<FName, FName> Renames;
				if (bDryRun)
				{
					OldClass = ReadNodeClassPath(Node);
				}
				else if (!RepointNode(Node, ToClass, State, OldClass, &Renames))
				{
					return; // not a wrapper node — should not happen after a class match
				}
				if (Renames.Num() > 0)
				{
					NodeRenames.Add(Node.ID, MoveTemp(Renames));
				}

				++Count;
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				D->SetStringField(TEXT("state"), State->Name.ToString());
				D->SetStringField(TEXT("kind"),  Kind);
				D->SetStringField(TEXT("node"),  Node.GetName().ToString());
				D->SetStringField(TEXT("from"),  OldClass);
				D->SetStringField(TEXT("to"),    bDryRun ? ToClassIn : ToClass->GetPathName());
				Repointed.Add(MakeShared<FJsonValueObject>(D));
			};

			for (FStateTreeEditorNode& N : State->EnterConditions) Process(N, TEXT("enter_condition"));
			if (State->SingleTask.Node.IsValid())                 Process(State->SingleTask, TEXT("task"));
			for (FStateTreeEditorNode& N : State->Tasks)          Process(N, TEXT("task"));
			for (FStateTreeTransition& Tr : State->Transitions)
			{
				for (FStateTreeEditorNode& N : Tr.Conditions)     Process(N, TEXT("transition_condition"));
			}
			for (UStateTreeState* Child : State->Children)        Walk(Child);
		};

		for (UStateTreeState* Root : EditorData->SubTrees) Walk(Root);

		bool bCompiled = false;
		bool bDidSave  = false;
		FString SaveError;
		int32 BindingSegmentsRewritten = 0;
		if (!bDryRun && Count > 0)
		{
			// Migrate property-binding path segments that referenced a repointed node's renamed (de-spaced)
			// property, e.g. a binding sourced from the old "Smart Object" output -> the new "SmartObject".
			// This MUST run before ValidateStateTree, which would otherwise drop the now-dangling bindings.
			if (NodeRenames.Num() > 0)
			{
				if (FStateTreeEditorPropertyBindings* Bindings = EditorData->GetPropertyEditorBindings())
				{
					auto FixPath = [&NodeRenames, &BindingSegmentsRewritten](FPropertyBindingPath& Path)
					{
						const TMap<FName, FName>* Renames = NodeRenames.Find(Path.GetStructID());
						if (!Renames) return;
						for (FPropertyBindingPathSegment& Seg : Path.GetMutableSegments())
						{
							if (const FName* NewName = Renames->Find(Seg.GetName()))
							{
								Seg.SetName(*NewName);
								++BindingSegmentsRewritten;
							}
						}
					};
					for (FStateTreePropertyPathBinding& Binding : Bindings->GetMutableBindings())
					{
						FixPath(Binding.GetMutableSourcePath());
						FixPath(Binding.GetMutableTargetPath());
					}
				}
			}

			// Safety-net fixup (re-outers/uniquifies instances, fixes state links), then compile.
			UStateTreeEditingSubsystem::ValidateStateTree(StateTree);

			if (bCompile)
			{
				FStateTreeCompilerLog Log;
				bCompiled = UStateTreeEditingSubsystem::CompileStateTree(StateTree, Log);
				if (!bCompiled)
				{
					Log.DumpToLog(LogNGGBridge);
				}
			}

			StateTree->MarkPackageDirty();

			// Never persist a tree that failed to compile — that would write a broken asset to disk.
			if (bSave && (!bCompile || bCompiled))
			{
				bDidSave = UEditorLoadingAndSavingUtils::SavePackages({ StateTree->GetOutermost() }, /*bOnlyDirty=*/false);
				if (!bDidSave)
				{
					SaveError = FString::Printf(TEXT("SavePackages failed for '%s'"), *StateTree->GetOutermost()->GetName());
				}
			}
		}

		UE_LOG(LogNGGBridge, Log, TEXT("/statetree/repoint_node: %s '%s' -> '%s' repointed=%d dry_run=%d compiled=%d saved=%d bindings_rewritten=%d"),
			*STPath, *FromClass, *ToClassIn, Count, bDryRun ? 1 : 0, bCompiled ? 1 : 0, bDidSave ? 1 : 0, BindingSegmentsRewritten);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         SaveError.IsEmpty());
		Out->SetBoolField  (TEXT("dry_run"),         bDryRun);
		Out->SetStringField(TEXT("state_tree"),      STPath);
		Out->SetStringField(TEXT("from_class"),      FromClass);
		Out->SetStringField(TEXT("to_class"),        ToClassIn);
		Out->SetNumberField(TEXT("repointed_count"), Count);
		Out->SetArrayField (TEXT("repointed"),       Repointed);
		if (!bDryRun && Count > 0)
		{
			if (bCompile) Out->SetBoolField(TEXT("compiled"), bCompiled);
			Out->SetBoolField  (TEXT("saved"), bDidSave);
			if (!SaveError.IsEmpty()) Out->SetStringField(TEXT("save_error"), SaveError);
			Out->SetNumberField(TEXT("bindings_rewritten"), BindingSegmentsRewritten);
		}
		Callback(JsonOk(StSerializeJson(Out)));
	});

	return true;
}
