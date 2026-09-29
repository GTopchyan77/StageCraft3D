// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGBehaviorTree.cpp
//
// Implementation of the /bt/* HTTP endpoints that expose Unreal Engine 5
// Behavior Tree and Blackboard authoring to MCP clients.
//
// Phase 1 — asset creation only:
//   POST /bt/create_tree            — create a UBehaviorTree, optionally link a Blackboard
//   POST /bt/create_blackboard      — create a UBlackboardData
//   POST /bt/add_blackboard_keys    — append typed entries to a UBlackboardData
//
// Custom task / decorator / service authoring is handled today via the
// existing /editor/create_blueprint endpoint with parent_class set to
// BTTask_BlueprintBase / BTDecorator_BlueprintBase / BTService_BlueprintBase.
// Phase 2 will add /bt/add_logic for graph wiring.
//
// Threading contract:
//   - All UObject mutation runs on the Game Thread via AsyncTask.
//   - Handlers themselves return true synchronously after dispatching the
//     async work; the OnComplete callback fires from inside the GT lambda.

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- HTTP / JSON ----------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ---- Engine ---------------------------------------------------------------
#include "Async/Async.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

// ---- Asset Tools / Asset Registry -----------------------------------------
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ---- Behavior Tree runtime (AIModule) -------------------------------------
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"

// ---- Behavior Tree editor factories (BehaviorTreeEditor) ------------------
#include "BehaviorTreeFactory.h"
#include "BlackboardDataFactory.h"

// ---- Behavior Tree runtime nodes (Phase 2) --------------------------------
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"

// ---- Behavior Tree EdGraph nodes (Phase 2) --------------------------------
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "EdGraphSchema_BehaviorTree.h"   // schema for the BT EdGraph
#include "AIGraphTypes.h"                 // FGraphNodeClassData
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "UObject/UObjectIterator.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace NGGBtPriv
{
	static FString BtSerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** Split "/Game/AI/BT_Foo" into PackagePath="/Game/AI" + AssetName="BT_Foo". */
	static bool BtSplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
	{
		int32 LastSlash = INDEX_NONE;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash)) return false;
		OutPackagePath = AssetPath.Left(LastSlash);
		OutAssetName   = AssetPath.Mid(LastSlash + 1);
		return !OutAssetName.IsEmpty();
	}

	/** Save the package containing Asset to disk. Returns true on success. */
	static bool BtSavePackageForAsset(UObject* Asset)
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

	/** Map a case-insensitive type string ("Bool", "Float", "Object", …) to a UBlackboardKeyType subclass. */
	static UClass* ResolveBlackboardKeyTypeClass(const FString& TypeStr)
	{
		const FString T = TypeStr.TrimStartAndEnd().ToLower();
		if (T == TEXT("bool"))    return UBlackboardKeyType_Bool::StaticClass();
		if (T == TEXT("int")
		 || T == TEXT("int32")
		 || T == TEXT("integer")) return UBlackboardKeyType_Int::StaticClass();
		if (T == TEXT("float")
		 || T == TEXT("double")
		 || T == TEXT("real"))    return UBlackboardKeyType_Float::StaticClass();
		if (T == TEXT("vector")
		 || T == TEXT("fvector")) return UBlackboardKeyType_Vector::StaticClass();
		if (T == TEXT("rotator")
		 || T == TEXT("frotator")) return UBlackboardKeyType_Rotator::StaticClass();
		if (T == TEXT("object")
		 || T == TEXT("uobject")) return UBlackboardKeyType_Object::StaticClass();
		if (T == TEXT("class"))   return UBlackboardKeyType_Class::StaticClass();
		if (T == TEXT("enum"))    return UBlackboardKeyType_Enum::StaticClass();
		if (T == TEXT("name")
		 || T == TEXT("fname"))   return UBlackboardKeyType_Name::StaticClass();
		if (T == TEXT("string")
		 || T == TEXT("fstring")) return UBlackboardKeyType_String::StaticClass();
		return nullptr;
	}

	/** Resolve a class ref string ("Actor", "/Script/Engine.Pawn", "/Game/Foo/BP_Bar") to a UClass*. */
	static UClass* ResolveClassRef(const FString& Ref)
	{
		if (Ref.IsEmpty()) return nullptr;

		// Full /Script/ or /Game/ path.
		if (Ref.StartsWith(TEXT("/Script/")) || Ref.StartsWith(TEXT("/Game/")) || Ref.StartsWith(TEXT("/Engine/")))
		{
			if (UClass* C = LoadObject<UClass>(nullptr, *Ref)) return C;
			// Try with _C suffix for blueprint-generated classes.
			const FString WithSuffix = Ref.EndsWith(TEXT("_C")) ? Ref : (Ref + TEXT("_C"));
			if (UClass* C = LoadObject<UClass>(nullptr, *WithSuffix)) return C;
			return nullptr;
		}

		// Short engine class name fallback.
		if (UClass* C = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Ref))) return C;
		if (UClass* C = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/CoreUObject.%s"), *Ref))) return C;
		return nullptr;
	}

	/** Find the existing UBehaviorTreeGraphNode_Root in a BT graph (factory creates one). */
	static UBehaviorTreeGraphNode_Root* FindRootGraphNode(UBehaviorTreeGraph* BTGraph)
	{
		if (!BTGraph) return nullptr;
		for (UEdGraphNode* N : BTGraph->Nodes)
		{
			if (auto* Root = Cast<UBehaviorTreeGraphNode_Root>(N))
			{
				return Root;
			}
		}
		return nullptr;
	}

	/**
	 * Lazy-init the BT's EdGraph + Root node.
	 *
	 * UBehaviorTreeFactory creates the asset's runtime data but does NOT
	 * materialise the EdGraph (UBehaviorTree::BTGraph) — that normally happens
	 * the first time the asset is opened in the BT editor (FBehaviorTreeEditor::
	 * InitBehaviorTreeEditor). We do it manually so headless authoring works
	 * without ever opening the editor for that asset.
	 *
	 * Returns the BTGraph (whether pre-existing or freshly created) plus the
	 * (possibly freshly created) Root node. Returns null on failure.
	 */
	static UBehaviorTreeGraph* EnsureBTGraph(UBehaviorTree* BT, UBehaviorTreeGraphNode_Root** OutRoot = nullptr)
	{
		if (!BT) return nullptr;

		UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
		if (!BTGraph)
		{
			BTGraph = NewObject<UBehaviorTreeGraph>(
				BT, UBehaviorTreeGraph::StaticClass(),
				FName(TEXT("BehaviorTreeGraph")), RF_Transactional);
			BTGraph->Schema = UEdGraphSchema_BehaviorTree::StaticClass();
			BTGraph->bAllowDeletion = false;
			BT->BTGraph = BTGraph;
			BTGraph->OnCreated();
		}

		UBehaviorTreeGraphNode_Root* Root = FindRootGraphNode(BTGraph);
		if (!Root)
		{
			Root = NewObject<UBehaviorTreeGraphNode_Root>(BTGraph);
			BTGraph->AddNode(Root, /*bUserAction*/ false, /*bSelectNewNode*/ false);
			Root->CreateNewGuid();
			Root->PostPlacedNewNode();
			Root->AllocateDefaultPins();
			Root->NodePosX = 0;
			Root->NodePosY = 0;
		}

		if (OutRoot) *OutRoot = Root;
		return BTGraph;
	}

	/** Apply per-type properties (e.g. Object.BaseClass) to a freshly constructed key-type instance. */
	static void ApplyKeyTypeProperties(UBlackboardKeyType* KeyType, const TSharedPtr<FJsonObject>& Spec)
	{
		if (!KeyType || !Spec.IsValid()) return;

		if (auto* AsObject = Cast<UBlackboardKeyType_Object>(KeyType))
		{
			FString BaseClass;
			if (Spec->TryGetStringField(TEXT("base_class"), BaseClass) && !BaseClass.IsEmpty())
			{
				if (UClass* C = ResolveClassRef(BaseClass)) AsObject->BaseClass = C;
			}
		}
		else if (auto* AsClass = Cast<UBlackboardKeyType_Class>(KeyType))
		{
			FString BaseClass;
			if (Spec->TryGetStringField(TEXT("base_class"), BaseClass) && !BaseClass.IsEmpty())
			{
				if (UClass* C = ResolveClassRef(BaseClass)) AsClass->BaseClass = C;
			}
		}
		else if (auto* AsEnum = Cast<UBlackboardKeyType_Enum>(KeyType))
		{
			FString EnumPath;
			if (Spec->TryGetStringField(TEXT("enum_path"), EnumPath) && !EnumPath.IsEmpty())
			{
				if (UEnum* E = LoadObject<UEnum>(nullptr, *EnumPath))
				{
					AsEnum->EnumType = E;
					AsEnum->EnumName = E->GetName();
					AsEnum->bIsEnumNameValid = true;
				}
			}
		}
	}
}

using namespace NGGBtPriv;

// ============================================================================
// Handler: POST /bt/create_tree
// Body: { asset_path, blackboard_path?, save? }
// Creates a UBehaviorTree at asset_path. If blackboard_path is supplied and
// the referenced UBlackboardData exists, it is linked as the tree's
// BlackboardAsset (the same field the editor populates from the BT root node's
// "Blackboard Asset" picker). If the BT already exists, returns success with
// already_existed=true; the existing blackboard link is left untouched unless
// blackboard_path is supplied (then it is overwritten).
// ============================================================================
bool FNGGHttpServer::HandleBtCreateTree(
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

	FString BlackboardPath;
	Body->TryGetStringField(TEXT("blackboard_path"), BlackboardPath);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, BlackboardPath, bSave]()
	{
		FString PackagePath, AssetName;
		if (!BtSplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}

		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;

		// Resolve blackboard early so we can fail before any asset mutation.
		UBlackboardData* BBAsset = nullptr;
		if (!BlackboardPath.IsEmpty())
		{
			FString BBObjectPath = BlackboardPath;
			if (!BBObjectPath.Contains(TEXT(".")))
			{
				const FString BBName = FPaths::GetBaseFilename(BlackboardPath);
				BBObjectPath = BlackboardPath + TEXT(".") + BBName;
			}
			BBAsset = LoadObject<UBlackboardData>(nullptr, *BBObjectPath);
			if (!BBAsset)
			{
				Callback(JsonError(404, FString::Printf(
					TEXT("blackboard_path '%s' did not resolve to a UBlackboardData asset"), *BlackboardPath)));
				return;
			}
		}

		// Already exists?
		if (UBehaviorTree* Existing = LoadObject<UBehaviorTree>(nullptr, *FullObjectPath))
		{
			bool bLinkedNow = false;
			if (BBAsset && Existing->BlackboardAsset != BBAsset)
			{
				Existing->BlackboardAsset = BBAsset;
				Existing->MarkPackageDirty();
				bLinkedNow = true;
			}
			bool bSaved = false;
			if (bSave && bLinkedNow) bSaved = BtSavePackageForAsset(Existing);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField  (TEXT("success"),         true);
			Out->SetBoolField  (TEXT("already_existed"), true);
			Out->SetStringField(TEXT("asset_path"),      AssetPath);
			Out->SetStringField(TEXT("blackboard_path"), BBAsset ? BBAsset->GetPathName() : FString());
			Out->SetBoolField  (TEXT("blackboard_linked_now"), bLinkedNow);
			Out->SetBoolField  (TEXT("saved"),           bSaved);
			Callback(JsonOk(BtSerializeJson(Out)));
			return;
		}

		// Create via the editor factory so the EdGraph gets initialised the same
		// way it would be from the New Asset menu.
		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		UBehaviorTreeFactory* Factory = NewObject<UBehaviorTreeFactory>();

		UObject* NewAsset = AssetToolsModule.Get().CreateAsset(
			AssetName, PackagePath, UBehaviorTree::StaticClass(), Factory);
		UBehaviorTree* BT = Cast<UBehaviorTree>(NewAsset);
		if (!BT)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateAsset failed for behavior tree '%s'"), *AssetPath)));
			return;
		}

		bool bLinkedNow = false;
		if (BBAsset)
		{
			BT->BlackboardAsset = BBAsset;
			bLinkedNow = true;
		}

		// Materialise the EdGraph + root now so headless authoring via
		// /bt/add_logic works without anyone first opening the asset in the
		// BT editor. The factory leaves BTGraph null otherwise.
		EnsureBTGraph(BT);

		BT->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(BT);

		bool bSaved = false;
		if (bSave) bSaved = BtSavePackageForAsset(BT);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bt/create_tree: %s blackboard='%s' saved=%d"),
			*AssetPath, *(BBAsset ? BBAsset->GetPathName() : FString()), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),               true);
		Out->SetBoolField  (TEXT("already_existed"),       false);
		Out->SetStringField(TEXT("asset_path"),            AssetPath);
		Out->SetStringField(TEXT("blackboard_path"),       BBAsset ? BBAsset->GetPathName() : FString());
		Out->SetBoolField  (TEXT("blackboard_linked_now"), bLinkedNow);
		Out->SetBoolField  (TEXT("saved"),                 bSaved);
		Callback(JsonCreated(BtSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bt/create_blackboard
// Body: { asset_path, parent_blackboard_path?, save? }
// Creates a UBlackboardData asset. If parent_blackboard_path is supplied, the
// new asset inherits keys from that parent (matches the editor's "Parent"
// picker on the Blackboard root).
// ============================================================================
bool FNGGHttpServer::HandleBtCreateBlackboard(
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

	FString ParentPath;
	Body->TryGetStringField(TEXT("parent_blackboard_path"), ParentPath);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, ParentPath, bSave]()
	{
		FString PackagePath, AssetName;
		if (!BtSplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}

		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;

		UBlackboardData* Parent = nullptr;
		if (!ParentPath.IsEmpty())
		{
			FString ParentObjectPath = ParentPath;
			if (!ParentObjectPath.Contains(TEXT(".")))
			{
				const FString PName = FPaths::GetBaseFilename(ParentPath);
				ParentObjectPath = ParentPath + TEXT(".") + PName;
			}
			Parent = LoadObject<UBlackboardData>(nullptr, *ParentObjectPath);
			if (!Parent)
			{
				Callback(JsonError(404, FString::Printf(
					TEXT("parent_blackboard_path '%s' did not resolve"), *ParentPath)));
				return;
			}
		}

		// Already exists?
		if (UBlackboardData* Existing = LoadObject<UBlackboardData>(nullptr, *FullObjectPath))
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField  (TEXT("success"),         true);
			Out->SetBoolField  (TEXT("already_existed"), true);
			Out->SetStringField(TEXT("asset_path"),      AssetPath);
			Out->SetNumberField(TEXT("key_count"),       Existing->Keys.Num());
			Callback(JsonOk(BtSerializeJson(Out)));
			return;
		}

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		UBlackboardDataFactory* Factory = NewObject<UBlackboardDataFactory>();

		UObject* NewAsset = AssetToolsModule.Get().CreateAsset(
			AssetName, PackagePath, UBlackboardData::StaticClass(), Factory);
		UBlackboardData* BBData = Cast<UBlackboardData>(NewAsset);
		if (!BBData)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CreateAsset failed for blackboard '%s'"), *AssetPath)));
			return;
		}

		if (Parent)
		{
			BBData->Parent = Parent;
		}

		BBData->UpdateKeyIDs();
		BBData->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(BBData);

		bool bSaved = false;
		if (bSave) bSaved = BtSavePackageForAsset(BBData);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bt/create_blackboard: %s parent='%s' saved=%d"),
			*AssetPath, *(Parent ? Parent->GetPathName() : FString()), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),                true);
		Out->SetBoolField  (TEXT("already_existed"),        false);
		Out->SetStringField(TEXT("asset_path"),             AssetPath);
		Out->SetStringField(TEXT("parent_blackboard_path"), Parent ? Parent->GetPathName() : FString());
		Out->SetNumberField(TEXT("key_count"),              BBData->Keys.Num());
		Out->SetBoolField  (TEXT("saved"),                  bSaved);
		Callback(JsonCreated(BtSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bt/add_blackboard_keys
// Body: {
//   asset_path,
//   keys: [
//     { name, type, description?, instance_synced?,
//       base_class? (Object/Class), enum_path? (Enum) },
//     ...
//   ],
//   save?
// }
// Appends typed keys to a UBlackboardData. Skips entries whose name is already
// taken (case-sensitive on FName) so the call is idempotent. Calls
// UpdateKeyIDs + PropagateKeyChangesToDerivedBlackboardAssets so child
// blackboards stay consistent.
// ============================================================================
bool FNGGHttpServer::HandleBtAddBlackboardKeys(
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

	const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
	if (!Body->TryGetArrayField(TEXT("keys"), KeysArr) || !KeysArr || KeysArr->Num() == 0)
	{
		Callback(JsonError(400, TEXT("keys must be a non-empty array")));
		return true;
	}

	// Capture the JSON entries as shared pointers so the lambda can use
	// per-type fields like base_class / enum_path on the Game Thread.
	struct FKeySpec
	{
		FString Name;
		FString TypeStr;
		FString Description;
		bool    bInstanceSynced = false;
		TSharedPtr<FJsonObject> Raw;
	};
	TArray<FKeySpec> Specs;
	Specs.Reserve(KeysArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *KeysArr)
	{
		const TSharedPtr<FJsonObject>& Obj = V->AsObject();
		if (!Obj.IsValid()) continue;

		FKeySpec K;
		K.Raw = Obj;
		Obj->TryGetStringField(TEXT("name"), K.Name);
		Obj->TryGetStringField(TEXT("type"), K.TypeStr);
		Obj->TryGetStringField(TEXT("description"), K.Description);
		Obj->TryGetBoolField  (TEXT("instance_synced"), K.bInstanceSynced);

		if (!K.Name.IsEmpty() && !K.TypeStr.IsEmpty())
		{
			Specs.Add(MoveTemp(K));
		}
	}

	if (Specs.Num() == 0)
	{
		Callback(JsonError(400, TEXT("no valid {name,type} entries supplied in keys[]")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, AssetPath, Specs, bSave]()
	{
		FString PackagePath, AssetName;
		if (!BtSplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			Callback(JsonError(400, TEXT("asset_path must be a content path with at least one '/'")));
			return;
		}
		const FString FullObjectPath = AssetPath + TEXT(".") + AssetName;

		UBlackboardData* BBData = LoadObject<UBlackboardData>(nullptr, *FullObjectPath);
		if (!BBData)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("UBlackboardData not found at '%s'"), *AssetPath)));
			return;
		}

		TArray<TSharedPtr<FJsonValue>> AddedJson;
		TArray<TSharedPtr<FJsonValue>> SkippedJson;

		for (const FKeySpec& S : Specs)
		{
			const FName EntryName(*S.Name);

			// Reject duplicates against the blackboard itself AND its parent chain;
			// UBlackboardData::IsValidKey traverses parents.
			bool bExists = false;
			for (const FBlackboardEntry& Existing : BBData->Keys)
			{
				if (Existing.EntryName == EntryName) { bExists = true; break; }
			}
			if (bExists)
			{
				TSharedRef<FJsonObject> Skip = MakeShared<FJsonObject>();
				Skip->SetStringField(TEXT("name"),   S.Name);
				Skip->SetStringField(TEXT("reason"), TEXT("already_exists"));
				SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
				continue;
			}

			UClass* KeyTypeClass = ResolveBlackboardKeyTypeClass(S.TypeStr);
			if (!KeyTypeClass)
			{
				TSharedRef<FJsonObject> Skip = MakeShared<FJsonObject>();
				Skip->SetStringField(TEXT("name"),   S.Name);
				Skip->SetStringField(TEXT("reason"), FString::Printf(
					TEXT("unknown type '%s' (supported: Bool, Int, Float, Vector, Rotator, Object, Class, Enum, Name, String)"),
					*S.TypeStr));
				SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
				continue;
			}

			// Owner = the BB asset so the key type is part of the package.
			UBlackboardKeyType* KeyType = NewObject<UBlackboardKeyType>(BBData, KeyTypeClass);
			if (!KeyType)
			{
				TSharedRef<FJsonObject> Skip = MakeShared<FJsonObject>();
				Skip->SetStringField(TEXT("name"),   S.Name);
				Skip->SetStringField(TEXT("reason"), FString::Printf(
					TEXT("failed to allocate UBlackboardKeyType of class '%s'"), *KeyTypeClass->GetName()));
				SkippedJson.Add(MakeShared<FJsonValueObject>(Skip));
				continue;
			}
			ApplyKeyTypeProperties(KeyType, S.Raw);

			FBlackboardEntry Entry;
			Entry.EntryName        = EntryName;
			Entry.EntryDescription = S.Description;
			Entry.KeyType          = KeyType;
			Entry.bInstanceSynced  = S.bInstanceSynced ? 1 : 0;
			BBData->Keys.Add(MoveTemp(Entry));

			TSharedRef<FJsonObject> Added = MakeShared<FJsonObject>();
			Added->SetStringField(TEXT("name"),  S.Name);
			Added->SetStringField(TEXT("type"),  KeyTypeClass->GetName());
			Added->SetBoolField  (TEXT("instance_synced"), S.bInstanceSynced);
			AddedJson.Add(MakeShared<FJsonValueObject>(Added));
		}

		BBData->UpdateKeyIDs();
		BBData->PropagateKeyChangesToDerivedBlackboardAssets();
		BBData->PostEditChange();
		BBData->MarkPackageDirty();

		bool bSaved = false;
		if (bSave) bSaved = BtSavePackageForAsset(BBData);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bt/add_blackboard_keys: %s added=%d skipped=%d saved=%d"),
			*AssetPath, AddedJson.Num(), SkippedJson.Num(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),    true);
		Out->SetStringField(TEXT("asset_path"), AssetPath);
		Out->SetArrayField (TEXT("added"),      AddedJson);
		Out->SetArrayField (TEXT("skipped"),    SkippedJson);
		Out->SetNumberField(TEXT("key_count"),  BBData->Keys.Num());
		Out->SetBoolField  (TEXT("saved"),      bSaved);
		Callback(JsonOk(BtSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Phase 2 — graph wiring helpers
// ============================================================================

namespace NGGBtPriv
{
	/** Resolve a composite alias ("Selector", "Sequence", "SimpleParallel") to a UBTCompositeNode subclass. */
	static UClass* ResolveCompositeClass(const FString& Name)
	{
		const FString N = Name.TrimStartAndEnd().ToLower();
		if (N == TEXT("selector"))        return UBTComposite_Selector::StaticClass();
		if (N == TEXT("sequence"))        return UBTComposite_Sequence::StaticClass();
		if (N == TEXT("simpleparallel")
		 || N == TEXT("simple_parallel")
		 || N == TEXT("parallel"))        return UBTComposite_SimpleParallel::StaticClass();
		return nullptr;
	}

	/**
	 * Resolve a task/decorator/service class string to a UClass*. Accepts:
	 *  - /Script/Module.ClassName  (full path)
	 *  - /Game/Path/BP_ClassName   (BP class — _C suffix auto-appended if missing)
	 *  - ShortName                 (tries /Script/AIModule.<Name>, then UObjectIterator scan)
	 * RequiredBase filters to a specific base class so we don't mis-bind unrelated UClasses.
	 */
	static UClass* ResolveBTNodeClass(const FString& Ref, UClass* RequiredBase)
	{
		if (Ref.IsEmpty() || !RequiredBase) return nullptr;

		auto Validate = [RequiredBase](UClass* C) -> UClass*
		{
			return (C && C->IsChildOf(RequiredBase)) ? C : nullptr;
		};

		// Full path
		if (Ref.StartsWith(TEXT("/")))
		{
			if (UClass* C = Validate(LoadObject<UClass>(nullptr, *Ref))) return C;
			const FString WithSuffix = Ref.EndsWith(TEXT("_C")) ? Ref : (Ref + TEXT("_C"));
			if (UClass* C = Validate(LoadObject<UClass>(nullptr, *WithSuffix))) return C;
			return nullptr;
		}

		// Short name in /Script/AIModule
		const FString AsAIModule = FString::Printf(TEXT("/Script/AIModule.%s"), *Ref);
		if (UClass* C = Validate(LoadObject<UClass>(nullptr, *AsAIModule))) return C;

		// Last resort: UClass iteration
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(RequiredBase) && It->GetName() == Ref)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/**
	 * If the runtime node has a "BlackboardKey" UPROPERTY of type FBlackboardKeySelector,
	 * set its SelectedKeyName and resolve against the BB. Returns true if applied.
	 * BB may be null — in that case we set the name but skip Resolve (resolve at load).
	 */
	static bool ApplyBlackboardKeyShorthand(
		UObject* RuntimeNode, const FString& KeyName, UBlackboardData* BB)
	{
		if (!RuntimeNode || KeyName.IsEmpty()) return false;

		FProperty* Prop = RuntimeNode->GetClass()->FindPropertyByName(TEXT("BlackboardKey"));
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || StructProp->Struct != FBlackboardKeySelector::StaticStruct())
		{
			return false;
		}

		FBlackboardKeySelector* Sel = StructProp->ContainerPtrToValuePtr<FBlackboardKeySelector>(RuntimeNode);
		if (!Sel) return false;

		Sel->SelectedKeyName = FName(*KeyName);
		if (BB)
		{
			Sel->ResolveSelectedKey(*BB);
		}
		return true;
	}

	/**
	 * Walk a Blackboard's local Keys plus its parent chain looking for an entry
	 * by name. Returns true if found anywhere up the chain — matching how
	 * UBlackboardData::IsValid resolves at runtime.
	 */
	static bool BlackboardHasKey(UBlackboardData* BB, const FName& KeyName)
	{
		for (UBlackboardData* Cur = BB; Cur; Cur = Cur->Parent)
		{
			for (const FBlackboardEntry& E : Cur->Keys)
			{
				if (E.EntryName == KeyName) return true;
			}
		}
		return false;
	}

	/**
	 * Read the SelectedKeyName off a node's BlackboardKey field, if present.
	 * Returns NAME_None if the node has no FBlackboardKeySelector field, or if
	 * the selector is unset.
	 */
	static FName ReadBlackboardKeyName(UObject* RuntimeNode)
	{
		if (!RuntimeNode) return NAME_None;
		FProperty* Prop = RuntimeNode->GetClass()->FindPropertyByName(TEXT("BlackboardKey"));
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || StructProp->Struct != FBlackboardKeySelector::StaticStruct())
		{
			return NAME_None;
		}
		const FBlackboardKeySelector* Sel = StructProp->ContainerPtrToValuePtr<FBlackboardKeySelector>(RuntimeNode);
		return Sel ? Sel->SelectedKeyName : NAME_None;
	}

	// Forward decls — used by SerializeChangedProperties below before they're defined later.
	static bool IsValueOrBBKeyStruct(const FStructProperty* StructProp);
	static TSharedPtr<FJsonValue> TryReadValueOrBBKey(const FStructProperty* StructProp,
		const void* StructPtr, const void* CDOStructPtr);

	/**
	 * Emit a JSON object of all UPROPERTYs on RuntimeNode that differ from the
	 * class CDO. Skips transient and the BlackboardKey selector (the latter is
	 * surfaced separately as the top-level "blackboard_key" shorthand to keep
	 * read → edit → write round-trips clean).
	 */
	static TSharedPtr<FJsonObject> SerializeChangedProperties(UObject* RuntimeNode)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!RuntimeNode) return Out;

		UClass* Cls = RuntimeNode->GetClass();
		UObject* CDO = Cls->GetDefaultObject();
		if (!CDO) return Out;

		for (TFieldIterator<FProperty> It(Cls); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop) continue;

			// Only edit-exposed, blueprint-visible-or-editable user-tunable fields.
			const bool bEditable = Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly);
			if (!bEditable) continue;
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_DisableEditOnInstance)) continue;

			// Skip the BlackboardKey selector — it's emitted as the top-level shorthand.
			if (Prop->GetFName() == FName(TEXT("BlackboardKey"))) continue;

			const void* InstancePtr = Prop->ContainerPtrToValuePtr<void>(RuntimeNode);
			const void* CDOPtr      = Prop->ContainerPtrToValuePtr<void>(CDO);

			// Special-case FValueOrBBKey_X: emit just the literal value or the
			// "@KeyName" binding instead of the full struct, for clean round-trip.
			if (FStructProperty* SP = CastField<FStructProperty>(Prop); SP && IsValueOrBBKeyStruct(SP))
			{
				if (TSharedPtr<FJsonValue> ValOrKey = TryReadValueOrBBKey(SP, InstancePtr, CDOPtr))
				{
					Out->SetField(Prop->GetName(), ValOrKey);
				}
				continue;
			}

			if (Prop->Identical(InstancePtr, CDOPtr, PPF_DeepComparison)) continue;

			TSharedPtr<FJsonValue> ValJson = FNGGHttpServer::PropertyToJson(Prop, RuntimeNode);
			if (ValJson.IsValid())
			{
				Out->SetField(Prop->GetName(), ValJson);
			}
		}
		return Out;
	}

	/**
	 * Detect FValueOrBBKey_Float / _Int32 / _Bool / _Name / _String / _Vector /
	 * _Rotator / _Object / _Class / _Enum / _Struct — UE5.4+ tagged-union types
	 * that wrap a literal value plus an optional Blackboard key reference. They
	 * have a "DefaultValue" UPROPERTY (per-type) and inherit "Key" (FName) from
	 * FValueOrBlackboardKeyBase. Both reflection-accessible; the generic
	 * SetPropertyFromJson can't deserialise them from a plain JSON scalar.
	 */
	static bool IsValueOrBBKeyStruct(const FStructProperty* StructProp)
	{
		if (!StructProp || !StructProp->Struct) return false;
		return StructProp->Struct->GetName().StartsWith(TEXT("ValueOrBBKey_"));
	}

	/** Convert a JSON scalar to its ImportText string form. Returns false for unsupported types. */
	static bool JsonScalarToText(const TSharedPtr<FJsonValue>& V, FString& OutText)
	{
		if (!V.IsValid()) return false;
		switch (V->Type)
		{
			case EJson::Number:  OutText = FString::SanitizeFloat(V->AsNumber()); return true;
			case EJson::String:  OutText = V->AsString(); return true;
			case EJson::Boolean: OutText = V->AsBool() ? TEXT("true") : TEXT("false"); return true;
			default:             return false;
		}
	}

	/**
	 * Set an FValueOrBBKey_X struct from a user-supplied JSON value.
	 *
	 *   1.5            → write into DefaultValue field (literal mode)
	 *   "@KeyName"     → bind to Blackboard key (sets Key, leaves DefaultValue alone)
	 *   "PlainString"  → write into DefaultValue (for FValueOrBBKey_String / _Name)
	 *
	 * Returns true if successfully applied. OutErr filled on failure.
	 */
	static bool TrySetValueOrBBKey(const FStructProperty* StructProp, void* StructPtr,
		const TSharedPtr<FJsonValue>& JsonValue, FString& OutErr)
	{
		if (!StructProp || !StructPtr || !JsonValue.IsValid())
		{
			OutErr = TEXT("null parameter to TrySetValueOrBBKey");
			return false;
		}

		// "@KeyName" string → bind to BB key.
		if (JsonValue->Type == EJson::String)
		{
			const FString S = JsonValue->AsString();
			if (S.StartsWith(TEXT("@")) && S.Len() > 1)
			{
				FProperty* KeyProp = StructProp->Struct->FindPropertyByName(TEXT("Key"));
				FNameProperty* KeyNameProp = CastField<FNameProperty>(KeyProp);
				if (!KeyNameProp)
				{
					OutErr = TEXT("FValueOrBBKey base struct has no FName 'Key' field — engine version mismatch?");
					return false;
				}
				FName* KeyValuePtr = KeyNameProp->ContainerPtrToValuePtr<FName>(StructPtr);
				*KeyValuePtr = FName(*S.Mid(1));
				return true;
			}
			// Plain string: treat as a literal DefaultValue (FString / FName cases).
		}

		// Otherwise: write the scalar into DefaultValue via ImportText.
		FProperty* DefaultProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
		if (!DefaultProp)
		{
			OutErr = TEXT("FValueOrBBKey struct has no 'DefaultValue' field — engine version mismatch?");
			return false;
		}

		FString TextValue;
		if (!JsonScalarToText(JsonValue, TextValue))
		{
			OutErr = TEXT("expected number/string/bool for FValueOrBBKey scalar");
			return false;
		}

		void* DefaultPtr = DefaultProp->ContainerPtrToValuePtr<void>(StructPtr);
		const TCHAR* ImportResult = DefaultProp->ImportText_Direct(*TextValue, DefaultPtr, /*Owner*/ nullptr, PPF_None);
		if (!ImportResult)
		{
			OutErr = FString::Printf(TEXT("ImportText_Direct rejected '%s' for %s"),
				*TextValue, *DefaultProp->GetClass()->GetName());
			return false;
		}
		return true;
	}

	/**
	 * Read an FValueOrBBKey_X for round-trip emission. Returns:
	 *   - the DefaultValue (as JSON) if it differs from CDO
	 *   - a "@KeyName" string if Key is bound
	 *   - null if the struct equals the CDO version
	 */
	static TSharedPtr<FJsonValue> TryReadValueOrBBKey(const FStructProperty* StructProp,
		const void* StructPtr, const void* CDOStructPtr)
	{
		if (!StructProp || !StructPtr || !CDOStructPtr) return nullptr;

		// Bound key wins — emit "@KeyName".
		FProperty* KeyProp = StructProp->Struct->FindPropertyByName(TEXT("Key"));
		FNameProperty* KeyNameProp = CastField<FNameProperty>(KeyProp);
		if (KeyNameProp)
		{
			const FName* KeyValuePtr = KeyNameProp->ContainerPtrToValuePtr<FName>(StructPtr);
			if (!KeyValuePtr->IsNone())
			{
				return MakeShared<FJsonValueString>(FString::Printf(TEXT("@%s"), *KeyValuePtr->ToString()));
			}
		}

		// Otherwise emit DefaultValue if it differs from CDO.
		FProperty* DefaultProp = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
		if (!DefaultProp) return nullptr;

		const void* DefaultPtr    = DefaultProp->ContainerPtrToValuePtr<void>(StructPtr);
		const void* CDODefaultPtr = DefaultProp->ContainerPtrToValuePtr<void>(CDOStructPtr);
		if (DefaultProp->Identical(DefaultPtr, CDODefaultPtr, PPF_DeepComparison))
		{
			return nullptr;
		}
		return FNGGHttpServer::PropertyToJson(DefaultProp, StructPtr);
	}

	/** Apply a JSON object of { property_name: value } pairs to a runtime UObject. */
	static void ApplyJsonProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props,
		TArray<FString>& OutWarnings)
	{
		if (!Target || !Props.IsValid()) return;

		UClass* Cls = Target->GetClass();

		for (const auto& Pair : Props->Values)
		{
			FProperty* Prop = Cls->FindPropertyByName(FName(*Pair.Key));
			if (FStructProperty* SP = CastField<FStructProperty>(Prop); SP && IsValueOrBBKeyStruct(SP))
			{
				void* StructPtr = SP->ContainerPtrToValuePtr<void>(Target);
				FString Err;
				if (!TrySetValueOrBBKey(SP, StructPtr, Pair.Value, Err))
				{
					OutWarnings.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *Err));
				}
				continue;
			}

			// UE5.8: Pair.Key is UE::FSharedString — deref to const TCHAR* for the FString& param.
			const FString Err = FNGGHttpServer::SetPropertyFromJson(Target, *Pair.Key, Pair.Value);
			if (!Err.IsEmpty())
			{
				OutWarnings.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *Err));
			}
		}
	}

	/** Get input or output pin (direction-filtered) from any UEdGraphNode. */
	static UEdGraphPin* GetDirPin(UEdGraphNode* Node, EEdGraphPinDirection Dir)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->Direction == Dir) return P;
		}
		return nullptr;
	}
}

// ============================================================================
// Handler: POST /bt/add_logic
// Body (full schema below; all fields except behavior_tree+nodes optional):
// {
//   "behavior_tree": "/Game/AI/BT_Foo",
//   "clear": true,                       // wipe non-Root nodes first (default false)
//   "nodes": [
//     {"id":"seq",   "type":"composite", "composite":"Sequence" },
//     {"id":"sel",   "type":"composite", "composite":"Selector" },
//     {"id":"wait",  "type":"task", "task_class":"BTTask_Wait",
//                    "properties":{"WaitTime":2.0}},
//     {"id":"move",  "type":"task", "task_class":"BTTask_MoveTo",
//                    "blackboard_key":"PatrolPoint",
//                    "properties":{"AcceptableRadius":50.0}},
//     {"id":"atk",   "type":"task", "task_class":"/Game/AI/BTT_Attack"}
//   ],
//   "decorators": [
//     {"parent":"sel", "decorator_class":"BTDecorator_Blackboard",
//      "blackboard_key":"IsAlerted"}
//   ],
//   "services": [
//     {"parent":"sel", "service_class":"BTService_DefaultFocus",
//      "blackboard_key":"TargetActor",
//      "properties":{"Interval":0.5}}
//   ],
//   "connections": [
//     {"from":"root","to":"seq"},
//     {"from":"seq","to":"sel"},
//     {"from":"sel","to":"move"},
//     {"from":"sel","to":"wait"}
//   ],
//   "compile": true,                     // run UBehaviorTreeGraph::UpdateAsset (default true)
//   "save": true                         // save package after compile (default true)
// }
//
// Notes:
//   - "root" is a reserved id referring to the BT's auto-created root node.
//   - Composite aliases: Selector / Sequence / SimpleParallel.
//   - Task/decorator/service classes accept short names (looked up in
//     /Script/AIModule), full /Script/Module.Class paths, or /Game/.../BP_X
//     paths for BP-derived nodes.
//   - blackboard_key shorthand sets the FBlackboardKeySelector::SelectedKeyName
//     on any *_BlackboardBase node and resolves against the BT's blackboard.
//   - Sibling execution order = order of appearance in connections[] (the first
//     connection from a parent becomes that parent's leftmost / first child).
//   - With clear=false, the call refuses if the BT already has non-Root nodes.
// ============================================================================
bool FNGGHttpServer::HandleBtAddLogic(
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

	FString BTPath;
	Body->TryGetStringField(TEXT("behavior_tree"), BTPath);
	if (BTPath.IsEmpty())
	{
		// Accept asset_path as an alias for consistency with other /bt/* tools.
		Body->TryGetStringField(TEXT("asset_path"), BTPath);
	}
	if (BTPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("behavior_tree (or asset_path) is required")));
		return true;
	}

	bool bClear   = false; Body->TryGetBoolField(TEXT("clear"),   bClear);
	bool bCompile = true;  Body->TryGetBoolField(TEXT("compile"), bCompile);
	bool bSave    = true;  Body->TryGetBoolField(TEXT("save"),    bSave);

	// Marshal node specs into POD-ish structs we can capture in the lambda.
	struct FNodeSpec
	{
		FString Id;
		FString Type;            // "composite" | "task"
		FString CompositeName;   // for composite
		FString TaskClass;       // for task
		FString BBKey;           // optional shorthand
		TSharedPtr<FJsonObject> Properties;
	};
	struct FSubNodeSpec
	{
		FString Parent;
		FString Class;           // decorator_class or service_class
		FString BBKey;
		TSharedPtr<FJsonObject> Properties;
	};
	struct FConnSpec { FString From; FString To; };

	TArray<FNodeSpec>    Nodes;
	TArray<FSubNodeSpec> Decorators;
	TArray<FSubNodeSpec> Services;
	TArray<FConnSpec>    Connections;

	auto ParseSubNodes = [&](const TCHAR* FieldName, const TCHAR* ClassFieldName,
		TArray<FSubNodeSpec>& Out) -> void
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Body->TryGetArrayField(FieldName, Arr) || !Arr) return;
		for (const auto& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) continue;
			FSubNodeSpec S;
			O->TryGetStringField(TEXT("parent"), S.Parent);
			O->TryGetStringField(ClassFieldName, S.Class);
			O->TryGetStringField(TEXT("blackboard_key"), S.BBKey);
			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			if (O->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj) S.Properties = *PropsObj;
			if (!S.Parent.IsEmpty() && !S.Class.IsEmpty()) Out.Add(MoveTemp(S));
		}
	};

	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("nodes"), Arr) && Arr)
		{
			for (const auto& V : *Arr)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				if (!O.IsValid()) continue;
				FNodeSpec S;
				O->TryGetStringField(TEXT("id"),             S.Id);
				O->TryGetStringField(TEXT("type"),           S.Type);
				O->TryGetStringField(TEXT("composite"),      S.CompositeName);
				O->TryGetStringField(TEXT("task_class"),     S.TaskClass);
				O->TryGetStringField(TEXT("blackboard_key"), S.BBKey);
				const TSharedPtr<FJsonObject>* PropsObj = nullptr;
				if (O->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj) S.Properties = *PropsObj;
				if (!S.Id.IsEmpty() && !S.Type.IsEmpty()) Nodes.Add(MoveTemp(S));
			}
		}
	}

	ParseSubNodes(TEXT("decorators"), TEXT("decorator_class"), Decorators);
	ParseSubNodes(TEXT("services"),   TEXT("service_class"),   Services);

	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("connections"), Arr) && Arr)
		{
			for (const auto& V : *Arr)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				if (!O.IsValid()) continue;
				FConnSpec C;
				O->TryGetStringField(TEXT("from"), C.From);
				O->TryGetStringField(TEXT("to"),   C.To);
				if (!C.From.IsEmpty() && !C.To.IsEmpty()) Connections.Add(MoveTemp(C));
			}
		}
	}

	if (Nodes.Num() == 0)
	{
		Callback(JsonError(400, TEXT("nodes[] must contain at least one entry")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BTPath, bClear, bCompile, bSave,
		 Nodes = MoveTemp(Nodes),
		 Decorators = MoveTemp(Decorators),
		 Services = MoveTemp(Services),
		 Connections = MoveTemp(Connections)]() mutable
	{
		FString FullObjectPath = BTPath;
		if (!FullObjectPath.Contains(TEXT(".")))
		{
			const FString BaseName = FPaths::GetBaseFilename(BTPath);
			FullObjectPath = BTPath + TEXT(".") + BaseName;
		}

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *FullObjectPath);
		if (!BT)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("UBehaviorTree not found at '%s'"), *BTPath)));
			return;
		}

		// Lazy-create the EdGraph + root if missing — UBehaviorTreeFactory leaves
		// BTGraph null until the asset is first opened in the BT editor, but we
		// want headless authoring to "just work" against any BT.
		UBehaviorTreeGraphNode_Root* RootNode = nullptr;
		UBehaviorTreeGraph* BTGraph = EnsureBTGraph(BT, &RootNode);
		if (!BTGraph || !RootNode)
		{
			Callback(JsonError(500,
				TEXT("Could not materialise UBehaviorTreeGraph + Root for this BT")));
			return;
		}

		// Optional clear: remove every non-Root node (and clear root's links).
		if (bClear)
		{
			// Collect nodes (graph + sub-nodes) so we can sweep ALL stale UObject
			// names out of the BTGraph package. RemoveNode alone leaves the deleted
			// nodes alive as orphan subobjects, so their names stay reserved — and
			// re-authoring with the same user IDs would collide on Rename. Move
			// every doomed node to the transient package to free up its name.
			TArray<UEdGraphNode*> AllDoomed;
			for (UEdGraphNode* N : BTGraph->Nodes)
			{
				if (!Cast<UBehaviorTreeGraphNode_Root>(N)) AllDoomed.Add(N);
			}

			// Sub-nodes (decorators / services) are reachable via SubNodes on each
			// primary node and need the same name release so their old IDs don't
			// linger.
			TArray<UAIGraphNode*> SubNodesToRelease;
			for (UEdGraphNode* N : BTGraph->Nodes)
			{
				if (auto* BTN = Cast<UBehaviorTreeGraphNode>(N))
				{
					for (UAIGraphNode* Sub : BTN->SubNodes)
					{
						if (Sub) SubNodesToRelease.Add(Sub);
					}
				}
			}

			for (UEdGraphNode* N : AllDoomed)
			{
				N->BreakAllNodeLinks();
				BTGraph->RemoveNode(N);
			}

			UPackage* Transient = GetTransientPackage();
			auto MoveToTransient = [Transient](UObject* Obj)
			{
				if (!Obj) return;
				const FName Trash = MakeUniqueObjectName(Transient, Obj->GetClass(), TEXT("TRASH_BT"));
				Obj->Rename(*Trash.ToString(), Transient, REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
			};
			for (UEdGraphNode* N : AllDoomed)        MoveToTransient(N);
			for (UAIGraphNode* N : SubNodesToRelease) MoveToTransient(N);

			RootNode->BreakAllNodeLinks();
		}
		else
		{
			// Refuse if the tree already has authored nodes — avoids ambiguous
			// merge semantics in the first cut.
			int32 NonRootCount = 0;
			for (UEdGraphNode* N : BTGraph->Nodes)
			{
				if (!Cast<UBehaviorTreeGraphNode_Root>(N)) ++NonRootCount;
			}
			if (NonRootCount > 0)
			{
				Callback(JsonError(409, FString::Printf(
					TEXT("BT '%s' already has %d non-Root nodes — pass clear=true to overwrite"),
					*BTPath, NonRootCount)));
				return;
			}
		}

		UBlackboardData* BB = BT->BlackboardAsset;
		// BB may be null for trees authored before a blackboard is linked. We
		// don't refuse here — blackboard_key shorthand will silently no-op resolve
		// (the names get stored, ResolveSelectedKey runs on load).

		TMap<FString, UBehaviorTreeGraphNode*> NodeMap;
		NodeMap.Add(TEXT("root"), RootNode);

		TArray<TSharedPtr<FJsonValue>> CreatedJson;
		TArray<FString>                Warnings;

		// ---- 1. Create primary nodes (composites + tasks) ------------------
		for (const FNodeSpec& S : Nodes)
		{
			if (NodeMap.Contains(S.Id))
			{
				Warnings.Add(FString::Printf(TEXT("duplicate node id '%s' — skipped"), *S.Id));
				continue;
			}

			UClass* RuntimeClass = nullptr;
			UBehaviorTreeGraphNode* EdNode = nullptr;
			const FString TypeLower = S.Type.ToLower();

			if (TypeLower == TEXT("composite"))
			{
				RuntimeClass = ResolveCompositeClass(S.CompositeName);
				if (!RuntimeClass)
				{
					Warnings.Add(FString::Printf(
						TEXT("node '%s': unknown composite '%s' (Selector|Sequence|SimpleParallel)"),
						*S.Id, *S.CompositeName));
					continue;
				}
				EdNode = NewObject<UBehaviorTreeGraphNode_Composite>(BTGraph);
			}
			else if (TypeLower == TEXT("task"))
			{
				RuntimeClass = ResolveBTNodeClass(S.TaskClass, UBTTaskNode::StaticClass());
				if (!RuntimeClass)
				{
					Warnings.Add(FString::Printf(
						TEXT("node '%s': could not resolve task_class '%s' to a UBTTaskNode subclass"),
						*S.Id, *S.TaskClass));
					continue;
				}
				EdNode = NewObject<UBehaviorTreeGraphNode_Task>(BTGraph);
			}
			else
			{
				Warnings.Add(FString::Printf(
					TEXT("node '%s': unknown type '%s' (composite|task)"), *S.Id, *S.Type));
				continue;
			}

			if (!EdNode)
			{
				Warnings.Add(FString::Printf(
					TEXT("node '%s': failed to allocate editor graph node"), *S.Id));
				continue;
			}
			UObject* RuntimeInst = NewObject<UObject>(BT, RuntimeClass);
			if (!RuntimeInst)
			{
				Warnings.Add(FString::Printf(
					TEXT("node '%s': failed to allocate runtime instance of '%s'"),
					*S.Id, *RuntimeClass->GetName()));
				continue;
			}
			EdNode->NodeInstance = RuntimeInst;
			EdNode->ClassData    = FGraphNodeClassData(RuntimeClass, FString());

			BTGraph->AddNode(EdNode, /*bUserAction*/ false, /*bSelectNewNode*/ false);
			EdNode->CreateNewGuid();
			EdNode->PostPlacedNewNode();
			EdNode->AllocateDefaultPins();

			// Persist the caller's id as the EdGraph node's UObject name so a
			// later read_tree can return the same id back. UObject::Rename ASSERTS
			// (not just returns false) when the target name is already taken in
			// the outer, so check first and fall back to MakeUniqueObjectName when
			// there's a stale collision.
			FName DesiredName(*S.Id);
			if (StaticFindObjectFast(nullptr, BTGraph, DesiredName))
			{
				DesiredName = MakeUniqueObjectName(BTGraph, EdNode->GetClass(), DesiredName);
				Warnings.Add(FString::Printf(
					TEXT("node '%s': name collision — assigned unique name '%s'"),
					*S.Id, *DesiredName.ToString()));
			}
			EdNode->Rename(*DesiredName.ToString(), BTGraph, REN_DontCreateRedirectors | REN_DoNotDirty);

			// Apply user-supplied properties to runtime instance.
			ApplyJsonProperties(RuntimeInst, S.Properties, Warnings);

			// BlackboardKey shorthand (no-op for nodes that don't have one).
			if (!S.BBKey.IsEmpty())
			{
				const bool bApplied = ApplyBlackboardKeyShorthand(RuntimeInst, S.BBKey, BB);
				if (bApplied && BB && !BlackboardHasKey(BB, FName(*S.BBKey)))
				{
					Warnings.Add(FString::Printf(
						TEXT("node '%s': blackboard_key '%s' does not exist on BB '%s' (or its parent chain)"),
						*S.Id, *S.BBKey, *BB->GetPathName()));
				}
			}

			NodeMap.Add(S.Id, EdNode);

			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("id"),    S.Id);
			J->SetStringField(TEXT("type"),  S.Type);
			J->SetStringField(TEXT("class"), RuntimeClass->GetPathName());
			CreatedJson.Add(MakeShared<FJsonValueObject>(J));
		}

		// ---- 2. Attach decorators (sub-nodes) ------------------------------
		auto AttachSubNode = [&](const FSubNodeSpec& S, UClass* Base, bool bIsService)
		{
			UBehaviorTreeGraphNode** ParentPtr = NodeMap.Find(S.Parent);
			if (!ParentPtr || !*ParentPtr)
			{
				Warnings.Add(FString::Printf(
					TEXT("%s parent='%s' not found in nodes[] (or 'root')"),
					bIsService ? TEXT("service") : TEXT("decorator"), *S.Parent));
				return;
			}
			UBehaviorTreeGraphNode* Parent = *ParentPtr;

			UClass* RuntimeClass = ResolveBTNodeClass(S.Class, Base);
			if (!RuntimeClass)
			{
				Warnings.Add(FString::Printf(
					TEXT("%s for parent='%s': could not resolve '%s' to a %s subclass"),
					bIsService ? TEXT("service") : TEXT("decorator"),
					*S.Parent, *S.Class, *Base->GetName()));
				return;
			}

			UBehaviorTreeGraphNode* SubEdNode = bIsService
				? Cast<UBehaviorTreeGraphNode>(NewObject<UBehaviorTreeGraphNode_Service>(BTGraph))
				: Cast<UBehaviorTreeGraphNode>(NewObject<UBehaviorTreeGraphNode_Decorator>(BTGraph));
			if (!SubEdNode)
			{
				Warnings.Add(FString::Printf(
					TEXT("%s for parent='%s': failed to allocate editor graph node"),
					bIsService ? TEXT("service") : TEXT("decorator"), *S.Parent));
				return;
			}

			UObject* RuntimeInst = NewObject<UObject>(BT, RuntimeClass);
			if (!RuntimeInst)
			{
				Warnings.Add(FString::Printf(
					TEXT("%s for parent='%s': failed to allocate runtime instance of '%s'"),
					bIsService ? TEXT("service") : TEXT("decorator"), *S.Parent, *RuntimeClass->GetName()));
				return;
			}
			SubEdNode->NodeInstance = RuntimeInst;
			SubEdNode->ClassData    = FGraphNodeClassData(RuntimeClass, FString());
			SubEdNode->ParentNode   = Parent;
			SubEdNode->CreateNewGuid();
			SubEdNode->PostPlacedNewNode();
			// Sub-nodes don't allocate pins (no edge connections from Decorators/Services to siblings).

			if (bIsService) Parent->Services.Add(SubEdNode);
			else            Parent->Decorators.Add(SubEdNode);
			Parent->SubNodes.Add(SubEdNode);

			ApplyJsonProperties(RuntimeInst, S.Properties, Warnings);
			if (!S.BBKey.IsEmpty())
			{
				const bool bApplied = ApplyBlackboardKeyShorthand(RuntimeInst, S.BBKey, BB);
				if (bApplied && BB && !BlackboardHasKey(BB, FName(*S.BBKey)))
				{
					Warnings.Add(FString::Printf(
						TEXT("%s on parent='%s': blackboard_key '%s' does not exist on BB '%s' (or its parent chain)"),
						bIsService ? TEXT("service") : TEXT("decorator"),
						*S.Parent, *S.BBKey, *BB->GetPathName()));
				}
			}
		};

		for (const FSubNodeSpec& S : Decorators) AttachSubNode(S, UBTDecorator::StaticClass(), false);
		for (const FSubNodeSpec& S : Services)   AttachSubNode(S, UBTService::StaticClass(),   true);

		// ---- 3. Wire connections (parent→child) ----------------------------
		// Track sibling index per parent so first-connected child = leftmost.
		TMap<FString, int32> SiblingIdx;
		const UEdGraphSchema* Schema = BTGraph->GetSchema();

		for (const FConnSpec& C : Connections)
		{
			UBehaviorTreeGraphNode** FromPtr = NodeMap.Find(C.From);
			UBehaviorTreeGraphNode** ToPtr   = NodeMap.Find(C.To);
			if (!FromPtr || !*FromPtr) { Warnings.Add(FString::Printf(TEXT("connection from='%s' not found"), *C.From)); continue; }
			if (!ToPtr   || !*ToPtr  ) { Warnings.Add(FString::Printf(TEXT("connection to='%s' not found"),   *C.To));   continue; }

			UEdGraphPin* OutPin = GetDirPin(*FromPtr, EGPD_Output);
			UEdGraphPin* InPin  = GetDirPin(*ToPtr,   EGPD_Input);
			if (!OutPin) { Warnings.Add(FString::Printf(TEXT("'%s' has no output pin (task/leaf?)"), *C.From)); continue; }
			if (!InPin ) { Warnings.Add(FString::Printf(TEXT("'%s' has no input pin"),               *C.To  )); continue; }

			bool bConnected = false;
			if (Schema)
			{
				bConnected = Schema->TryCreateConnection(OutPin, InPin);
			}
			if (!bConnected)
			{
				// Fallback to raw link — schema validation may reject e.g. multi-input pins
				// even when the underlying tree is valid. MakeLinkTo is symmetric: calling
				// it once links both sides; a second call on the other pin would duplicate
				// one entry in LinkedTo[].
				OutPin->MakeLinkTo(InPin);
			}

			// Position the child so siblings cascade left-to-right under the parent.
			const int32 Idx = SiblingIdx.FindOrAdd(C.From);
			SiblingIdx[C.From] = Idx + 1;
			(*ToPtr)->NodePosX = (*FromPtr)->NodePosX + (Idx * 320 - 160);
			(*ToPtr)->NodePosY = (*FromPtr)->NodePosY + 200;
		}

		// ---- 4. Compile ----------------------------------------------------
		if (bCompile)
		{
			// Mirror EdGraph → runtime tree. UBehaviorTreeGraph::ClearDebuggerFlags
			// is a sentinel value the editor passes; 0 is safe for headless calls.
			BTGraph->RebuildExecutionOrder();
			BTGraph->UpdateAsset(0);
		}

		BT->PostEditChange();
		BT->MarkPackageDirty();

		bool bSaved = false;
		if (bSave) bSaved = BtSavePackageForAsset(BT);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bt/add_logic: %s nodes=%d decorators=%d services=%d connections=%d warnings=%d saved=%d"),
			*BTPath, CreatedJson.Num(), Decorators.Num(), Services.Num(),
			Connections.Num(), Warnings.Num(), bSaved ? 1 : 0);

		// A requested-but-failed save means the asset wasn't persisted — don't
		// report overall success, or the caller will assume the write landed.
		const bool bSaveOk = !bSave || bSaved;

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),       bSaveOk);
		Out->SetStringField(TEXT("behavior_tree"), BTPath);
		Out->SetArrayField (TEXT("created"),       CreatedJson);
		Out->SetNumberField(TEXT("decorators"),    Decorators.Num());
		Out->SetNumberField(TEXT("services"),      Services.Num());
		Out->SetNumberField(TEXT("connections"),   Connections.Num());

		TArray<TSharedPtr<FJsonValue>> WJson;
		for (const FString& W : Warnings) WJson.Add(MakeShared<FJsonValueString>(W));
		Out->SetArrayField (TEXT("warnings"),      WJson);
		Out->SetBoolField  (TEXT("compiled"),      bCompile);
		Out->SetBoolField  (TEXT("saved"),         bSaved);
		Callback(JsonOk(BtSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: GET /bt/read_tree?behavior_tree=/Game/AI/BT_Foo
// Returns the BT graph as JSON in a shape compatible with /bt/add_logic so the
// caller can read → edit → write a tree round-trip. Emits the EdGraph (the
// authoring representation), not the runtime tree.
// ============================================================================
bool FNGGHttpServer::HandleBtReadTree(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	const FString BTPath = GetQueryParam(Req, TEXT("behavior_tree"));
	if (BTPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("?behavior_tree=/Game/... is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BTPath]()
	{
		FString FullObjectPath = BTPath;
		if (!FullObjectPath.Contains(TEXT(".")))
		{
			const FString BaseName = FPaths::GetBaseFilename(BTPath);
			FullObjectPath = BTPath + TEXT(".") + BaseName;
		}

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *FullObjectPath);
		if (!BT)
		{
			Callback(JsonError(404, FString::Printf(TEXT("UBehaviorTree not found at '%s'"), *BTPath)));
			return;
		}
		UBehaviorTreeGraph* BTGraph = EnsureBTGraph(BT);
		if (!BTGraph)
		{
			Callback(JsonError(500, TEXT("BT has no UBehaviorTreeGraph and could not create one")));
			return;
		}

		// Assign stable string IDs so connections[] can reference nodes.
		// Use the EdGraph node's name (auto-generated) as the id.
		TMap<UBehaviorTreeGraphNode*, FString> Ids;
		auto IdFor = [&Ids](UBehaviorTreeGraphNode* N) -> FString
		{
			if (!N) return FString();
			if (FString* Cached = Ids.Find(N)) return *Cached;
			FString Id;
			if (Cast<UBehaviorTreeGraphNode_Root>(N)) Id = TEXT("root");
			else Id = N->GetName();
			Ids.Add(N, Id);
			return Id;
		};

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TArray<TSharedPtr<FJsonValue>> DecsJson;
		TArray<TSharedPtr<FJsonValue>> SvcsJson;
		TArray<TSharedPtr<FJsonValue>> ConnsJson;

		for (UEdGraphNode* RawN : BTGraph->Nodes)
		{
			UBehaviorTreeGraphNode* N = Cast<UBehaviorTreeGraphNode>(RawN);
			if (!N) continue;

			const FString Id = IdFor(N);

			// Skip emitting the root as a node (it's special — referenced as "root" only).
			if (!Cast<UBehaviorTreeGraphNode_Root>(N))
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("id"), Id);

				if (Cast<UBehaviorTreeGraphNode_Task>(N))
				{
					J->SetStringField(TEXT("type"), TEXT("task"));
					if (N->NodeInstance) J->SetStringField(TEXT("task_class"), N->NodeInstance->GetClass()->GetPathName());
				}
				else if (Cast<UBehaviorTreeGraphNode_Composite>(N))
				{
					J->SetStringField(TEXT("type"), TEXT("composite"));
					if (N->NodeInstance) J->SetStringField(TEXT("composite_class"), N->NodeInstance->GetClass()->GetPathName());
				}
				else
				{
					J->SetStringField(TEXT("type"), N->GetClass()->GetName());
				}
				J->SetNumberField(TEXT("pos_x"), N->NodePosX);
				J->SetNumberField(TEXT("pos_y"), N->NodePosY);

				// Round-trip extras: blackboard_key shorthand + non-default UPROPERTYs.
				if (N->NodeInstance)
				{
					const FName BBK = ReadBlackboardKeyName(N->NodeInstance);
					if (!BBK.IsNone())
					{
						J->SetStringField(TEXT("blackboard_key"), BBK.ToString());
					}
					TSharedPtr<FJsonObject> Props = SerializeChangedProperties(N->NodeInstance);
					if (Props.IsValid() && Props->Values.Num() > 0)
					{
						J->SetObjectField(TEXT("properties"), Props);
					}
				}
				NodesJson.Add(MakeShared<FJsonValueObject>(J));
			}

			// Sub-nodes (decorators / services). Same round-trip extras apply.
			auto EmitSubNode = [&](UBehaviorTreeGraphNode* Sub, const TCHAR* ClassFieldName,
				TArray<TSharedPtr<FJsonValue>>& OutArr)
			{
				if (!Sub) return;
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("parent"), Id);
				if (Sub->NodeInstance)
				{
					J->SetStringField(ClassFieldName, Sub->NodeInstance->GetClass()->GetPathName());
					const FName BBK = ReadBlackboardKeyName(Sub->NodeInstance);
					if (!BBK.IsNone())
					{
						J->SetStringField(TEXT("blackboard_key"), BBK.ToString());
					}
					TSharedPtr<FJsonObject> Props = SerializeChangedProperties(Sub->NodeInstance);
					if (Props.IsValid() && Props->Values.Num() > 0)
					{
						J->SetObjectField(TEXT("properties"), Props);
					}
				}
				OutArr.Add(MakeShared<FJsonValueObject>(J));
			};
			for (UBehaviorTreeGraphNode* D : N->Decorators) EmitSubNode(D, TEXT("decorator_class"), DecsJson);
			for (UBehaviorTreeGraphNode* S : N->Services)   EmitSubNode(S, TEXT("service_class"),   SvcsJson);

			// Outgoing connections from this node.
			if (UEdGraphPin* OutPin = GetDirPin(N, EGPD_Output))
			{
				// Children sorted by NodePosX = execution order.
				TArray<UBehaviorTreeGraphNode*> Children;
				for (UEdGraphPin* Linked : OutPin->LinkedTo)
				{
					if (auto* C = Cast<UBehaviorTreeGraphNode>(Linked->GetOwningNode())) Children.Add(C);
				}
				Children.Sort([](const UBehaviorTreeGraphNode& A, const UBehaviorTreeGraphNode& B)
					{ return A.NodePosX < B.NodePosX; });
				for (UBehaviorTreeGraphNode* C : Children)
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("from"), Id);
					J->SetStringField(TEXT("to"),   IdFor(C));
					ConnsJson.Add(MakeShared<FJsonValueObject>(J));
				}
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("behavior_tree"),   BTPath);
		Out->SetStringField(TEXT("blackboard_path"), BT->BlackboardAsset ? BT->BlackboardAsset->GetPathName() : FString());
		Out->SetArrayField (TEXT("nodes"),           NodesJson);
		Out->SetArrayField (TEXT("decorators"),      DecsJson);
		Out->SetArrayField (TEXT("services"),        SvcsJson);
		Out->SetArrayField (TEXT("connections"),     ConnsJson);
		Callback(JsonOk(BtSerializeJson(Out)));
	});

	return true;
}
