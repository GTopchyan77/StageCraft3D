// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGBlueprintAssets.cpp
//
// Implementation of the Blueprint *asset* endpoints — creating and reshaping
// Blueprints, their component hierarchy (SCS), and the user-defined struct /
// enum / interface assets they depend on:
//   /blueprints/{create,set_defaults,reparent}
//   /components/{set_defaults,get_defaults,add,remove}
//   /blueprints/{create_struct,create_enum,create_interface,create_anim}
//
// Graph authoring (nodes, pins, wiring) lives in NGGBlueprintGraph.cpp; this
// file is about the asset and its class layout.

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
// ---- Blueprint creation / reparenting --------------------------------------
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Factories/BlueprintFactory.h"
#include "BlueprintEditorLibrary.h"
// ---- Blueprint component CDO helpers ---------------------------------------
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
// ---- User-defined struct / enum / interface authoring ----------------------
// UE5.8: the Engine/ forwarding header was removed; canonical path is CoreUObject/StructUtils.
#include "StructUtils/UserDefinedStruct.h"
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
// ---- Anim Blueprint asset creation -----------------------------------------
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Factories/AnimBlueprintFactory.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/PropertyIterator.h"

// ============================================================================
// Cross-TU helpers (defined in NGGBlueprintGraph.cpp)
// ============================================================================
namespace NGGBpPriv
{
	bool ResolveVariablePinType(const FString& TypeStr, FEdGraphPinType& OutType, FString& OutError);
}

// ============================================================================
// Property-set helper: routes Static/SkeletalMesh assignments through the
// component's setter so internal caches stay in sync. Raw ImportText on these
// pointer properties bypasses UStaticMeshComponent::SetStaticMesh() and trips
// UStaticMeshComponent::OutdatedKnownStaticMeshDetected on the next thumbnail
// refresh. Returns true if the property was handled specially; out-params
// indicate success vs failure. Callers fall back to ImportText_Direct if the
// function returns false.
//
// Not static: /actors/update needs the same behaviour, so NGGLevel.cpp forward-
// declares this and links against the definition here.
// ============================================================================
bool TrySetMeshPropertyViaSetter(
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
// Local helpers for the asset-creation handlers below
// ============================================================================
namespace NGGAssetCreatePriv
{
	/** Serialize a JSON object to a string. */
	static FString AssetSerializeJson(const TSharedRef<FJsonObject>& Obj)
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
			Callback(JsonOk(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
		Callback(JsonCreated(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
			Callback(JsonOk(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
		Callback(JsonCreated(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
			Callback(JsonOk(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
		Callback(JsonCreated(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
			Callback(JsonOk(NGGAssetCreatePriv::AssetSerializeJson(Out)));
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
		Callback(JsonCreated(NGGAssetCreatePriv::AssetSerializeJson(Out)));
	});

	return true;
}
