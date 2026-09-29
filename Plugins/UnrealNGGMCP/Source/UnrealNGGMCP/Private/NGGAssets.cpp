// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGAssets.cpp
//
// Implementation of the asset-registry and asset-lifecycle endpoints:
// /health, /project/info, /assets/{list,get,duplicate,create,set_prop,
// set_map_entries,delete}, /editor/{save_all,reimport}, /gameplaytags/list and
// /assets/import.
//
// These are the routes that answer "what is in this project" and "put this
// thing in it". They share the reflection helpers on FNGGHttpServer
// (PropertyToJson / SetPropertyFromJson), which stay in NGGHttpServer.cpp.

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
#include "Factories/DataAssetFactory.h"
#include "FileHelpers.h"          // UEditorLoadingAndSavingUtils
#include "EditorReimportHandler.h"// FReimportManager
#include "AssetImportTask.h"
#include "ObjectTools.h"
#include "PackageTools.h"
// ---- UObject reflection ----------------------------------------------------
#include "UObject/PropertyIterator.h"
#include "JsonObjectConverter.h"
// ---- Gameplay Tags ---------------------------------------------------------
#include "GameplayTagsManager.h"
// ---- Texture import (legacy factory, avoids Interchange crash) -------------
#include "Factories/TextureFactory.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture.h"
// ---- Delete: selection must let go of an asset before it can be destroyed --
#include "Selection.h"  // USelection — full definition for DeselectAll/Deselect
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
// ---- Asset kinds /assets/get reports structure for ------------------------
// UE5.8: the Engine/ forwarding header was removed; canonical path is CoreUObject/StructUtils.
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
// ---- /health and /project_info ---------------------------------------------
#include "Misc/App.h"                          // FApp::GetProjectName
#include "HAL/FileManager.h"                   // IFileManager — Source/*.Target.cs scan
#include "Runtime/Launch/Resources/Version.h"  // ENGINE_MAJOR_VERSION and friends


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

	// Version surface. Without this a plugin built against a different engine —
	// or a sidecar newer than the bridge it is talking to — shows up as an
	// assortment of odd 404s and shape mismatches instead of one clear message.
	//
	// engine_version is what this binary was COMPILED against (ENGINE_*_VERSION
	// are compile-time macros), which is exactly the question that matters when
	// a route misbehaves: the running editor and the plugin binary can only
	// differ if someone copied a stale DLL in.
	Body->SetStringField(TEXT("engine_version"),
		FString::Printf(TEXT("%d.%d.%d"),
			ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION, ENGINE_PATCH_VERSION));
	Body->SetStringField(TEXT("plugin_version"),   TEXT(NGG_PLUGIN_VERSION));
	Body->SetNumberField(TEXT("bridge_protocol"),  NGG_BRIDGE_PROTOCOL);
	Body->SetBoolField(TEXT("auth_required"),      !AuthToken.IsEmpty());

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
