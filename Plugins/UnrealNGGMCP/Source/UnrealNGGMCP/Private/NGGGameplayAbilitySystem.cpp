// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGGameplayAbilitySystem.cpp
//
// Implementation of the /gas/* HTTP endpoints that expose Unreal Engine 5
// Gameplay Ability System (GAS) authoring to MCP clients.
//
//   POST /gas/setup_actor          — ensure a Blueprint has a UAbilitySystemComponent
//   POST /gas/create_attribute_set — create a UAttributeSet subclass Blueprint
//   POST /gas/create_ability       — create a UGameplayAbility subclass Blueprint
//   POST /gas/create_effect        — create a UGameplayEffect subclass Blueprint
//   POST /gas/configure_asc        — set replication mode on an existing ASC
//   GET  /gas/read_setup           — read the GAS-relevant config off a Blueprint
//
// Threading contract:
//   - All UObject mutation runs on the Game Thread via AsyncTask.
//   - Handlers themselves return true synchronously after dispatching the
//     async work; the OnComplete callback fires from inside the GT lambda.
//
// Helper-call discipline:
//   - Helpers in the NGGGasPriv namespace are referenced from handlers via
//     fully-qualified calls (NGGGasPriv::Foo) to avoid C2668 ambiguity that
//     can appear under adaptive non-unity builds when other translation units
//     declare same-named helpers in their own *Priv namespace.

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
#include "UObject/UObjectIterator.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

// ---- Asset Tools / Asset Registry -----------------------------------------
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ObjectTools.h"  // ObjectTools::ForceDeleteObjects — used to roll back a partially-created asset
#include "Editor.h"       // GEditor — clear selection before ForceDeleteObjects
#include "Selection.h"    // USelection — DeselectAll prior to ForceDeleteObjects

// ---- Blueprint editing ----------------------------------------------------
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"  // UEdGraphSchema_K2::PC_Struct
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Factories/BlueprintFactory.h"

// ---- Gameplay Ability System ---------------------------------------------
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace NGGGasPriv
{
	static FString GasSerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** Split "/Game/GAS/AS_Foo" into PackagePath="/Game/GAS" + AssetName="AS_Foo". */
	static bool GasSplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
	{
		int32 LastSlash = INDEX_NONE;
		if (!AssetPath.FindLastChar(TEXT('/'), LastSlash)) return false;
		OutPackagePath = AssetPath.Left(LastSlash);
		OutAssetName   = AssetPath.Mid(LastSlash + 1);
		return !OutAssetName.IsEmpty();
	}

	/** Save the package containing Asset to disk. Returns true on success. */
	static bool GasSavePackageForAsset(UObject* Asset)
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

	/** Parse an array of JSON string values into an FGameplayTagContainer. */
	static FGameplayTagContainer ParseTagContainer(const TArray<TSharedPtr<FJsonValue>>* TagsArr)
	{
		FGameplayTagContainer Container;
		if (!TagsArr) return Container;
		UGameplayTagsManager& TM = UGameplayTagsManager::Get();
		for (const TSharedPtr<FJsonValue>& V : *TagsArr)
		{
			FString TagStr;
			if (V.IsValid() && V->TryGetString(TagStr) && !TagStr.IsEmpty())
			{
				FGameplayTag Tag = TM.RequestGameplayTag(FName(*TagStr), /*bErrorIfNotFound*/ false);
				if (Tag.IsValid()) Container.AddTag(Tag);
			}
		}
		return Container;
	}

	/** Convert a FGameplayTagContainer to a JSON array of tag strings. */
	static TArray<TSharedPtr<FJsonValue>> TagContainerToJson(const FGameplayTagContainer& C)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FGameplayTag& T : C)
		{
			Out.Add(MakeShared<FJsonValueString>(T.ToString()));
		}
		return Out;
	}

	/**
	 * Resolve "AS_Character.Health" or "/Game/GAS/AS_Character.Health" to an
	 * FGameplayAttribute. Returns an invalid attribute if resolution fails so
	 * the caller can log a warning without aborting the whole effect.
	 */
	static FGameplayAttribute ResolveAttribute(const FString& AttrStr)
	{
		FString ClassName, PropName;
		if (!AttrStr.Split(TEXT("."), &ClassName, &PropName)) return FGameplayAttribute();
		ClassName = ClassName.TrimStartAndEnd();
		PropName  = PropName.TrimStartAndEnd();
		if (ClassName.IsEmpty() || PropName.IsEmpty()) return FGameplayAttribute();

		UClass* AttrSetClass = nullptr;

		// Full asset path → load the class directly. Try with and without the _C suffix
		// because Blueprint-generated attribute set classes are <Path>.<Name>_C.
		if (ClassName.StartsWith(TEXT("/")))
		{
			AttrSetClass = LoadObject<UClass>(nullptr, *ClassName);
			if (!AttrSetClass)
			{
				const FString WithC = ClassName.EndsWith(TEXT("_C")) ? ClassName : (ClassName + TEXT("_C"));
				AttrSetClass = LoadObject<UClass>(nullptr, *WithC);
			}
		}

		// Short name fallback — scan all loaded UAttributeSet subclasses.
		if (!AttrSetClass)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UAttributeSet::StaticClass()) &&
					(It->GetName() == ClassName || It->GetName() == (ClassName + TEXT("_C"))))
				{
					AttrSetClass = *It;
					break;
				}
			}
		}
		if (!AttrSetClass) return FGameplayAttribute();

		FProperty* Prop = AttrSetClass->FindPropertyByName(FName(*PropName));
		if (!Prop) return FGameplayAttribute();
		return FGameplayAttribute(Prop);
	}

	/** Map "add" / "multiply" / "override" → EGameplayModOp. */
	static TEnumAsByte<EGameplayModOp::Type> ParseModOp(const FString& Op)
	{
		const FString L = Op.ToLower();
		// UE5.7 renamed Multiplicative → MultiplyAdditive; use the numeric value for compatibility.
		if (L == TEXT("multiply") || L == TEXT("multiplicative")) return static_cast<EGameplayModOp::Type>(1); // MultiplyAdditive
		if (L == TEXT("override"))                                 return EGameplayModOp::Override;
		return EGameplayModOp::Additive; // default "add"
	}

	/** Map duration policy string → EGameplayEffectDurationType. */
	static EGameplayEffectDurationType ParseDurationPolicy(const FString& S)
	{
		const FString L = S.ToLower();
		if (L == TEXT("infinite"))    return EGameplayEffectDurationType::Infinite;
		if (L == TEXT("hasduration") || L == TEXT("has_duration") || L == TEXT("duration"))
		                              return EGameplayEffectDurationType::HasDuration;
		return EGameplayEffectDurationType::Instant;
	}

	/** Reverse of ParseDurationPolicy — for /gas/read_setup output. */
	static FString DurationPolicyToString(EGameplayEffectDurationType T)
	{
		switch (T)
		{
			case EGameplayEffectDurationType::Instant:     return TEXT("Instant");
			case EGameplayEffectDurationType::HasDuration: return TEXT("HasDuration");
			case EGameplayEffectDurationType::Infinite:    return TEXT("Infinite");
			default:                                        return TEXT("Unknown");
		}
	}

	/** Map replication mode string → EGameplayEffectReplicationMode. */
	static EGameplayEffectReplicationMode ParseReplicationMode(const FString& S)
	{
		const FString L = S.ToLower();
		if (L == TEXT("full"))    return EGameplayEffectReplicationMode::Full;
		if (L == TEXT("minimal")) return EGameplayEffectReplicationMode::Minimal;
		return EGameplayEffectReplicationMode::Mixed;
	}

	static FString ReplicationModeToString(EGameplayEffectReplicationMode M)
	{
		switch (M)
		{
			case EGameplayEffectReplicationMode::Full:    return TEXT("Full");
			case EGameplayEffectReplicationMode::Minimal: return TEXT("Minimal");
			case EGameplayEffectReplicationMode::Mixed:   return TEXT("Mixed");
			default:                                       return TEXT("Mixed");
		}
	}

	/** Map net execution policy string → EGameplayAbilityNetExecutionPolicy. */
	static EGameplayAbilityNetExecutionPolicy::Type ParseNetPolicy(const FString& S)
	{
		const FString L = S.ToLower();
		if (L == TEXT("localonly")       || L == TEXT("local_only"))         return EGameplayAbilityNetExecutionPolicy::LocalOnly;
		if (L == TEXT("serverinitiated") || L == TEXT("server_initiated"))   return EGameplayAbilityNetExecutionPolicy::ServerInitiated;
		if (L == TEXT("serveronly")      || L == TEXT("server_only"))        return EGameplayAbilityNetExecutionPolicy::ServerOnly;
		return EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	}

	static FString NetPolicyToString(EGameplayAbilityNetExecutionPolicy::Type P)
	{
		switch (P)
		{
			case EGameplayAbilityNetExecutionPolicy::LocalOnly:       return TEXT("LocalOnly");
			case EGameplayAbilityNetExecutionPolicy::LocalPredicted:  return TEXT("LocalPredicted");
			case EGameplayAbilityNetExecutionPolicy::ServerInitiated: return TEXT("ServerInitiated");
			case EGameplayAbilityNetExecutionPolicy::ServerOnly:      return TEXT("ServerOnly");
			default:                                                   return TEXT("LocalPredicted");
		}
	}

	/** Map instancing policy string → EGameplayAbilityInstancingPolicy. */
	PRAGMA_DISABLE_DEPRECATION_WARNINGS  // NonInstanced is deprecated in UE5.7 — still valid to set
	static EGameplayAbilityInstancingPolicy::Type ParseInstancingPolicy(const FString& S)
	{
		const FString L = S.ToLower();
		if (L == TEXT("noninstanced")          || L == TEXT("non_instanced"))     return EGameplayAbilityInstancingPolicy::NonInstanced;
		if (L == TEXT("instancedperexecution") || L == TEXT("per_execution"))     return EGameplayAbilityInstancingPolicy::InstancedPerExecution;
		return EGameplayAbilityInstancingPolicy::InstancedPerActor;
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	static FString InstancingPolicyToString(EGameplayAbilityInstancingPolicy::Type P)
	{
		switch (P)
		{
			case EGameplayAbilityInstancingPolicy::NonInstanced:          return TEXT("NonInstanced");
			case EGameplayAbilityInstancingPolicy::InstancedPerActor:     return TEXT("InstancedPerActor");
			case EGameplayAbilityInstancingPolicy::InstancedPerExecution: return TEXT("InstancedPerExecution");
			default:                                                       return TEXT("InstancedPerActor");
		}
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	/**
	 * Map stacking type string → EGameplayEffectStackingType.
	 * bOutValid is set false when S is non-empty but doesn't match a known type,
	 * so the caller can reject the request instead of silently writing None.
	 */
	static EGameplayEffectStackingType ParseStackingType(const FString& S, bool& bOutValid)
	{
		bOutValid = true;
		const FString L = S.ToLower();
		if (L.Contains(TEXT("source"))) return EGameplayEffectStackingType::AggregateBySource;
		if (L.Contains(TEXT("target"))) return EGameplayEffectStackingType::AggregateByTarget;
		if (L == TEXT("none"))          return EGameplayEffectStackingType::None;
		bOutValid = false;
		return EGameplayEffectStackingType::None;
	}

	static FString StackingTypeToString(EGameplayEffectStackingType T)
	{
		switch (T)
		{
			case EGameplayEffectStackingType::AggregateBySource: return TEXT("AggregateBySource");
			case EGameplayEffectStackingType::AggregateByTarget: return TEXT("AggregateByTarget");
			case EGameplayEffectStackingType::None:              return TEXT("None");
			default:                                              return TEXT("None");
		}
	}

	/** Load a UBlueprint at a /Game/ content path. Returns nullptr if not found. */
	static UBlueprint* LoadBlueprint(const FString& AssetPath)
	{
		if (AssetPath.IsEmpty()) return nullptr;
		FString FullPath = AssetPath;
		if (!FullPath.Contains(TEXT(".")))
		{
			const FString Name = FPaths::GetBaseFilename(AssetPath);
			FullPath = AssetPath + TEXT(".") + Name;
		}
		return LoadObject<UBlueprint>(nullptr, *FullPath);
	}

	/**
	 * Resolve a UClass from a JSON-supplied class ref. Accepts:
	 *   - empty string         → returns FallbackClass
	 *   - full /Script/ path   → LoadObject<UClass> (also tries _C suffix)
	 *   - full /Game/ path     → LoadObject<UClass> (also tries _C suffix)
	 *   - short name           → matches FallbackClass->GetName() AND returns FallbackClass
	 *                            otherwise iterates UClass and matches by name
	 */
	static UClass* ResolveClassRefOrFallback(const FString& Ref, UClass* FallbackClass)
	{
		if (Ref.IsEmpty()) return FallbackClass;

		if (Ref.StartsWith(TEXT("/")))
		{
			if (UClass* C = LoadObject<UClass>(nullptr, *Ref)) return C;
			const FString WithSuffix = Ref.EndsWith(TEXT("_C")) ? Ref : (Ref + TEXT("_C"));
			if (UClass* C = LoadObject<UClass>(nullptr, *WithSuffix)) return C;
			return nullptr;
		}

		// Short name. Try a fast equality with the fallback first.
		if (FallbackClass && (FallbackClass->GetName() == Ref ||
			(TEXT("U") + FallbackClass->GetName()) == Ref))
		{
			return FallbackClass;
		}

		// Iterate loaded classes for a name match.
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == Ref || (TEXT("U") + It->GetName()) == Ref)
			{
				return *It;
			}
		}
		return nullptr;
	}

	/**
	 * Create a Blueprint subclass of ParentClass at AssetPath. If the asset
	 * already exists, returns the existing UBlueprint (idempotent). On failure,
	 * fills OutError and returns nullptr.
	 */
	static UBlueprint* CreateBlueprintAsset(const FString& AssetPath, UClass* ParentClass,
		FString& OutError, bool& bOutAlreadyExisted)
	{
		bOutAlreadyExisted = false;
		if (!ParentClass)
		{
			OutError = TEXT("ParentClass is null");
			return nullptr;
		}

		FString PackagePath, AssetName;
		if (!GasSplitAssetPath(AssetPath, PackagePath, AssetName))
		{
			OutError = TEXT("asset_path must be a content path with at least one '/'");
			return nullptr;
		}

		const FString FullPath = AssetPath + TEXT(".") + AssetName;
		if (UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *FullPath))
		{
			bOutAlreadyExisted = true;
			return Existing;
		}

		FAssetToolsModule& ATM = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;
		Factory->bEditAfterNew = false;

		UObject* NewAsset = ATM.Get().CreateAsset(AssetName, PackagePath, UBlueprint::StaticClass(), Factory);
		UBlueprint* BP = Cast<UBlueprint>(NewAsset);
		if (!BP)
		{
			OutError = FString::Printf(TEXT("CreateAsset failed for '%s'"), *AssetPath);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(BP);
		return BP;
	}

	/**
	 * Find an existing AbilitySystemComponent SCS node template on a Blueprint.
	 * If bAdd is true and none is found, add a new one and return its template.
	 *
	 * NOTE on the returned pointer: the SCS node template is the per-class
	 * archetype (CDO-equivalent for components). Property writes here will
	 * carry into all instances spawned from this Blueprint — exactly the same
	 * semantics as editing defaults in the Blueprint editor's Components panel.
	 */
	static UAbilitySystemComponent* FindOrAddAscToBlueprint(UBlueprint* BP, bool bAdd, bool& bOutAdded)
	{
		bOutAdded = false;
		if (!BP || !BP->SimpleConstructionScript) return nullptr;

		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentTemplate && Node->ComponentTemplate->IsA<UAbilitySystemComponent>())
			{
				return Cast<UAbilitySystemComponent>(Node->ComponentTemplate);
			}
		}

		if (!bAdd) return nullptr;

		// UE5 SCS API: CreateNode (not CreateNodeWithNewName which was removed).
		USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(
			UAbilitySystemComponent::StaticClass(), TEXT("AbilitySystemComponent"));
		if (!NewNode || !NewNode->ComponentTemplate) return nullptr;

		BP->SimpleConstructionScript->AddNode(NewNode);
		bOutAdded = true;
		return Cast<UAbilitySystemComponent>(NewNode->ComponentTemplate);
	}

	/**
	 * Roll back a Blueprint asset that THIS call just created but failed to save,
	 * so a failed save never leaves a phantom registered in memory / the asset
	 * registry. Mirrors the delete path in NGGHttpServer.cpp::HandleAssetsDelete:
	 * clear editor selection first (ForceDeleteObjects walks the selection and
	 * asserts on dangling TypedElementHandles), then ObjectTools::ForceDeleteObjects.
	 *
	 * Returns true if the asset was destroyed. On failure OutError describes why,
	 * so the caller can still return its 500 with a clear message rather than
	 * leaving the caller unaware that a phantom may persist.
	 *
	 * Callers MUST guard on the newly-created flag (NOT bAlreadyExisted) — never
	 * pass a pre-existing asset here.
	 */
	static bool RollbackCreatedBlueprint(UBlueprint* BP, FString& OutError)
	{
		if (!BP)
		{
			OutError = TEXT("rollback skipped — Blueprint pointer was null");
			return false;
		}

		// Clear editor selection before ForceDeleteObjects. The delete pass issues a
		// NoteSelectionChange → UpdatePivotLocationForSelection element walk that
		// asserts if USelection holds any dangling/unregistered TypedElementHandle
		// (which earlier handlers can leave behind). Deselecting first guarantees
		// the pass iterates over nothing.
		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true, /*WarnAboutTools*/ false);
			if (USelection* Sel = GEditor->GetSelectedActors())     Sel->DeselectAll();
			if (USelection* Sel = GEditor->GetSelectedObjects())    Sel->DeselectAll();
			if (USelection* Sel = GEditor->GetSelectedComponents()) Sel->DeselectAll();
		}

		TArray<UObject*> ObjectsToDelete;
		ObjectsToDelete.Add(BP);

		const int32 DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*bShowConfirmation*/ false);
		if (DeletedCount <= 0)
		{
			OutError = TEXT("ForceDeleteObjects deleted 0 objects — phantom asset may still be registered in memory");
			return false;
		}
		return true;
	}
}

// ============================================================================
// Handler: POST /gas/setup_actor
// Body: { blueprint_path, replication_mode?, save? }
//
// Ensures the target Blueprint has a UAbilitySystemComponent SCS node, sets
// its replication mode, and recompiles. Idempotent: if an ASC already exists
// the call updates its replication mode (when supplied) without adding a
// second component.
// ============================================================================
bool FNGGHttpServer::HandleGasSetupActor(
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

	FString BlueprintPath;
	Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
	if (BlueprintPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint_path is required")));
		return true;
	}

	FString ReplicationMode = TEXT("Mixed");
	Body->TryGetStringField(TEXT("replication_mode"), ReplicationMode);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BlueprintPath, ReplicationMode, bSave]()
	{
		UBlueprint* BP = NGGGasPriv::LoadBlueprint(BlueprintPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("UBlueprint not found at '%s'"), *BlueprintPath)));
			return;
		}

		bool bAdded = false;
		UAbilitySystemComponent* ASC = NGGGasPriv::FindOrAddAscToBlueprint(BP, /*bAdd*/ true, bAdded);
		if (!ASC)
		{
			Callback(JsonError(500,
				TEXT("Failed to find or add UAbilitySystemComponent — Blueprint has no SimpleConstructionScript or component template was null")));
			return;
		}

		ASC->Modify();
		ASC->SetReplicationMode(NGGGasPriv::ParseReplicationMode(ReplicationMode));

		FKismetEditorUtilities::CompileBlueprint(BP);
		BP->MarkPackageDirty();

		bool bSaved = false;
		if (bSave) bSaved = NGGGasPriv::GasSavePackageForAsset(BP);

		// A requested-but-failed save must not be reported as success — the change
		// only lives in memory and would be lost, leaving the caller believing it
		// was persisted.
		if (bSave && !bSaved)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("/gas/setup_actor: '%s' was modified but SavePackage failed — change not persisted to disk"),
				*BlueprintPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/gas/setup_actor: %s asc_added=%d replication_mode=%s saved=%d"),
			*BlueprintPath, bAdded ? 1 : 0, *ReplicationMode, bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),          true);
		Out->SetStringField(TEXT("blueprint_path"),   BlueprintPath);
		Out->SetBoolField  (TEXT("asc_added"),        bAdded);
		Out->SetStringField(TEXT("replication_mode"), ReplicationMode);
		Out->SetBoolField  (TEXT("saved"),            bSaved);
		Callback(JsonOk(NGGGasPriv::GasSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /gas/create_attribute_set
// Body: {
//   asset_path,
//   parent_class?,                       // defaults to UAttributeSet
//   attributes?: [ { name, default_value? }, ... ],
//   save?
// }
// Creates a Blueprint subclass of the requested AttributeSet base class and
// appends an FGameplayAttributeData member variable for each attribute spec.
// default_value is parsed best-effort via the standard variable default-value
// path; complex composite defaults are left as warnings.
// ============================================================================
bool FNGGHttpServer::HandleGasCreateAttributeSet(
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

	FString ParentClassRef;
	Body->TryGetStringField(TEXT("parent_class"), ParentClassRef);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	// Marshal attribute specs.
	struct FAttrSpec
	{
		FString Name;
		bool    bHasDefault = false;
		double  DefaultValue = 0.0;
	};
	TArray<FAttrSpec> Attrs;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("attributes"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				if (!O.IsValid()) continue;
				FAttrSpec S;
				O->TryGetStringField(TEXT("name"), S.Name);
				if (S.Name.IsEmpty()) continue;
				S.bHasDefault = O->TryGetNumberField(TEXT("default_value"), S.DefaultValue);
				Attrs.Add(MoveTemp(S));
			}
		}
	}

	AsyncTask(ENamedThreads::GameThread,
		[Callback, AssetPath, ParentClassRef, bSave, Attrs = MoveTemp(Attrs)]()
	{
		UClass* ParentClass = NGGGasPriv::ResolveClassRefOrFallback(ParentClassRef, UAttributeSet::StaticClass());
		if (!ParentClass || !ParentClass->IsChildOf(UAttributeSet::StaticClass()))
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("parent_class '%s' did not resolve to a UAttributeSet subclass"),
				*ParentClassRef)));
			return;
		}

		FString CreateErr;
		bool bAlreadyExisted = false;
		UBlueprint* BP = NGGGasPriv::CreateBlueprintAsset(AssetPath, ParentClass, CreateErr, bAlreadyExisted);
		if (!BP)
		{
			Callback(JsonError(500, CreateErr));
			return;
		}

		// Build the FGameplayAttributeData pin type once.
		FEdGraphPinType AttrPinType;
		AttrPinType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
		AttrPinType.PinSubCategoryObject = FGameplayAttributeData::StaticStruct();
		AttrPinType.ContainerType        = EPinContainerType::None;

		TArray<TSharedPtr<FJsonValue>> AddedJson;
		TArray<FString>                Warnings;

		for (const FAttrSpec& S : Attrs)
		{
			const FName VarName(*S.Name);

			// Skip if the variable already exists on this Blueprint (idempotent re-runs).
			if (FBlueprintEditorUtils::FindNewVariableIndex(BP, VarName) != INDEX_NONE)
			{
				Warnings.Add(FString::Printf(
					TEXT("attribute '%s': already exists on this Blueprint, skipped"), *S.Name));
				continue;
			}

			// AddMemberVariable accepts a textual default — for FGameplayAttributeData
			// the struct literal "(BaseValue=N,CurrentValue=N)" works because
			// FGameplayAttributeData has BaseValue + CurrentValue UPROPERTYs.
			const FString DefaultText = S.bHasDefault
				? FString::Printf(TEXT("(BaseValue=%f,CurrentValue=%f)"), S.DefaultValue, S.DefaultValue)
				: FString();
			const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(BP, VarName, AttrPinType, DefaultText);
			if (!bAdded)
			{
				Warnings.Add(FString::Printf(
					TEXT("attribute '%s': AddMemberVariable returned false"), *S.Name));
				continue;
			}

			TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), S.Name);
			if (S.bHasDefault) J->SetNumberField(TEXT("default_value"), S.DefaultValue);
			AddedJson.Add(MakeShared<FJsonValueObject>(J));
		}

		FKismetEditorUtilities::CompileBlueprint(BP);
		BP->MarkPackageDirty();

		bool bSaved = false;
		if (bSave) bSaved = NGGGasPriv::GasSavePackageForAsset(BP);

		// A requested-but-failed save must not be reported as success — the asset
		// only lives in memory and would be lost, leaving the caller believing it
		// was persisted. For an asset THIS call created (not a pre-existing one),
		// roll it back so a failed save doesn't leave a phantom Blueprint registered
		// in memory / the asset registry (the non-transactional gap).
		if (bSave && !bSaved)
		{
			if (!bAlreadyExisted)
			{
				FString RollbackErr;
				const bool bRolledBack = NGGGasPriv::RollbackCreatedBlueprint(BP, RollbackErr);
				FString Msg = bRolledBack
					? FString::Printf(TEXT("/gas/create_attribute_set: '%s' was built but SavePackage failed — the partial asset was rolled back, nothing persisted"), *AssetPath)
					: FString::Printf(TEXT("/gas/create_attribute_set: '%s' was built but SavePackage failed AND rollback of the partial asset failed (%s) — a phantom asset may remain in memory"), *AssetPath, *RollbackErr);
				Callback(JsonError(500, Msg));
				return;
			}
			Callback(JsonError(500, FString::Printf(
				TEXT("/gas/create_attribute_set: '%s' was modified but SavePackage failed — change not persisted to disk"),
				*AssetPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/gas/create_attribute_set: %s parent=%s already_existed=%d added=%d warnings=%d saved=%d"),
			*AssetPath, *ParentClass->GetPathName(),
			bAlreadyExisted ? 1 : 0, AddedJson.Num(), Warnings.Num(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),          true);
		Out->SetStringField(TEXT("asset_path"),       AssetPath);
		Out->SetBoolField  (TEXT("already_existed"),  bAlreadyExisted);
		Out->SetStringField(TEXT("parent_class"),     ParentClass->GetPathName());
		Out->SetArrayField (TEXT("attributes_added"), AddedJson);

		TArray<TSharedPtr<FJsonValue>> WJson;
		for (const FString& W : Warnings) WJson.Add(MakeShared<FJsonValueString>(W));
		Out->SetArrayField (TEXT("warnings"),         WJson);
		Out->SetBoolField  (TEXT("saved"),            bSaved);
		Callback(bAlreadyExisted ? JsonOk(NGGGasPriv::GasSerializeJson(Out))
		                         : JsonCreated(NGGGasPriv::GasSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /gas/create_ability
// Body: {
//   asset_path,
//   parent_class?,
//   net_execution_policy?,
//   instancing_policy?,
//   ability_tags?: [...],
//   block_ability_tags?: [...],
//   cancel_abilities_tags?: [...],
//   cost_effect?,        // /Game/... path to a UGameplayEffect class
//   cooldown_effect?,    // same
//   activation_group?,   // accepted but currently no-op (not in stock UE5.7 GAS)
//   save?
// }
// Creates a UGameplayAbility subclass Blueprint and writes the requested
// settings onto its CDO. Compiles, marks dirty, saves.
// ============================================================================
bool FNGGHttpServer::HandleGasCreateAbility(
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

	FString ParentClassRef;
	Body->TryGetStringField(TEXT("parent_class"), ParentClassRef);

	FString NetPolicyStr;
	Body->TryGetStringField(TEXT("net_execution_policy"), NetPolicyStr);
	FString InstancingPolicyStr;
	Body->TryGetStringField(TEXT("instancing_policy"), InstancingPolicyStr);
	FString ActivationGroupStr;
	Body->TryGetStringField(TEXT("activation_group"), ActivationGroupStr);

	FString CostEffectRef;
	Body->TryGetStringField(TEXT("cost_effect"), CostEffectRef);
	FString CooldownEffectRef;
	Body->TryGetStringField(TEXT("cooldown_effect"), CooldownEffectRef);

	const TArray<TSharedPtr<FJsonValue>>* AbilityTagsArr     = nullptr; Body->TryGetArrayField(TEXT("ability_tags"),          AbilityTagsArr);
	const TArray<TSharedPtr<FJsonValue>>* BlockTagsArr       = nullptr; Body->TryGetArrayField(TEXT("block_ability_tags"),    BlockTagsArr);
	const TArray<TSharedPtr<FJsonValue>>* CancelTagsArr      = nullptr; Body->TryGetArrayField(TEXT("cancel_abilities_tags"), CancelTagsArr);

	// Copy tag specs into TArrays we can move into the lambda (the const* aliases
	// borrow into Body which won't outlive the dispatch).
	auto CopyArr = [](const TArray<TSharedPtr<FJsonValue>>* Src) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (Src) Out = *Src;
		return Out;
	};
	TArray<TSharedPtr<FJsonValue>> AbilityTags = CopyArr(AbilityTagsArr);
	TArray<TSharedPtr<FJsonValue>> BlockTags   = CopyArr(BlockTagsArr);
	TArray<TSharedPtr<FJsonValue>> CancelTags  = CopyArr(CancelTagsArr);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, AssetPath, ParentClassRef, NetPolicyStr, InstancingPolicyStr,
		 ActivationGroupStr, CostEffectRef, CooldownEffectRef, bSave,
		 AbilityTags = MoveTemp(AbilityTags),
		 BlockTags   = MoveTemp(BlockTags),
		 CancelTags  = MoveTemp(CancelTags)]() mutable
	{
		UClass* ParentClass = NGGGasPriv::ResolveClassRefOrFallback(ParentClassRef, UGameplayAbility::StaticClass());
		if (!ParentClass || !ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("parent_class '%s' did not resolve to a UGameplayAbility subclass"),
				*ParentClassRef)));
			return;
		}

		FString CreateErr;
		bool bAlreadyExisted = false;
		UBlueprint* BP = NGGGasPriv::CreateBlueprintAsset(AssetPath, ParentClass, CreateErr, bAlreadyExisted);
		if (!BP)
		{
			Callback(JsonError(500, CreateErr));
			return;
		}

		// Compile first so GeneratedClass / CDO are valid.
		FKismetEditorUtilities::CompileBlueprint(BP);
		if (!BP->GeneratedClass)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("Blueprint '%s' has no GeneratedClass after compile"), *AssetPath)));
			return;
		}
		UGameplayAbility* CDO = Cast<UGameplayAbility>(BP->GeneratedClass->GetDefaultObject());
		if (!CDO)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CDO for '%s' is not a UGameplayAbility"), *AssetPath)));
			return;
		}

		CDO->Modify();

		TArray<FString> Warnings;

		// NetExecutionPolicy and InstancingPolicy are protected in UE5.7 — write via reflection.
		auto WriteEnumProp = [](UObject* Obj, const FName PropName, uint8 Value) -> bool
		{
			FByteProperty* Prop = CastField<FByteProperty>(Obj->GetClass()->FindPropertyByName(PropName));
			if (!Prop) return false;
			Prop->SetPropertyValue_InContainer(Obj, Value);
			return true;
		};
		auto WriteClassProp = [](UObject* Obj, const FName PropName, UClass* Value) -> bool
		{
			FClassProperty* Prop = CastField<FClassProperty>(Obj->GetClass()->FindPropertyByName(PropName));
			if (!Prop) return false;
			Prop->SetObjectPropertyValue_InContainer(Obj, Value);
			return true;
		};
		auto WriteTagContainerProp = [](UObject* Obj, const FName PropName, const FGameplayTagContainer& Tags) -> bool
		{
			FStructProperty* SP = CastField<FStructProperty>(Obj->GetClass()->FindPropertyByName(PropName));
			if (!SP) return false;
			FGameplayTagContainer* Ptr = SP->ContainerPtrToValuePtr<FGameplayTagContainer>(Obj);
			if (!Ptr) return false;
			*Ptr = Tags;
			return true;
		};

		if (!NetPolicyStr.IsEmpty())
		{
			if (!WriteEnumProp(CDO, TEXT("NetExecutionPolicy"), (uint8)NGGGasPriv::ParseNetPolicy(NetPolicyStr)))
				Warnings.Add(TEXT("NetExecutionPolicy: property not found via reflection — field may have moved in this UE version"));
		}
		if (!InstancingPolicyStr.IsEmpty())
		{
			if (!WriteEnumProp(CDO, TEXT("InstancingPolicy"), (uint8)NGGGasPriv::ParseInstancingPolicy(InstancingPolicyStr)))
				Warnings.Add(TEXT("InstancingPolicy: property not found via reflection"));
		}
		if (!ActivationGroupStr.IsEmpty())
		{
			// EGameplayAbilityActivationGroup is a Lyra/GAS-Companion concept,
			// not present on stock UE5.7 UGameplayAbility. Surface as a warning
			// rather than silently ignore.
			Warnings.Add(FString::Printf(
				TEXT("activation_group='%s' ignored — not a stock UE5.7 UGameplayAbility field"),
				*ActivationGroupStr));
		}

		bool bAnyTagsSet = false;
		if (AbilityTags.Num() > 0)
		{
			// AbilityTags is deprecated (renamed to AssetTags) in UE5.7 — suppress the warning.
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			CDO->AbilityTags = NGGGasPriv::ParseTagContainer(&AbilityTags);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			bAnyTagsSet = true;
		}
		if (BlockTags.Num() > 0)
		{
			if (WriteTagContainerProp(CDO, TEXT("BlockAbilitiesWithTag"), NGGGasPriv::ParseTagContainer(&BlockTags)))
				bAnyTagsSet = true;
			else
				Warnings.Add(TEXT("BlockAbilitiesWithTag: property not found via reflection"));
		}
		if (CancelTags.Num() > 0)
		{
			if (WriteTagContainerProp(CDO, TEXT("CancelAbilitiesWithTag"), NGGGasPriv::ParseTagContainer(&CancelTags)))
				bAnyTagsSet = true;
			else
				Warnings.Add(TEXT("CancelAbilitiesWithTag: property not found via reflection"));
		}

		auto LoadEffectClass = [&Warnings](const FString& Ref, const TCHAR* FieldLabel) -> TSubclassOf<UGameplayEffect>
		{
			if (Ref.IsEmpty()) return nullptr;
			FString Path = Ref;
			if (Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT(".")))
			{
				const FString Name = FPaths::GetBaseFilename(Path);
				Path = Path + TEXT(".") + Name;
			}
			UClass* C = LoadObject<UClass>(nullptr, *Path);
			if (!C)
			{
				const FString WithSuffix = Path.EndsWith(TEXT("_C")) ? Path : (Path + TEXT("_C"));
				C = LoadObject<UClass>(nullptr, *WithSuffix);
			}
			if (!C || !C->IsChildOf(UGameplayEffect::StaticClass()))
			{
				Warnings.Add(FString::Printf(
					TEXT("%s '%s' did not resolve to a UGameplayEffect subclass"), FieldLabel, *Ref));
				return nullptr;
			}
			return TSubclassOf<UGameplayEffect>(C);
		};

		if (!CostEffectRef.IsEmpty())
		{
			TSubclassOf<UGameplayEffect> Cls = LoadEffectClass(CostEffectRef, TEXT("cost_effect"));
			if (Cls && !WriteClassProp(CDO, TEXT("CostGameplayEffectClass"), Cls.Get()))
				Warnings.Add(TEXT("CostGameplayEffectClass: property not found via reflection"));
		}
		if (!CooldownEffectRef.IsEmpty())
		{
			TSubclassOf<UGameplayEffect> Cls = LoadEffectClass(CooldownEffectRef, TEXT("cooldown_effect"));
			if (Cls && !WriteClassProp(CDO, TEXT("CooldownGameplayEffectClass"), Cls.Get()))
				Warnings.Add(TEXT("CooldownGameplayEffectClass: property not found via reflection"));
		}

		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);

		bool bSaved = false;
		if (bSave) bSaved = NGGGasPriv::GasSavePackageForAsset(BP);

		// A requested-but-failed save must not be reported as success — the asset
		// only lives in memory and would be lost, leaving the caller believing it
		// was persisted. For an asset THIS call created (not a pre-existing one),
		// roll it back so a failed save doesn't leave a phantom Blueprint registered
		// in memory / the asset registry (the non-transactional gap).
		if (bSave && !bSaved)
		{
			if (!bAlreadyExisted)
			{
				FString RollbackErr;
				const bool bRolledBack = NGGGasPriv::RollbackCreatedBlueprint(BP, RollbackErr);
				FString Msg = bRolledBack
					? FString::Printf(TEXT("/gas/create_ability: '%s' was built but SavePackage failed — the partial asset was rolled back, nothing persisted"), *AssetPath)
					: FString::Printf(TEXT("/gas/create_ability: '%s' was built but SavePackage failed AND rollback of the partial asset failed (%s) — a phantom asset may remain in memory"), *AssetPath, *RollbackErr);
				Callback(JsonError(500, Msg));
				return;
			}
			Callback(JsonError(500, FString::Printf(
				TEXT("/gas/create_ability: '%s' was modified but SavePackage failed — change not persisted to disk"),
				*AssetPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/gas/create_ability: %s parent=%s already_existed=%d net=%s inst=%s tags=%d cost='%s' cooldown='%s' warnings=%d saved=%d"),
			*AssetPath, *ParentClass->GetPathName(),
			bAlreadyExisted ? 1 : 0,
			*NetPolicyStr, *InstancingPolicyStr,
			bAnyTagsSet ? 1 : 0,
			*CostEffectRef, *CooldownEffectRef,
			Warnings.Num(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),              true);
		Out->SetStringField(TEXT("asset_path"),           AssetPath);
		Out->SetBoolField  (TEXT("already_existed"),      bAlreadyExisted);
		Out->SetStringField(TEXT("parent_class"),         ParentClass->GetPathName());
		Out->SetStringField(TEXT("net_execution_policy"), NGGGasPriv::NetPolicyToString(CDO->GetNetExecutionPolicy()));
		Out->SetStringField(TEXT("instancing_policy"),    NGGGasPriv::InstancingPolicyToString(CDO->GetInstancingPolicy()));
		Out->SetBoolField  (TEXT("tags_set"),             bAnyTagsSet);

		TArray<TSharedPtr<FJsonValue>> WJson;
		for (const FString& W : Warnings) WJson.Add(MakeShared<FJsonValueString>(W));
		Out->SetArrayField (TEXT("warnings"),             WJson);
		Out->SetBoolField  (TEXT("saved"),                bSaved);
		Callback(bAlreadyExisted ? JsonOk(NGGGasPriv::GasSerializeJson(Out))
		                         : JsonCreated(NGGGasPriv::GasSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /gas/create_effect
// Body: {
//   asset_path,
//   parent_class?,
//   duration_policy?,                    // "Instant" | "HasDuration" | "Infinite"
//   duration?,                           // float — used only for HasDuration
//   period?,                             // float
//   stacking_type?,                      // "None" | "AggregateBySource" | "AggregateByTarget"
//   stack_limit?,                        // int — 0 / -1 = no limit
//   modifiers?: [ { attribute, operation, magnitude }, ... ],
//   granted_tags?: [...],
//   application_required_tags?: [...],
//   ongoing_required_tags?: [...],
//   immunity_tags?: [...],               // accepted but no-op on stock UE5.7
//   save?
// }
// ============================================================================
bool FNGGHttpServer::HandleGasCreateEffect(
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

	FString ParentClassRef;
	Body->TryGetStringField(TEXT("parent_class"), ParentClassRef);

	FString DurationPolicyStr;
	Body->TryGetStringField(TEXT("duration_policy"), DurationPolicyStr);
	double Duration = 0.0;     bool bHasDuration   = Body->TryGetNumberField(TEXT("duration"),    Duration);
	double Period   = 0.0;     bool bHasPeriod     = Body->TryGetNumberField(TEXT("period"),      Period);
	int32  StackLimit = 0;     bool bHasStackLimit = Body->TryGetNumberField(TEXT("stack_limit"), StackLimit);
	FString StackingTypeStr;   Body->TryGetStringField(TEXT("stacking_type"), StackingTypeStr);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	struct FModSpec
	{
		FString Attribute;
		FString Operation;
		double  Magnitude = 0.0;
	};
	TArray<FModSpec> Mods;
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Body->TryGetArrayField(TEXT("modifiers"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				if (!O.IsValid()) continue;
				FModSpec M;
				O->TryGetStringField(TEXT("attribute"), M.Attribute);
				O->TryGetStringField(TEXT("operation"), M.Operation);
				O->TryGetNumberField(TEXT("magnitude"), M.Magnitude);
				if (!M.Attribute.IsEmpty()) Mods.Add(MoveTemp(M));
			}
		}
	}

	auto CopyArr = [](const TArray<TSharedPtr<FJsonValue>>* Src) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (Src) Out = *Src;
		return Out;
	};
	const TArray<TSharedPtr<FJsonValue>>* GrantedArr  = nullptr; Body->TryGetArrayField(TEXT("granted_tags"),                GrantedArr);
	const TArray<TSharedPtr<FJsonValue>>* AppReqArr   = nullptr; Body->TryGetArrayField(TEXT("application_required_tags"),   AppReqArr);
	const TArray<TSharedPtr<FJsonValue>>* OngoingArr  = nullptr; Body->TryGetArrayField(TEXT("ongoing_required_tags"),       OngoingArr);
	const TArray<TSharedPtr<FJsonValue>>* ImmunityArr = nullptr; Body->TryGetArrayField(TEXT("immunity_tags"),               ImmunityArr);

	TArray<TSharedPtr<FJsonValue>> GrantedTags  = CopyArr(GrantedArr);
	TArray<TSharedPtr<FJsonValue>> AppReqTags   = CopyArr(AppReqArr);
	TArray<TSharedPtr<FJsonValue>> OngoingTags  = CopyArr(OngoingArr);
	TArray<TSharedPtr<FJsonValue>> ImmunityTags = CopyArr(ImmunityArr);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, AssetPath, ParentClassRef, DurationPolicyStr,
		 Duration, bHasDuration, Period, bHasPeriod,
		 StackLimit, bHasStackLimit, StackingTypeStr, bSave,
		 Mods         = MoveTemp(Mods),
		 GrantedTags  = MoveTemp(GrantedTags),
		 AppReqTags   = MoveTemp(AppReqTags),
		 OngoingTags  = MoveTemp(OngoingTags),
		 ImmunityTags = MoveTemp(ImmunityTags)]() mutable
	{
		UClass* ParentClass = NGGGasPriv::ResolveClassRefOrFallback(ParentClassRef, UGameplayEffect::StaticClass());
		if (!ParentClass || !ParentClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			Callback(JsonError(400, FString::Printf(
				TEXT("parent_class '%s' did not resolve to a UGameplayEffect subclass"),
				*ParentClassRef)));
			return;
		}

		FString CreateErr;
		bool bAlreadyExisted = false;
		UBlueprint* BP = NGGGasPriv::CreateBlueprintAsset(AssetPath, ParentClass, CreateErr, bAlreadyExisted);
		if (!BP)
		{
			Callback(JsonError(500, CreateErr));
			return;
		}

		FKismetEditorUtilities::CompileBlueprint(BP);
		if (!BP->GeneratedClass)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("Blueprint '%s' has no GeneratedClass after compile"), *AssetPath)));
			return;
		}
		UGameplayEffect* CDO = Cast<UGameplayEffect>(BP->GeneratedClass->GetDefaultObject());
		if (!CDO)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("CDO for '%s' is not a UGameplayEffect"), *AssetPath)));
			return;
		}

		CDO->Modify();

		TArray<FString> Warnings;

		// Duration / period.
		if (!DurationPolicyStr.IsEmpty())
		{
			CDO->DurationPolicy = NGGGasPriv::ParseDurationPolicy(DurationPolicyStr);
		}
		if (CDO->DurationPolicy == EGameplayEffectDurationType::HasDuration && bHasDuration)
		{
			CDO->DurationMagnitude = FGameplayEffectModifierMagnitude(
				FScalableFloat(static_cast<float>(Duration)));
		}
		if (bHasPeriod)
		{
			CDO->Period = FScalableFloat(static_cast<float>(Period));
		}

		// Stacking. SetStackingType/GetStackingType are declared but not exported in UE5.7 binaries,
		// so we use property reflection to write/read StackingType directly.
		if (!StackingTypeStr.IsEmpty())
		{
			bool bStackingValid = false;
			const EGameplayEffectStackingType T = NGGGasPriv::ParseStackingType(StackingTypeStr, bStackingValid);
			if (!bStackingValid)
			{
				// Reject rather than silently persisting None — otherwise a typo'd
				// stacking_type would be written to the asset and saved as garbage.
				Callback(JsonError(400, FString::Printf(
					TEXT("stacking_type '%s' is invalid — expected None, AggregateBySource, or AggregateByTarget"),
					*StackingTypeStr)));
				return;
			}

			FProperty* StackProp = CDO->GetClass()->FindPropertyByName(TEXT("StackingType"));
			const int64 EnumVal = static_cast<int64>(T);
			if (FByteProperty* Prop = CastField<FByteProperty>(StackProp))
			{
				// FByteProperty may be a plain byte or a TEnumAsByte-backed enum; if it
				// carries an enum, confirm the value is a declared entry before writing.
				if (UEnum* Enum = Prop->GetIntPropertyEnum(); Enum && !Enum->IsValidEnumValue(EnumVal))
				{
					Callback(JsonError(400, FString::Printf(
						TEXT("stacking_type '%s' (=%lld) is not a valid '%s' value"),
						*StackingTypeStr, EnumVal, *Enum->GetName())));
					return;
				}
				Prop->SetPropertyValue_InContainer(CDO, static_cast<uint8>(T));
			}
			else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(StackProp))
			{
				// UE5.7 typically reflects enum UPROPERTYs as FEnumProperty; write via the underlying numeric prop.
				if (UEnum* Enum = EnumProp->GetEnum(); Enum && !Enum->IsValidEnumValue(EnumVal))
				{
					Callback(JsonError(400, FString::Printf(
						TEXT("stacking_type '%s' (=%lld) is not a valid '%s' value"),
						*StackingTypeStr, EnumVal, *Enum->GetName())));
					return;
				}
				FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
				Underlying->SetIntPropertyValue(
					EnumProp->ContainerPtrToValuePtr<void>(CDO), EnumVal);
			}
			else
			{
				Warnings.Add(FString::Printf(
					TEXT("stacking_type '%s' not applied — 'StackingType' is neither FByteProperty nor FEnumProperty"),
					*StackingTypeStr));
			}
		}
		if (bHasStackLimit)
		{
			CDO->StackLimitCount = StackLimit;
		}

		// Modifiers.
		int32 ModifiersAdded = 0;
		for (const FModSpec& M : Mods)
		{
			FGameplayModifierInfo Info;
			Info.Attribute = NGGGasPriv::ResolveAttribute(M.Attribute);
			if (!Info.Attribute.IsValid())
			{
				Warnings.Add(FString::Printf(
					TEXT("modifier attribute '%s' did not resolve — modifier skipped (would never apply at runtime)"),
					*M.Attribute));
				continue;
			}
			Info.ModifierOp        = NGGGasPriv::ParseModOp(M.Operation);
			Info.ModifierMagnitude = FGameplayEffectModifierMagnitude(
				FScalableFloat(static_cast<float>(M.Magnitude)));
			CDO->Modifiers.Add(Info);
			++ModifiersAdded;
		}

		// Granted tags via the legacy FInheritedTagContainer (deprecated in 5.3 — modern path is
		// UAssetTagsGameplayEffectComponent). We keep the legacy write for broadest compatibility;
		// suppress the deprecation warning explicitly so the rest of the file's warnings stay clean.
		if (GrantedTags.Num() > 0)
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			CDO->InheritableGameplayEffectTags.Added = NGGGasPriv::ParseTagContainer(&GrantedTags);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}

		// Application / ongoing requirement tags. These are deprecated as of 5.3
		// in favour of UTargetTagRequirementsGameplayEffectComponent but remain
		// readable/writable on the CDO; we keep the legacy path so existing
		// content keeps round-tripping cleanly.
		if (AppReqTags.Num() > 0)
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			CDO->ApplicationTagRequirements.RequireTags = NGGGasPriv::ParseTagContainer(&AppReqTags);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		if (OngoingTags.Num() > 0)
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			CDO->OngoingTagRequirements.RequireTags = NGGGasPriv::ParseTagContainer(&OngoingTags);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
		}
		if (ImmunityTags.Num() > 0)
		{
			// Stock UE5.7 GAS no longer exposes a direct immunity_tags container
			// on UGameplayEffect — the modern path is UImmunityGameplayEffectComponent.
			// We surface this so callers know their request was acknowledged but
			// not applied, instead of silently dropping it.
			Warnings.Add(TEXT("immunity_tags ignored — UE5.7 stock UGameplayEffect has no direct immunity tag field; use UImmunityGameplayEffectComponent"));
		}

		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);

		bool bSaved = false;
		if (bSave) bSaved = NGGGasPriv::GasSavePackageForAsset(BP);

		// A requested-but-failed save must not be reported as success — the asset
		// only lives in memory and would be lost, leaving the caller believing it
		// was persisted. For an asset THIS call created (not a pre-existing one),
		// roll it back so a failed save doesn't leave a phantom Blueprint registered
		// in memory / the asset registry (the non-transactional gap).
		if (bSave && !bSaved)
		{
			if (!bAlreadyExisted)
			{
				FString RollbackErr;
				const bool bRolledBack = NGGGasPriv::RollbackCreatedBlueprint(BP, RollbackErr);
				FString Msg = bRolledBack
					? FString::Printf(TEXT("/gas/create_effect: effect '%s' was built but SavePackage failed — the partial asset was rolled back, nothing persisted"), *AssetPath)
					: FString::Printf(TEXT("/gas/create_effect: effect '%s' was built but SavePackage failed AND rollback of the partial asset failed (%s) — a phantom asset may remain in memory"), *AssetPath, *RollbackErr);
				Callback(JsonError(500, Msg));
				return;
			}
			Callback(JsonError(500, FString::Printf(
				TEXT("/gas/create_effect: effect '%s' was modified but SavePackage failed — change not persisted to disk"),
				*AssetPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/gas/create_effect: %s parent=%s already_existed=%d duration_policy=%s mods=%d warnings=%d saved=%d"),
			*AssetPath, *ParentClass->GetPathName(),
			bAlreadyExisted ? 1 : 0,
			*NGGGasPriv::DurationPolicyToString(CDO->DurationPolicy),
			ModifiersAdded, Warnings.Num(), bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetStringField(TEXT("asset_path"),      AssetPath);
		Out->SetBoolField  (TEXT("already_existed"), bAlreadyExisted);
		Out->SetStringField(TEXT("parent_class"),    ParentClass->GetPathName());
		Out->SetStringField(TEXT("duration_policy"), NGGGasPriv::DurationPolicyToString(CDO->DurationPolicy));
		Out->SetNumberField(TEXT("modifier_count"),  ModifiersAdded);

		TArray<TSharedPtr<FJsonValue>> WJson;
		for (const FString& W : Warnings) WJson.Add(MakeShared<FJsonValueString>(W));
		Out->SetArrayField (TEXT("warnings"),        WJson);
		Out->SetBoolField  (TEXT("saved"),           bSaved);
		Callback(bAlreadyExisted ? JsonOk(NGGGasPriv::GasSerializeJson(Out))
		                         : JsonCreated(NGGGasPriv::GasSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /gas/configure_asc
// Body: { blueprint_path, replication_mode?, save? }
// Updates an existing ASC's replication mode. Errors 404 if the Blueprint has
// no AbilitySystemComponent — use /gas/setup_actor to add one first.
// ============================================================================
bool FNGGHttpServer::HandleGasConfigureAsc(
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

	FString BlueprintPath;
	Body->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
	if (BlueprintPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint_path is required")));
		return true;
	}

	FString ReplicationMode = TEXT("Mixed");
	Body->TryGetStringField(TEXT("replication_mode"), ReplicationMode);

	bool bSave = true;
	Body->TryGetBoolField(TEXT("save"), bSave);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BlueprintPath, ReplicationMode, bSave]()
	{
		UBlueprint* BP = NGGGasPriv::LoadBlueprint(BlueprintPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("UBlueprint not found at '%s'"), *BlueprintPath)));
			return;
		}

		bool bAddedDummy = false;
		UAbilitySystemComponent* ASC = NGGGasPriv::FindOrAddAscToBlueprint(BP, /*bAdd*/ false, bAddedDummy);
		if (!ASC)
		{
			Callback(JsonError(404,
				TEXT("No AbilitySystemComponent found on blueprint — call /gas/setup_actor first")));
			return;
		}

		ASC->Modify();
		ASC->SetReplicationMode(NGGGasPriv::ParseReplicationMode(ReplicationMode));

		BP->MarkPackageDirty();
		FKismetEditorUtilities::CompileBlueprint(BP);

		bool bSaved = false;
		if (bSave) bSaved = NGGGasPriv::GasSavePackageForAsset(BP);

		// A requested-but-failed save must not be reported as success — the change
		// only lives in memory and would be lost, leaving the caller believing it
		// was persisted.
		if (bSave && !bSaved)
		{
			Callback(JsonError(500, FString::Printf(
				TEXT("/gas/configure_asc: '%s' was modified but SavePackage failed — change not persisted to disk"),
				*BlueprintPath)));
			return;
		}

		UE_LOG(LogNGGBridge, Log,
			TEXT("/gas/configure_asc: %s replication_mode=%s saved=%d"),
			*BlueprintPath, *ReplicationMode, bSaved ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),          true);
		Out->SetStringField(TEXT("blueprint_path"),   BlueprintPath);
		Out->SetStringField(TEXT("replication_mode"), ReplicationMode);
		Out->SetBoolField  (TEXT("saved"),            bSaved);
		Callback(JsonOk(NGGGasPriv::GasSerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: GET /gas/read_setup?blueprint_path=/Game/...
// Reads back the GAS-relevant configuration of a Blueprint:
//   - whether it has an ASC (and its replication mode)
//   - what UAttributeSet subclasses it embeds as components
//   - if it IS an ability or effect, the corresponding CDO settings
// ============================================================================
bool FNGGHttpServer::HandleGasReadSetup(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	const FString BlueprintPath = GetQueryParam(Req, TEXT("blueprint_path"));
	if (BlueprintPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("?blueprint_path=/Game/... is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BlueprintPath]()
	{
		UBlueprint* BP = NGGGasPriv::LoadBlueprint(BlueprintPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("UBlueprint not found at '%s'"), *BlueprintPath)));
			return;
		}

		// ASC + AttributeSet scan via the Blueprint's SCS.
		TSharedRef<FJsonObject> AscJson = MakeShared<FJsonObject>();
		AscJson->SetBoolField(TEXT("present"), false);

		TArray<TSharedPtr<FJsonValue>> AttrSetsJson;

		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate) continue;
				UActorComponent* T = Node->ComponentTemplate;
				if (UAbilitySystemComponent* AsAsc = Cast<UAbilitySystemComponent>(T))
				{
					AscJson->SetBoolField  (TEXT("present"),          true);
					AscJson->SetStringField(TEXT("replication_mode"), NGGGasPriv::ReplicationModeToString(AsAsc->ReplicationMode));
					AscJson->SetStringField(TEXT("component_name"),   Node->GetVariableName().ToString());
				}
				else if (T->IsA<UAttributeSet>())
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("component_name"), Node->GetVariableName().ToString());
					J->SetStringField(TEXT("class_name"),     T->GetClass()->GetPathName());
					AttrSetsJson.Add(MakeShared<FJsonValueObject>(J));
				}
			}
		}

		// CDO-based introspection (ability vs effect vs attribute set blueprint).
		FString GasRole = TEXT("Unknown");
		TSharedPtr<FJsonObject> AbilityJson;
		TSharedPtr<FJsonObject> EffectJson;

		UClass* GenClass = BP->GeneratedClass ? BP->GeneratedClass.Get() : nullptr;
		UObject* CDO = GenClass ? GenClass->GetDefaultObject() : nullptr;

		// Reflection helpers for protected UGameplayAbility fields in UE5.7.
		auto ReadTagContainerProp = [](UObject* Obj, FName PropName) -> FGameplayTagContainer
		{
			FGameplayTagContainer Out;
			FStructProperty* SP = CastField<FStructProperty>(Obj->GetClass()->FindPropertyByName(PropName));
			if (SP)
			{
				const FGameplayTagContainer* Ptr = SP->ContainerPtrToValuePtr<FGameplayTagContainer>(Obj);
				if (Ptr) Out = *Ptr;
			}
			return Out;
		};
		auto ReadSubclassOfProp = [](UObject* Obj, FName PropName) -> FString
		{
			FClassProperty* CP = CastField<FClassProperty>(Obj->GetClass()->FindPropertyByName(PropName));
			if (CP)
			{
				UClass* C = Cast<UClass>(CP->GetObjectPropertyValue_InContainer(Obj));
				if (C) return C->GetPathName();
			}
			return FString();
		};

		if (UGameplayAbility* AsAbility = Cast<UGameplayAbility>(CDO))
		{
			GasRole = TEXT("GameplayAbility");
			AbilityJson = MakeShared<FJsonObject>();
			AbilityJson->SetStringField(TEXT("net_execution_policy"), NGGGasPriv::NetPolicyToString(AsAbility->GetNetExecutionPolicy()));
			AbilityJson->SetStringField(TEXT("instancing_policy"),    NGGGasPriv::InstancingPolicyToString(AsAbility->GetInstancingPolicy()));
			// AbilityTags, BlockAbilitiesWithTag, CancelAbilitiesWithTag are all protected in UE5.7.
			AbilityJson->SetArrayField (TEXT("ability_tags"),         NGGGasPriv::TagContainerToJson(ReadTagContainerProp(AsAbility, TEXT("AbilityTags"))));
			AbilityJson->SetArrayField (TEXT("block_ability_tags"),   NGGGasPriv::TagContainerToJson(ReadTagContainerProp(AsAbility, TEXT("BlockAbilitiesWithTag"))));
			AbilityJson->SetArrayField (TEXT("cancel_abilities_tags"),NGGGasPriv::TagContainerToJson(ReadTagContainerProp(AsAbility, TEXT("CancelAbilitiesWithTag"))));
			AbilityJson->SetStringField(TEXT("cost_effect"),          ReadSubclassOfProp(AsAbility, TEXT("CostGameplayEffectClass")));
			AbilityJson->SetStringField(TEXT("cooldown_effect"),      ReadSubclassOfProp(AsAbility, TEXT("CooldownGameplayEffectClass")));
		}
		else if (UGameplayEffect* AsEffect = Cast<UGameplayEffect>(CDO))
		{
			GasRole = TEXT("GameplayEffect");
			EffectJson = MakeShared<FJsonObject>();
			EffectJson->SetStringField(TEXT("duration_policy"), NGGGasPriv::DurationPolicyToString(AsEffect->DurationPolicy));

			// Try to surface the literal duration if it's a ScalableFloat-only magnitude.
			float DurationLiteral = 0.0f;
			AsEffect->DurationMagnitude.GetStaticMagnitudeIfPossible(/*Level*/ 1.0f, DurationLiteral);
			EffectJson->SetNumberField(TEXT("duration"),       DurationLiteral);
			EffectJson->SetNumberField(TEXT("period"),         AsEffect->Period.GetValueAtLevel(1.0f));
			// GetStackingType() not exported in UE5.7 — read via reflection.
			EGameplayEffectStackingType StackType = EGameplayEffectStackingType::None;
			FProperty* StackProp = AsEffect->GetClass()->FindPropertyByName(TEXT("StackingType"));
			if (FByteProperty* SP = CastField<FByteProperty>(StackProp))
			{
				StackType = static_cast<EGameplayEffectStackingType>(SP->GetPropertyValue_InContainer(AsEffect));
			}
			else if (FEnumProperty* EP = CastField<FEnumProperty>(StackProp))
			{
				// UE5.7 typically reflects enum UPROPERTYs as FEnumProperty; read via the underlying numeric prop.
				FNumericProperty* Underlying = EP->GetUnderlyingProperty();
				StackType = static_cast<EGameplayEffectStackingType>(
					Underlying->GetSignedIntPropertyValue(EP->ContainerPtrToValuePtr<void>(AsEffect)));
			}
			else
			{
				EffectJson->SetStringField(TEXT("stacking_type_warning"),
					TEXT("'StackingType' is neither FByteProperty nor FEnumProperty — reported value may be inaccurate"));
			}
			EffectJson->SetStringField(TEXT("stacking_type"), NGGGasPriv::StackingTypeToString(StackType));
			EffectJson->SetNumberField(TEXT("stack_limit"),    AsEffect->StackLimitCount);
			EffectJson->SetNumberField(TEXT("modifier_count"), AsEffect->Modifiers.Num());
		}
		else if (CDO && CDO->IsA<UAttributeSet>())
		{
			GasRole = TEXT("AttributeSet");
		}
		else if (BP->ParentClass &&
			(BP->ParentClass->IsChildOf(AActor::StaticClass()) ||
			 BP->ParentClass->IsChildOf(UActorComponent::StaticClass())))
		{
			// Actor / Component blueprint that owns an ASC counts as the
			// "AbilitySystemActor" role; otherwise leave it Unknown.
			if (AscJson->GetBoolField(TEXT("present")))
			{
				GasRole = TEXT("AbilitySystemActor");
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetStringField(TEXT("blueprint_path"),  BlueprintPath);
		Out->SetStringField(TEXT("gas_role"),        GasRole);
		Out->SetObjectField(TEXT("asc"),             AscJson);
		Out->SetArrayField (TEXT("attribute_sets"),  AttrSetsJson);
		if (AbilityJson.IsValid()) Out->SetObjectField(TEXT("ability"), AbilityJson);
		else                       Out->SetField(TEXT("ability"), MakeShared<FJsonValueNull>());
		if (EffectJson.IsValid())  Out->SetObjectField(TEXT("effect"),  EffectJson);
		else                       Out->SetField(TEXT("effect"),  MakeShared<FJsonValueNull>());

		Callback(JsonOk(NGGGasPriv::GasSerializeJson(Out)));
	});

	return true;
}
