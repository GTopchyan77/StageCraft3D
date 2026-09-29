// Copyright 2025-2026 NGG. All Rights Reserved.
//
// NGGBlueprintGraph.cpp
//
// Implementation of the /bp/* HTTP endpoints that expose Blueprint Event Graph
// authoring (K2 node creation + pin wiring + compile) to MCP clients.
//
// Threading contract:
//   - All UnrealEd/Blueprint APIs must run on the Game Thread.
//   - Every handler wraps its work in AsyncTask(ENamedThreads::GameThread).

#include "NGGHttpServer.h"
#include "UnrealNGGMCPModule.h"

// ---- HTTP / JSON -----------------------------------------------------------
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ---- Engine ---------------------------------------------------------------
#include "Async/Async.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformTime.h"

// ---- Blueprint / K2 -------------------------------------------------------
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Self.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/CompilerResultsLog.h"
// ---- Widget Blueprint self-heal (GUID map) -------------------------------
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Misc/Guid.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/MemberReference.h"
#include "StructUtils/UserDefinedStruct.h"

// ---- Asset Registry (for /bp/lint_project) ---------------------------------
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"

// ---- Blueprint editor selection (for /bp/get_selection) --------------------
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "BlueprintEditor.h"

// ============================================================================
// Local helpers
// ============================================================================

namespace NGGBpPriv
{
	static FString SerializeJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** Load a Blueprint by content path, trying a few sensible variants. */
	static UBlueprint* LoadBlueprintFlexible(const FString& AssetPath)
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!BP)
		{
			const FString Alt = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
			BP = LoadObject<UBlueprint>(nullptr, *Alt);
		}
		return BP;
	}

	/** Find a UEdGraph on a blueprint by case-insensitive name. Pass empty name to get EventGraph. */
	static UEdGraph* FindBlueprintGraph(UBlueprint* BP, const FString& GraphName)
	{
		if (!BP) return nullptr;
		const bool bNameWasExplicit = !GraphName.IsEmpty();
		const FString Target = GraphName.IsEmpty() ? FString(TEXT("EventGraph")) : GraphName;
		auto Matches = [&Target](UEdGraph* G) -> bool
		{
			return G && G->GetName().Equals(Target, ESearchCase::IgnoreCase);
		};
		for (UEdGraph* G : BP->UbergraphPages)    if (Matches(G)) return G;
		for (UEdGraph* G : BP->FunctionGraphs)    if (Matches(G)) return G;
		for (UEdGraph* G : BP->MacroGraphs)       if (Matches(G)) return G;
		for (UEdGraph* G : BP->IntermediateGeneratedGraphs) if (Matches(G)) return G;
		// Interface-function implementation graphs live on the interface description, not FunctionGraphs.
		for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
		{
			for (UEdGraph* G : Desc.Graphs) if (Matches(G)) return G;
		}
		// Fall back to first ubergraph ONLY when the caller did not name a specific
		// graph (i.e. the default "EventGraph" lookup). If an explicit name was
		// given and not found, return null so the caller errors clearly instead of
		// silently editing the wrong graph.
		if (!bNameWasExplicit && BP->UbergraphPages.Num() > 0)
		{
			return BP->UbergraphPages[0];
		}
		return nullptr;
	}

	/** Resolve a node-class string (short name or /Script/Pkg.Name) to UClass*. */
	static UClass* ResolveNodeClass(const FString& In)
	{
		if (In.IsEmpty()) return nullptr;

		// Try direct FindObject (accepts /Script/BlueprintGraph.K2Node_CallFunction)
		if (UClass* Direct = FindObject<UClass>(nullptr, *In))
		{
			return Direct;
		}
		// Try short-name lookup under BlueprintGraph package
		const FString Short = FPackageName::ObjectPathToObjectName(In);
		const TArray<FString> Packages = {
			TEXT("/Script/BlueprintGraph."),
			TEXT("/Script/AnimGraph."),
			TEXT("/Script/Engine."),
		};
		for (const FString& Pkg : Packages)
		{
			if (UClass* C = FindObject<UClass>(nullptr, *(Pkg + Short))) return C;
		}
		// Last resort: iterate
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == Short && It->IsChildOf(UEdGraphNode::StaticClass()))
			{
				return *It;
			}
		}
		return nullptr;
	}

	/**
	 * Resolve a user-supplied type string into an FEdGraphPinType for variable
	 * creation. Accepts canonical UE primitives ("bool", "int", "float", ...),
	 * common struct names ("FVector", "FRotator", ...), object-class paths
	 * ("/Script/Engine.Actor"), short class names ("Actor", "MaterialInstanceDynamic"),
	 * and an explicit "class:Foo" syntax for class-reference pins.
	 *
	 * Forward-declared so HandleBpCreateVariable can call it; ResolveGameClass
	 * is defined just below.
	 */
	static UClass* ResolveGameClass(const FString& In);

	// Helpers used by ResolveVariablePinType to look up enums and structs by
	// either full asset path, native object name, or short name.
	static UEnum* ResolveEnumByName(const FString& In)
	{
		if (In.IsEmpty()) return nullptr;
		if (UEnum* Direct = FindObject<UEnum>(nullptr, *In)) return Direct;
		if (UEnum* Loaded = LoadObject<UEnum>(nullptr, *In)) return Loaded;
		const FString Short = FPackageName::ObjectPathToObjectName(In);
		for (TObjectIterator<UEnum> It; It; ++It)
		{
			if (It->GetName() == Short || It->GetName() == (TEXT("E") + Short))
			{
				return *It;
			}
		}
		return nullptr;
	}

	static UScriptStruct* ResolveStructByName(const FString& In)
	{
		if (In.IsEmpty()) return nullptr;
		if (UScriptStruct* Direct = FindObject<UScriptStruct>(nullptr, *In)) return Direct;
		if (UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *In)) return Loaded;
		const FString Short = FPackageName::ObjectPathToObjectName(In);
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (It->GetName() == Short || It->GetName() == (TEXT("F") + Short))
			{
				return *It;
			}
		}
		return nullptr;
	}

	// Non-static so it can be forward-declared and reused from other TUs
	// (e.g. NGGHttpServer.cpp's struct/interface/function handlers).
	bool ResolveVariablePinType(const FString& TypeStrIn, FEdGraphPinType& OutType, FString& OutError)
	{
		OutError.Empty();
		OutType = FEdGraphPinType();

		FString TypeStr = TypeStrIn;
		TypeStr.TrimStartAndEndInline();

		if (TypeStr.IsEmpty())
		{
			OutError = TEXT("type string is empty");
			return false;
		}

		// ---- Container wrappers: array<INNER>, set<INNER>, map<KEY,VALUE> ----
		// Parsed recursively. UE5 does not allow nested containers; we reject.
		auto TryStripContainer = [&](const TCHAR* Keyword, FString& OutInner) -> bool
		{
			const int32 KeyLen = FCString::Strlen(Keyword);
			if (TypeStr.Len() < KeyLen + 1) return false;
			if (!TypeStr.Left(KeyLen).Equals(Keyword, ESearchCase::IgnoreCase)) return false;
			if (!TypeStr.EndsWith(TEXT(">"))) return false;
			OutInner = TypeStr.Mid(KeyLen, TypeStr.Len() - KeyLen - 1);
			OutInner.TrimStartAndEndInline();
			return true;
		};

		FString InnerStr;
		if (TryStripContainer(TEXT("array<"), InnerStr))
		{
			FEdGraphPinType Inner;
			if (!ResolveVariablePinType(InnerStr, Inner, OutError)) return false;
			if (Inner.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("nested containers are not supported by UE5 Blueprints");
				return false;
			}
			OutType = Inner;
			OutType.ContainerType = EPinContainerType::Array;
			return true;
		}
		if (TryStripContainer(TEXT("set<"), InnerStr))
		{
			FEdGraphPinType Inner;
			if (!ResolveVariablePinType(InnerStr, Inner, OutError)) return false;
			if (Inner.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("nested containers are not supported by UE5 Blueprints");
				return false;
			}
			OutType = Inner;
			OutType.ContainerType = EPinContainerType::Set;
			return true;
		}
		if (TryStripContainer(TEXT("map<"), InnerStr))
		{
			// Split on the top-level comma (respecting nested angle brackets).
			int32 Depth = 0;
			int32 CommaIdx = INDEX_NONE;
			for (int32 i = 0; i < InnerStr.Len(); ++i)
			{
				const TCHAR C = InnerStr[i];
				if (C == TCHAR('<')) ++Depth;
				else if (C == TCHAR('>')) --Depth;
				else if (C == TCHAR(',') && Depth == 0) { CommaIdx = i; break; }
			}
			if (CommaIdx == INDEX_NONE)
			{
				OutError = TEXT("map<K,V> requires two type arguments separated by ','");
				return false;
			}
			FString KeyStr   = InnerStr.Left(CommaIdx);   KeyStr.TrimStartAndEndInline();
			FString ValueStr = InnerStr.Mid(CommaIdx + 1); ValueStr.TrimStartAndEndInline();
			FEdGraphPinType KeyPin, ValuePin;
			if (!ResolveVariablePinType(KeyStr,   KeyPin,   OutError)) return false;
			if (!ResolveVariablePinType(ValueStr, ValuePin, OutError)) return false;
			if (KeyPin.ContainerType != EPinContainerType::None || ValuePin.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("nested containers are not supported by UE5 Blueprints");
				return false;
			}
			OutType = KeyPin;
			OutType.ContainerType = EPinContainerType::Map;
			OutType.PinValueType.TerminalCategory          = ValuePin.PinCategory;
			OutType.PinValueType.TerminalSubCategory       = ValuePin.PinSubCategory;
			OutType.PinValueType.TerminalSubCategoryObject = ValuePin.PinSubCategoryObject;
			OutType.PinValueType.bTerminalIsConst          = ValuePin.bIsConst;
			OutType.PinValueType.bTerminalIsWeakPointer    = ValuePin.bIsWeakPointer;
			return true;
		}

		// "class:Foo" — produce a class-reference pin.
		if (TypeStr.StartsWith(TEXT("class:"), ESearchCase::IgnoreCase))
		{
			const FString ClassName = TypeStr.Mid(6);
			UClass* Resolved = ResolveGameClass(ClassName);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("could not resolve class '%s'"), *ClassName);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Class;
			OutType.PinSubCategoryObject = Resolved;
			return true;
		}

		// "softclass:Foo" — TSoftClassPtr<Foo>.
		if (TypeStr.StartsWith(TEXT("softclass:"), ESearchCase::IgnoreCase))
		{
			const FString ClassName = TypeStr.Mid(10);
			UClass* Resolved = ResolveGameClass(ClassName);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("could not resolve class '%s'"), *ClassName);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			OutType.PinSubCategoryObject = Resolved;
			return true;
		}

		// "softobject:Foo" — TSoftObjectPtr<Foo>.
		if (TypeStr.StartsWith(TEXT("softobject:"), ESearchCase::IgnoreCase))
		{
			const FString ClassName = TypeStr.Mid(11);
			UClass* Resolved = ResolveGameClass(ClassName);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("could not resolve class '%s'"), *ClassName);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			OutType.PinSubCategoryObject = Resolved;
			return true;
		}

		// "object:Foo" — explicit object reference (alternative to a bare class name).
		if (TypeStr.StartsWith(TEXT("object:"), ESearchCase::IgnoreCase))
		{
			const FString ClassName = TypeStr.Mid(7);
			UClass* Resolved = ResolveGameClass(ClassName);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("could not resolve class '%s'"), *ClassName);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Resolved;
			return true;
		}

		// "interface:Foo" — interface reference.
		if (TypeStr.StartsWith(TEXT("interface:"), ESearchCase::IgnoreCase))
		{
			const FString IfaceName = TypeStr.Mid(10);
			UClass* Resolved = ResolveGameClass(IfaceName);
			if (!Resolved)
			{
				OutError = FString::Printf(TEXT("could not resolve interface '%s'"), *IfaceName);
				return false;
			}
			if (!Resolved->HasAnyClassFlags(CLASS_Interface))
			{
				OutError = FString::Printf(TEXT("'%s' is not an interface class"), *IfaceName);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Interface;
			OutType.PinSubCategoryObject = Resolved;
			return true;
		}

		// "enum:Foo" — UEnum / UUserDefinedEnum (canonical BP form: PC_Byte + UEnum subcat).
		if (TypeStr.StartsWith(TEXT("enum:"), ESearchCase::IgnoreCase))
		{
			const FString EnumName = TypeStr.Mid(5);
			UEnum* E = ResolveEnumByName(EnumName);
			if (!E)
			{
				OutError = FString::Printf(TEXT("enum '%s' not found"), *EnumName);
				return false;
			}
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutType.PinSubCategoryObject = E;
			return true;
		}

		// "struct:Foo" — UScriptStruct / UUserDefinedStruct by path or short name.
		if (TypeStr.StartsWith(TEXT("struct:"), ESearchCase::IgnoreCase))
		{
			const FString StructName = TypeStr.Mid(7);
			UScriptStruct* S = ResolveStructByName(StructName);
			if (!S)
			{
				OutError = FString::Printf(TEXT("struct '%s' not found"), *StructName);
				return false;
			}
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = S;
			return true;
		}

		const FString Low = TypeStr.ToLower();

		// Primitives
		if (Low == TEXT("bool") || Low == TEXT("boolean"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
		if (Low == TEXT("int") || Low == TEXT("int32") || Low == TEXT("integer"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (Low == TEXT("int64"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			return true;
		}
		if (Low == TEXT("float") || Low == TEXT("double") || Low == TEXT("real"))
		{
			OutType.PinCategory    = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (Low == TEXT("string") || Low == TEXT("fstring"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
			return true;
		}
		if (Low == TEXT("name") || Low == TEXT("fname"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		}
		if (Low == TEXT("text") || Low == TEXT("ftext"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
			return true;
		}
		if (Low == TEXT("byte") || Low == TEXT("uint8"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			return true;
		}

		// Common engine structs
		if (Low == TEXT("vector") || Low == TEXT("fvector"))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			return true;
		}
		if (Low == TEXT("vector2d") || Low == TEXT("fvector2d"))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
			return true;
		}
		if (Low == TEXT("rotator") || Low == TEXT("frotator"))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
			return true;
		}
		if (Low == TEXT("transform") || Low == TEXT("ftransform"))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
			return true;
		}
		if (Low == TEXT("linearcolor") || Low == TEXT("flinearcolor"))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
			return true;
		}
		if (Low == TEXT("color") || Low == TEXT("fcolor"))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FColor>::Get();
			return true;
		}

		// ---- Generic asset-path fallback ----------------------------------
		// If the string is a bare asset path or short name, try resolving it
		// as (in order) a UEnum, a UScriptStruct, then a UClass. This lets the
		// caller write `/Game/Data/E_GamePhase` or `F_BombData` without a
		// prefix and have the right pin category inferred automatically.
		if (UEnum* E = ResolveEnumByName(TypeStr))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Byte;
			OutType.PinSubCategoryObject = E;
			return true;
		}
		if (UScriptStruct* S = ResolveStructByName(TypeStr))
		{
			OutType.PinCategory          = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = S;
			return true;
		}

		// Path-style class lookup (e.g. /Script/Engine.Actor) or short class name fallback.
		UClass* Resolved = ResolveGameClass(TypeStr);
		if (Resolved)
		{
			if (Resolved->HasAnyClassFlags(CLASS_Interface))
			{
				OutType.PinCategory = UEdGraphSchema_K2::PC_Interface;
			}
			else
			{
				OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			}
			OutType.PinSubCategoryObject = Resolved;
			return true;
		}

		OutError = FString::Printf(
			TEXT("unrecognized type '%s' (try primitives like 'bool'/'int'/'float'/'FVector', ")
			TEXT("class refs like 'class:Actor' or '/Script/Engine.Actor', ")
			TEXT("'enum:E_Foo', 'struct:F_Foo', 'softclass:Foo', 'softobject:Foo', 'interface:BPI_Foo', ")
			TEXT("or container wrappers 'array<T>' / 'set<T>' / 'map<K,V>')"),
			*TypeStr);
		return false;
	}

	/** Resolve a UClass by its path string (/Script/Engine.Actor or short "Actor"). */
	static UClass* ResolveGameClass(const FString& In)
	{
		if (In.IsEmpty()) return nullptr;
		if (UClass* Direct = FindObject<UClass>(nullptr, *In)) return Direct;
		if (UClass* AnyPkg = LoadObject<UClass>(nullptr, *In)) return AnyPkg;
		const FString Short = FPackageName::ObjectPathToObjectName(In);
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == Short || It->GetName() == (TEXT("U") + Short) || It->GetName() == (TEXT("A") + Short))
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Case-insensitive pin lookup on a node. */
	static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& Name)
	{
		if (!Node) return nullptr;
		// Try exact first
		if (UEdGraphPin* P = Node->FindPin(Name)) return P;
		// Case-insensitive
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		// Try common aliases against schema constants
		const UEdGraphSchema_K2* K2 = GetDefault<UEdGraphSchema_K2>();
		auto TryAlias = [&](const FName& Schema) -> UEdGraphPin*
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinName == Schema) return Pin;
			}
			return nullptr;
		};
		const FString Low = Name.ToLower();
		if (Low == TEXT("execute") || Low == TEXT("exec") || Low == TEXT("in"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_Execute)) return P;
		}
		if (Low == TEXT("then") || Low == TEXT("out") || Low == TEXT("next"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_Then)) return P;
		}
		if (Low == TEXT("true"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_Then)) return P;
		}
		if (Low == TEXT("false"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_Else)) return P;
		}
		if (Low == TEXT("condition"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_Condition)) return P;
		}
		if (Low == TEXT("self") || Low == TEXT("target"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_Self)) return P;
		}
		if (Low == TEXT("return") || Low == TEXT("returnvalue") || Low == TEXT("result"))
		{
			if (auto* P = TryAlias(UEdGraphSchema_K2::PN_ReturnValue)) return P;
		}
		// Variable Get/Set nodes don't have a "ReturnValue" pin — their data pin
		// is named after the variable. So if a caller passes a generic alias
		// like "return" / "value" / "output" to a variable node, resolve it to
		// the variable-named pin.
		if (Low == TEXT("return") || Low == TEXT("returnvalue") || Low == TEXT("result")
			|| Low == TEXT("value") || Low == TEXT("output"))
		{
			if (UK2Node_Variable* VN = Cast<UK2Node_Variable>(Node))
			{
				const FName VarName = VN->VariableReference.GetMemberName();
				if (!VarName.IsNone())
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin && Pin->PinName == VarName) return Pin;
					}
					// Case-insensitive fallback against the variable name string
					const FString VarStr = VarName.ToString();
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin && Pin->PinName.ToString().Equals(VarStr, ESearchCase::IgnoreCase))
						{
							return Pin;
						}
					}
				}
			}
		}
		return nullptr;
	}

	/** Apply a pin default value. Handles object pins by loading the asset. */
	static bool ApplyPinDefault(UEdGraphPin* Pin, const FString& Value, FString& OutError)
	{
		if (!Pin) { OutError = TEXT("pin is null"); return false; }
		const FName Cat = Pin->PinType.PinCategory;
		const bool bIsObjectish =
			Cat == UEdGraphSchema_K2::PC_Object ||
			Cat == UEdGraphSchema_K2::PC_Class  ||
			Cat == UEdGraphSchema_K2::PC_SoftObject ||
			Cat == UEdGraphSchema_K2::PC_SoftClass ||
			Cat == UEdGraphSchema_K2::PC_Interface;

		if (bIsObjectish && (Value.StartsWith(TEXT("/Game/")) || Value.StartsWith(TEXT("/Script/")) || Value.StartsWith(TEXT("/Engine/"))))
		{
			UObject* Obj = LoadObject<UObject>(nullptr, *Value);
			if (!Obj)
			{
				OutError = FString::Printf(TEXT("failed to load object '%s'"), *Value);
				return false;
			}
			Pin->DefaultObject = Obj;
			Pin->DefaultValue.Empty();
			Pin->AutogeneratedDefaultValue.Empty();
			return true;
		}

		Pin->DefaultValue = Value;
		Pin->DefaultObject = nullptr;
		return true;
	}

	/** Serialise a pin's basic type for responses. */
	static TSharedPtr<FJsonObject> PinToJson(UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		if (!Pin) return O;
		O->SetStringField(TEXT("name"), Pin->PinName.ToString());
		O->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		O->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
		// Concrete class/struct/enum behind an object/struct/enum/class pin — the
		// full path needed to distinguish engine types from project (/Game) types.
		if (UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get())
		{
			O->SetStringField(TEXT("type_object"), SubObj->GetPathName());
		}
		switch (Pin->PinType.ContainerType)
		{
			case EPinContainerType::Array: O->SetStringField(TEXT("container"), TEXT("array")); break;
			case EPinContainerType::Set:   O->SetStringField(TEXT("container"), TEXT("set"));   break;
			case EPinContainerType::Map:   O->SetStringField(TEXT("container"), TEXT("map"));   break;
			default: break;
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			O->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (Pin->DefaultObject)
		{
			O->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}
		TArray<TSharedPtr<FJsonValue>> LinkArr;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNode()) continue;
			TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
			L->SetStringField(TEXT("node"), Linked->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			L->SetStringField(TEXT("pin"),  Linked->PinName.ToString());
			LinkArr.Add(MakeShared<FJsonValueObject>(L));
		}
		O->SetArrayField(TEXT("connected_to"), LinkArr);
		return O;
	}

	/** Find an SCS component's UClass by name on a blueprint. */
	static UClass* FindSCSComponentClass(UBlueprint* BP, const FString& ComponentName)
	{
		if (!BP || !BP->SimpleConstructionScript) return nullptr;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node) continue;
			if (Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Node->ComponentClass;
			}
		}
		return nullptr;
	}

	/** Configure a node instance BEFORE AllocateDefaultPins. Returns error or empty. */
	static FString ConfigureNodePreAllocate(UK2Node* Node, UBlueprint* BP, const TSharedPtr<FJsonObject>& Config)
	{
		if (!Node) return TEXT("node is null");
		const TSharedPtr<FJsonObject> C = Config.IsValid() ? Config : MakeShared<FJsonObject>();

		if (auto* N = Cast<UK2Node_CallFunction>(Node))
		{
			const TSharedPtr<FJsonObject>* FuncObj = nullptr;
			FString FuncName;
			FString FuncClass;
			if (C->TryGetObjectField(TEXT("function"), FuncObj) && FuncObj && (*FuncObj).IsValid())
			{
				(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
				(*FuncObj)->TryGetStringField(TEXT("class"), FuncClass);
			}
			else
			{
				C->TryGetStringField(TEXT("function"), FuncName);
				C->TryGetStringField(TEXT("class"),    FuncClass);
			}
			if (FuncName.IsEmpty())
			{
				return TEXT("K2Node_CallFunction requires config.function.name");
			}
			UClass* TargetClass = nullptr;
			bool bSelfContext = false;
			C->TryGetBoolField(TEXT("self_context"), bSelfContext);
			if (bSelfContext && BP && BP->GeneratedClass)
			{
				TargetClass = BP->GeneratedClass;
			}
			else if (!FuncClass.IsEmpty())
			{
				TargetClass = ResolveGameClass(FuncClass);
			}
			if (!TargetClass)
			{
				return FString::Printf(TEXT("could not resolve function class '%s'"), *FuncClass);
			}
			UFunction* Func = TargetClass->FindFunctionByName(*FuncName);
			if (Func)
			{
				N->FunctionReference.SetFromField<UFunction>(Func, bSelfContext);
				return FString();
			}

			// Self-context fallback: a call_function targeting a custom event
			// added in the SAME add_logic batch won't resolve via
			// FindFunctionByName because the custom event isn't on
			// GeneratedClass until the next compile. Walk the BP's graphs
			// for a UK2Node_CustomEvent matching this name; if found, register
			// the call as a self-member by name (no UFunction validation —
			// the next compile will bind it).
			if (bSelfContext && BP)
			{
				const FName WantName(*FuncName);
				auto FindCustomEvent = [&WantName](const TArray<UEdGraph*>& Graphs) -> bool
				{
					for (UEdGraph* G : Graphs)
					{
						if (!G) continue;
						for (UEdGraphNode* N : G->Nodes)
						{
							if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
							{
								if (CE->CustomFunctionName == WantName) return true;
							}
						}
					}
					return false;
				};
				if (FindCustomEvent(BP->UbergraphPages) || FindCustomEvent(BP->FunctionGraphs))
				{
					N->FunctionReference.SetSelfMember(WantName);
					return FString();
				}
			}

			// Display-name -> internal C++ name alias table. Chat-window agents
			// frequently pass the friendly name shown in the BP palette
			// ("GetActorLocation", "DestroyActor") but the actual UFunction is
			// the underlying K2_* / engine-internal name. If the alias resolves
			// AND the original name doesn't, transparently rewrite — silent
			// success, not an error. Different from the UE5.7 rename table
			// below which only suggests in error text.
			static const TMap<FString, FString> DisplayNameAliases = []{
				TMap<FString, FString> M;
				// AActor
				M.Add(TEXT("GetActorLocation"),  TEXT("K2_GetActorLocation"));
				M.Add(TEXT("SetActorLocation"),  TEXT("K2_SetActorLocation"));
				M.Add(TEXT("GetActorRotation"),  TEXT("K2_GetActorRotation"));
				M.Add(TEXT("SetActorRotation"),  TEXT("K2_SetActorRotation"));
				M.Add(TEXT("DestroyActor"),      TEXT("K2_DestroyActor"));
				M.Add(TEXT("AttachToActor"),     TEXT("K2_AttachToActor"));
				M.Add(TEXT("AttachToComponent"), TEXT("K2_AttachToComponent"));
				M.Add(TEXT("TeleportTo"),        TEXT("K2_TeleportTo"));
				M.Add(TEXT("GetWorld"),          TEXT("K2_GetWorld"));
				// USceneComponent — common display-name -> internal mappings
				M.Add(TEXT("GetWorldLocation"),  TEXT("K2_GetComponentLocation"));
				M.Add(TEXT("GetWorldRotation"),  TEXT("K2_GetComponentRotation"));
				M.Add(TEXT("GetWorldScale"),     TEXT("K2_GetComponentScale"));
				M.Add(TEXT("AttachTo"),          TEXT("K2_AttachTo"));
				return M;
			}();
			if (const FString* Mapped = DisplayNameAliases.Find(FuncName))
			{
				if (UFunction* MappedFunc = TargetClass->FindFunctionByName(**Mapped))
				{
					UE_LOG(LogNGGBridge, Verbose,
						TEXT("function-alias: rewriting '%s' -> '%s' on '%s'"),
						*FuncName, **Mapped, *TargetClass->GetName());
					N->FunctionReference.SetFromField<UFunction>(MappedFunc, bSelfContext);
					return FString();
				}
			}

			// Build a UE5.7 rename suggestion table once.
			static const TMap<FString, FString> Renames = []{
				TMap<FString, FString> M;
				M.Add(TEXT("Add_FloatFloat"),          TEXT("Add_DoubleDouble"));
				M.Add(TEXT("Subtract_FloatFloat"),     TEXT("Subtract_DoubleDouble"));
				M.Add(TEXT("Multiply_FloatFloat"),     TEXT("Multiply_DoubleDouble"));
				M.Add(TEXT("Divide_FloatFloat"),       TEXT("Divide_DoubleDouble"));
				M.Add(TEXT("Less_FloatFloat"),         TEXT("Less_DoubleDouble"));
				M.Add(TEXT("Greater_FloatFloat"),      TEXT("Greater_DoubleDouble"));
				M.Add(TEXT("LessEqual_FloatFloat"),    TEXT("LessEqual_DoubleDouble"));
				M.Add(TEXT("GreaterEqual_FloatFloat"), TEXT("GreaterEqual_DoubleDouble"));
				M.Add(TEXT("EqualEqual_FloatFloat"),   TEXT("EqualEqual_DoubleDouble"));
				M.Add(TEXT("NotEqual_FloatFloat"),     TEXT("NotEqual_DoubleDouble"));
				M.Add(TEXT("Percent_FloatFloat"),      TEXT("Percent_DoubleDouble"));
				M.Add(TEXT("FMin"),                    TEXT("Min"));
				M.Add(TEXT("FMax"),                    TEXT("Max"));
				return M;
			}();
			if (const FString* Replacement = Renames.Find(FuncName))
			{
				return FString::Printf(
					TEXT("function '%s' not found on class '%s'. Did you mean '%s'? (UE5.7 renamed some math operators from _FloatFloat to _DoubleDouble.)"),
					*FuncName, *TargetClass->GetName(), **Replacement);
			}
			return FString::Printf(TEXT("function '%s' not found on class '%s'"), *FuncName, *TargetClass->GetName());
		}

		// NOTE: UK2Node_ComponentBoundEvent inherits from UK2Node_Event, so it must
		// be handled FIRST — otherwise the parent-class cast below catches it and
		// we never reach the specialized branch.
		if (auto* N = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			FString ComponentName, DelegateName;
			C->TryGetStringField(TEXT("component"), ComponentName);
			// Accept 'delegate' (explicit) OR 'event_name' (from the ergonomic helper
			// which translates the user's `event: "OnComponentHit"` to event_name).
			C->TryGetStringField(TEXT("delegate"), DelegateName);
			if (DelegateName.IsEmpty()) C->TryGetStringField(TEXT("event_name"), DelegateName);
			if (ComponentName.IsEmpty() || DelegateName.IsEmpty())
			{
				return TEXT("K2Node_ComponentBoundEvent requires config.component and config.delegate (or event)");
			}
			UClass* CompClass = FindSCSComponentClass(BP, ComponentName);
			if (!CompClass)
			{
				// Widget Blueprints keep widgets in the WidgetTree, not the SCS.
				// A widget flagged bIsVariable becomes an FObjectProperty on the
				// generated class after compile — resolve its class from there so
				// bound events (Button OnClicked, ...) work for widgets too.
				auto ResolveFromClass = [&](UClass* K) -> UClass*
				{
					if (!K) return nullptr;
					if (FObjectProperty* P = FindFProperty<FObjectProperty>(K, FName(*ComponentName)))
					{
						return P->PropertyClass;
					}
					return nullptr;
				};
				CompClass = ResolveFromClass(BP ? BP->SkeletonGeneratedClass : nullptr);
				if (!CompClass)
				{
					CompClass = ResolveFromClass(BP ? BP->GeneratedClass : nullptr);
				}
			}
			if (!CompClass)
			{
				return FString::Printf(
					TEXT("component '%s' not found on blueprint SCS or generated class (for widgets: set is_variable via ue5_style_widgets and compile first)"),
					*ComponentName);
			}
			FMulticastDelegateProperty* DelegateProp = nullptr;
			for (TFieldIterator<FMulticastDelegateProperty> It(CompClass); It; ++It)
			{
				if (It->GetFName() == FName(*DelegateName) || It->GetName().Equals(DelegateName, ESearchCase::IgnoreCase))
				{
					DelegateProp = *It;
					break;
				}
			}
			if (!DelegateProp)
			{
				return FString::Printf(TEXT("delegate '%s' not found on component class '%s'"),
					*DelegateName, *CompClass->GetName());
			}

			FObjectProperty* CompProp = nullptr;
			auto TryFind2 = [&](UClass* K)
			{
				if (!K || CompProp) return;
				CompProp = FindFProperty<FObjectProperty>(K, FName(*ComponentName));
			};
			TryFind2(BP ? BP->SkeletonGeneratedClass : nullptr);
			TryFind2(BP ? BP->GeneratedClass : nullptr);
			if (!CompProp)
			{
				return FString::Printf(TEXT("component FObjectProperty '%s' not found on generated class"), *ComponentName);
			}

			N->ComponentPropertyName = CompProp->GetFName();
			N->DelegatePropertyName  = DelegateProp->GetFName();
			if (UObject* Owner = DelegateProp->GetOwner<UObject>())
			{
				if (UClass* OwnerCls = Cast<UClass>(Owner))
				{
					N->DelegateOwnerClass = OwnerCls->GetAuthoritativeClass();
				}
			}
			if (UFunction* SigFunc = DelegateProp->SignatureFunction)
			{
				N->EventReference.SetFromField<UFunction>(SigFunc, /*bSelfContext*/ false);
				N->CustomFunctionName = FName(*FString::Printf(
					TEXT("BndEvt__%s_%s_%s_%s"),
					*BP->GetName(), *CompProp->GetName(), *N->GetName(), *SigFunc->GetName()));
			}
			N->bOverrideFunction = false;
			N->bInternalEvent    = true;
			return FString();
		}

		// NOTE: UK2Node_CustomEvent inherits from UK2Node_Event, so it must be
		// handled BEFORE the parent-class branch below. Configuring a custom
		// event through the override-event path leaves it with bOverrideFunction
		// =true pointing at a non-existent parent function, which makes
		// ConformImplementedEvents -> FixOverriddenEventSignature assert on the
		// next compile (BlueprintEditorUtils.cpp:7067).
		if (auto* N = Cast<UK2Node_CustomEvent>(Node))
		{
			FString EventName;
			C->TryGetStringField(TEXT("event_name"), EventName);
			if (EventName.IsEmpty()) C->TryGetStringField(TEXT("name"), EventName);
			if (EventName.IsEmpty())
			{
				return TEXT("K2Node_CustomEvent requires config.event_name (or name)");
			}
			N->bIsEditable        = true;
			N->bOverrideFunction  = false;
			N->bInternalEvent     = false;
			N->CustomFunctionName = FName(*EventName);
			N->EventReference     = FMemberReference();
			return FString();
		}

		if (auto* N = Cast<UK2Node_Event>(Node))
		{
			FString EventName;
			C->TryGetStringField(TEXT("event_name"), EventName);
			if (EventName.IsEmpty()) C->TryGetStringField(TEXT("name"), EventName);
			if (EventName.IsEmpty())
			{
				return TEXT("K2Node_Event requires config.event_name");
			}
			UClass* OwnerClass = BP ? BP->ParentClass : nullptr;
			UFunction* Func = OwnerClass ? OwnerClass->FindFunctionByName(*EventName) : nullptr;
			// Alias rewrite: chat-window agents commonly pass the BP display name
			// ("BeginPlay", "Tick", "ActorBeginOverlap") which doesn't resolve as
			// a UFunction — Actor's overridable events are prefixed with "Receive".
			// If the bare name fails, transparently retry with the "Receive" prefix.
			if (!Func && OwnerClass && !EventName.StartsWith(TEXT("Receive")))
			{
				const FString PrefixedName = TEXT("Receive") + EventName;
				if (UFunction* PrefixedFunc = OwnerClass->FindFunctionByName(*PrefixedName))
				{
					UE_LOG(LogNGGBridge, Verbose,
						TEXT("event-alias: rewriting '%s' -> '%s' on '%s'"),
						*EventName, *PrefixedName, *OwnerClass->GetName());
					EventName = PrefixedName;
					Func = PrefixedFunc;
				}
			}
			if (!Func)
			{
				// Refuse to create a broken override-event referencing a
				// non-existent parent function — that produces the same assert
				// in FixOverriddenEventSignature documented above. Callers who
				// want a self-defined event should use type="custom_event".
				return FString::Printf(
					TEXT("K2Node_Event '%s' is not an overridable function on '%s' — Actor override events use the 'Receive' prefix (e.g. ReceiveBeginPlay, ReceiveTick), or use type=\"custom_event\" for self-defined events"),
					*EventName,
					OwnerClass ? *OwnerClass->GetName() : TEXT("(no parent class)"));
			}
			bool bOverride = true;
			C->TryGetBoolField(TEXT("override"), bOverride);
			N->EventReference.SetFromField<UFunction>(Func, /*bIsConsideredSelfContext*/ false);
			N->bOverrideFunction = bOverride;
			return FString();
		}

		if (auto* N = Cast<UK2Node_Variable>(Node))
		{
			FString VarName;
			C->TryGetStringField(TEXT("variable"), VarName);
			if (VarName.IsEmpty()) C->TryGetStringField(TEXT("name"), VarName);
			if (VarName.IsEmpty())
			{
				return TEXT("K2Node_Variable[Get/Set] requires config.variable");
			}
			bool bSelfContext = true;
			C->TryGetBoolField(TEXT("self_context"), bSelfContext);
			UClass* OwnerClass = (BP && BP->SkeletonGeneratedClass) ? BP->SkeletonGeneratedClass : (BP ? BP->GeneratedClass : nullptr);
			if (bSelfContext)
			{
				N->VariableReference.SetSelfMember(FName(*VarName));
			}
			else
			{
				N->VariableReference.SetExternalMember(FName(*VarName), OwnerClass);
			}
			return FString();
		}

		if (auto* N = Cast<UK2Node_DynamicCast>(Node))
		{
			FString TargetClassStr;
			C->TryGetStringField(TEXT("target_class"), TargetClassStr);
			if (TargetClassStr.IsEmpty())
			{
				return TEXT("K2Node_DynamicCast requires config.target_class");
			}
			UClass* Target = ResolveGameClass(TargetClassStr);
			if (!Target)
			{
				return FString::Printf(TEXT("could not resolve target_class '%s'"), *TargetClassStr);
			}
			N->TargetType = Target;
			return FString();
		}

		if (auto* N = Cast<UK2Node_MacroInstance>(Node))
		{
			FString MacroRef;
			C->TryGetStringField(TEXT("macro"), MacroRef);
			if (MacroRef.IsEmpty())
			{
				return TEXT("K2Node_MacroInstance requires config.macro");
			}
			UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *MacroRef);
			if (!MacroGraph)
			{
				return FString::Printf(TEXT("could not load macro graph '%s'"), *MacroRef);
			}
			N->SetMacroGraph(MacroGraph);
			return FString();
		}

		// K2Node_IfThenElse, K2Node_ExecutionSequence, K2Node_Self, K2Node_Knot
		// need no pre-allocate config.
		return FString();
	}
/**
 * Self-heal: remove empty override-event duplicates.
 * If two or more K2Node_Event nodes for the same parent function exist (e.g.
 * two ReceiveAnyDamage events), UE refuses to compile ("more than one function
 * with the same name X"). This picks the "winner" — the one with the most
 * outgoing exec connections — and deletes the rest. Called automatically
 * before compile on /bp/add_logic and /bp/compile.
 * Returns number of nodes removed.
 */
static int32 AutoHealDuplicateEvents(UBlueprint* BP)
{
	if (!BP) return 0;
	int32 Removed = 0;
	TArray<UEdGraph*> Graphs;
	Graphs.Append(BP->UbergraphPages);
	Graphs.Append(BP->FunctionGraphs);

	for (UEdGraph* G : Graphs)
	{
		if (!G) continue;

		// Bucket events by MemberName. CustomEvents are skipped: they don't
		// share the override-event identity model — they collide on
		// CustomFunctionName, not EventReference.MemberName.
		TMap<FName, TArray<UK2Node_Event*>> Buckets;
		for (UEdGraphNode* N : G->Nodes)
		{
			UK2Node_Event* Evt = Cast<UK2Node_Event>(N);
			if (!Evt
				|| Evt->IsA<UK2Node_ComponentBoundEvent>()
				|| Evt->IsA<UK2Node_CustomEvent>()) continue;
			const FName MemberName = Evt->EventReference.GetMemberName();
			if (MemberName.IsNone()) continue;
			Buckets.FindOrAdd(MemberName).Add(Evt);
		}

		// Drop empties in each bucket > 1.
		for (auto& KV : Buckets)
		{
			if (KV.Value.Num() < 2) continue;
			auto ExecLinkCount = [](UK2Node_Event* E) -> int32
			{
				int32 C = 0;
				for (UEdGraphPin* P : E->Pins)
				{
					if (P && P->Direction == EGPD_Output && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						C += P->LinkedTo.Num();
					}
				}
				return C;
			};
			KV.Value.Sort([&](UK2Node_Event& A, UK2Node_Event& B) { return ExecLinkCount(&A) > ExecLinkCount(&B); });
			for (int32 i = 1; i < KV.Value.Num(); ++i)
			{
				UK2Node_Event* Loser = KV.Value[i];
				if (!Loser) continue;
				UE_LOG(LogNGGBridge, Log, TEXT("auto-heal: removing duplicate event '%s' (guid=%s)"),
					*KV.Key.ToString(),
					*Loser->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				G->RemoveNode(Loser);
				++Removed;
			}
		}
	}

	if (Removed > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
	return Removed;
}

/**
 * Self-heal: repair invalid override-event state.
 *
 * UE asserts in FixOverriddenEventSignature (BlueprintEditorUtils.cpp:7067)
 * during ConformImplementedEvents when it finds a K2Node_Event with
 * bOverrideFunction=true whose EventReference resolves to no UFunction on the
 * parent class — i.e. an "override" of a function that doesn't exist. The
 * assert kills the editor.
 *
 * Two ways this state appears:
 *  1) UK2Node_CustomEvent that was misconfigured through the override path
 *     (the historical bug fixed in ConfigureNodePreAllocate above). Recovery:
 *     downgrade it to a real custom event — clear bOverrideFunction, set
 *     CustomFunctionName from the stale member name, drop the EventReference.
 *  2) UK2Node_Event that the user requested with a name that's not on the
 *     parent class. Recovery: just delete it; the call site has been tightened
 *     to refuse this, so we only see legacy nodes here.
 *
 * Called before compile from /bp/add_logic and /bp/compile so we don't crash
 * on already-corrupt assets that were authored before the fix.
 */
static int32 AutoHealInvalidOverrideEvents(UBlueprint* BP)
{
	if (!BP) return 0;
	UClass* ParentCls = BP->ParentClass;

	int32 Healed = 0;
	TArray<UEdGraph*> Graphs;
	Graphs.Append(BP->UbergraphPages);
	Graphs.Append(BP->FunctionGraphs);

	for (UEdGraph* G : Graphs)
	{
		if (!G) continue;
		// Iterate a copy because we may RemoveNode mid-loop.
		TArray<UEdGraphNode*> Nodes = G->Nodes;
		for (UEdGraphNode* N : Nodes)
		{
			UK2Node_Event* Evt = Cast<UK2Node_Event>(N);
			if (!Evt || Evt->IsA<UK2Node_ComponentBoundEvent>()) continue;
			if (!Evt->bOverrideFunction) continue;

			const FName MemberName = Evt->EventReference.GetMemberName();
			UFunction* ParentFunc = (ParentCls && !MemberName.IsNone())
				? ParentCls->FindFunctionByName(MemberName)
				: nullptr;
			if (ParentFunc) continue; // valid override, leave alone

			if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Evt))
			{
				CE->bOverrideFunction = false;
				CE->bIsEditable       = true;
				CE->bInternalEvent    = false;
				if (CE->CustomFunctionName.IsNone() && !MemberName.IsNone())
				{
					CE->CustomFunctionName = MemberName;
				}
				CE->EventReference = FMemberReference();
				CE->ReconstructNode();
				UE_LOG(LogNGGBridge, Log,
					TEXT("auto-heal: repaired CustomEvent '%s' (was misconfigured as override) guid=%s"),
					*CE->CustomFunctionName.ToString(),
					*CE->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				++Healed;
			}
			else
			{
				UE_LOG(LogNGGBridge, Log,
					TEXT("auto-heal: removing invalid override event '%s' (not on parent '%s') guid=%s"),
					*MemberName.ToString(),
					ParentCls ? *ParentCls->GetName() : TEXT("(no parent class)"),
					*Evt->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				G->RemoveNode(Evt);
				++Healed;
			}
		}
	}

	if (Healed > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
	return Healed;
}

/**
 * Pre-compile heal: detect & repair function graphs whose override would
 * make ConformCallsToParentFunctions assert.
 *
 * Crash signature this guards against:
 *   Assertion failed: (OldName != NAME_None) && (NewName != NAME_None)
 *   Engine\Source\Editor\UnrealEd\Private\Kismet2\BlueprintEditorUtils.cpp
 *   FBlueprintEditorUtils::ReplaceFunctionReferences -> RenameGraph
 *   FConformCallsToParentFunctionUtils::ConformParentFunctionOverrides
 *
 * UE 5.7 ConformParentFunctionOverrides (BlueprintEditorUtils.cpp:6932) does:
 *
 *     const FName FunctionName =
 *         (Entry->CustomGeneratedFunctionName != NAME_None)
 *             ? Entry->CustomGeneratedFunctionName
 *             : Entry->FunctionReference.GetMemberName();
 *     if (Entry == FunctionEntryNodes[0]
 *         && !IsEventGraph(CurrentGraph)
 *         && CurrentGraph->GetFName() != FunctionName)
 *     {
 *         RenameGraph(CurrentGraph, FunctionName.ToString());  // <-- crash
 *     }
 *
 * If both `CustomGeneratedFunctionName` and `FunctionReference.GetMemberName()`
 * are NAME_None — which happens on freshly-created function graphs whose entry
 * node hasn't been initialized to point at itself yet — `FunctionName` is
 * NAME_None and the rename hits the assert.
 *
 * Failure modes we repair:
 *   1. A function graph whose UEdGraph::FName is NAME_None (corruption).
 *      We delete the graph — leaving it would assert on next compile.
 *   2. A function graph whose UK2Node_FunctionEntry references a parent
 *      function that no longer exists on ParentClass (e.g. parent C++ was
 *      renamed/removed). We clear FunctionReference and seed it with the
 *      graph's own name so the graph compiles as a regular user function.
 *   3. A non-event function graph whose first FunctionEntry has both
 *      CustomGeneratedFunctionName=None AND FunctionReference.MemberName=None
 *      while the graph itself has a real FName. We seed MemberName from the
 *      graph's FName so ConformParentFunctionOverrides sees matching names
 *      and skips the rename. This is the UE 5.7 crash path.
 *
 * Runs before compile from /bp/add_logic and /bp/compile.
 */
static int32 AutoHealStaleOverrideFunctions(UBlueprint* BP)
{
	if (!BP) return 0;
	UClass* ParentCls = BP->ParentClass;

	int32 Healed = 0;
	// Copy first because we may RemoveGraph mid-iteration.
	TArray<TObjectPtr<UEdGraph>> Graphs = BP->FunctionGraphs;

	for (UEdGraph* G : Graphs)
	{
		if (!G) continue;

		// (1) Corrupt graph name — would crash RenameGraph(OldName=None).
		if (G->GetFName() == NAME_None)
		{
			UE_LOG(LogNGGBridge, Log,
				TEXT("auto-heal: removing function graph with NAME_None on BP '%s'"),
				*BP->GetName());
			FBlueprintEditorUtils::RemoveGraph(BP, G, EGraphRemoveFlags::Recompile);
			++Healed;
			continue;
		}

		// Find the *first* function-entry — that's the one ConformParentFunctionOverrides
		// uses to compute the rename target (FunctionEntryNodes[0]).
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* N : G->Nodes)
		{
			if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N))
			{
				Entry = E;
				break;
			}
		}
		if (!Entry) continue;

		const FName MemberName   = Entry->FunctionReference.GetMemberName();
		const FName CustomName   = Entry->CustomGeneratedFunctionName;
		const bool  bIsEventGraph = FBlueprintEditorUtils::IsEventGraph(G);

		// (2) Stale parent override — MemberName references a parent function
		// that no longer exists. Clear the reference and seed MemberName from
		// the graph's own FName so the post-clear state can't fall into case (3).
		if (!MemberName.IsNone())
		{
			UFunction* ParentFunc = ParentCls
				? ParentCls->FindFunctionByName(MemberName)
				: nullptr;
			if (!ParentFunc)
			{
				UE_LOG(LogNGGBridge, Log,
					TEXT("auto-heal: clearing stale override on function '%s' (parent class '%s' has no '%s') in BP '%s'"),
					*G->GetName(),
					ParentCls ? *ParentCls->GetName() : TEXT("(no parent class)"),
					*MemberName.ToString(),
					*BP->GetName());
				Entry->FunctionReference = FMemberReference();
				Entry->FunctionReference.SetSelfMember(G->GetFName());
				Entry->ReconstructNode();
				++Healed;
				continue;
			}
			// Healthy override — fall through to case (3) check just in case the
			// graph FName is None (handled above) or matches already (no-op).
		}

		// (3) UE 5.7 crash path: ConformParentFunctionOverrides will compute
		// FunctionName = CustomName ?: MemberName. If both are None and the
		// graph isn't an event graph, it calls RenameGraph(Graph, "None")
		// which asserts inside ReplaceFunctionReferences.
		//
		// Repair: seed MemberName from the graph's FName as a self-context
		// reference. This is the same shape RenameGraph produces when it
		// renames a function graph (see BlueprintEditorUtils.cpp:2618 — the
		// RenameGraphLambda updates Entry->FunctionReference.SetMemberName).
		if (CustomName.IsNone() && MemberName.IsNone() && !bIsEventGraph)
		{
			UE_LOG(LogNGGBridge, Log,
				TEXT("auto-heal: seeding FunctionEntry MemberName from graph FName '%s' on BP '%s' (was NAME_None — would crash ConformParentFunctionOverrides)"),
				*G->GetName(), *BP->GetName());
			Entry->FunctionReference.SetSelfMember(G->GetFName());
			Entry->ReconstructNode();
			++Healed;
		}
	}

	if (Healed > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}
	return Healed;
}

/**
 * Run lint + auto-fix on BP and return a JSON report { issues, summary, clean }
 * suitable for attaching under "lint" in a write-handler response. Does NOT
 * compile or save — the caller owns those steps. Defined in the second
 * NGGBpPriv block (below), declared here so earlier handlers can call it.
 */
TSharedRef<FJsonObject> BuildLintReport(UBlueprint* BP, bool bAutoFix);

} // namespace NGGBpPriv
using namespace NGGBpPriv;

// ============================================================================
// Node-id map helpers (stored on FNGGHttpServer)
// ============================================================================

static FString MakeBpNodeKey(const FString& BpPath, const FString& NodeId)
{
	return BpPath + TEXT("::") + NodeId;
}

// Since BpNodeIdMap is a member but these helpers are in anonymous namespace,
// we defer reading/writing it to the member handlers directly (they have `this`).

// ============================================================================
// Handler: POST /bp/add_node
// ============================================================================

bool FNGGHttpServer::HandleBpAddNode(
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

	FString BpPath; Body->TryGetStringField(TEXT("blueprint"), BpPath);
	FString GraphName       = TEXT("EventGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);
	FString NodeClass; Body->TryGetStringField(TEXT("node_class"), NodeClass);
	FString NodeId;
	Body->TryGetStringField(TEXT("node_id"), NodeId);
	TSharedPtr<FJsonObject> Config;
	if (Body->HasTypedField<EJson::Object>(TEXT("config")))
	{
		Config = Body->GetObjectField(TEXT("config"));
	}
	TSharedPtr<FJsonObject> PinDefaults;
	if (Body->HasTypedField<EJson::Object>(TEXT("pin_defaults")))
	{
		PinDefaults = Body->GetObjectField(TEXT("pin_defaults"));
	}
	double PosX = 0.0, PosY = 0.0;
	if (Body->HasTypedField<EJson::Object>(TEXT("position")))
	{
		TSharedPtr<FJsonObject> Pos = Body->GetObjectField(TEXT("position"));
		Pos->TryGetNumberField(TEXT("x"), PosX);
		Pos->TryGetNumberField(TEXT("y"), PosY);
	}

	if (BpPath.IsEmpty() || NodeClass.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and node_class are required")));
		return true;
	}

	TWeakPtr<FNGGHttpServer> WeakSelf = AsWeak();
	AsyncTask(ENamedThreads::GameThread,
		[WeakSelf, Callback, BpPath, GraphName, NodeClass, NodeId, Config, PinDefaults, PosX, PosY]()
	{
		// Bail if the server was destroyed before this deferred task ran.
		TSharedPtr<FNGGHttpServer> Self = WeakSelf.Pin();
		if (!Self.IsValid()) { return; }

		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			UE_LOG(LogNGGBridge, Warning, TEXT("/bp/add_node: blueprint not found: %s"), *BpPath);
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}
		UEdGraph* Graph = FindBlueprintGraph(BP, GraphName);
		if (!Graph)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Graph '%s' not found on blueprint"), *GraphName)));
			return;
		}
		UClass* NodeCls = ResolveNodeClass(NodeClass);
		if (!NodeCls || !NodeCls->IsChildOf(UK2Node::StaticClass()))
		{
			Callback(JsonError(400, FString::Printf(TEXT("node_class '%s' is not a UK2Node subclass"), *NodeClass)));
			return;
		}

		// ---- Duplicate-event detection: reuse existing override event nodes.
		// UE's editor doesn't let you add a second ReceiveAnyDamage / ReceiveBeginPlay /
		// etc. — the event graph stores one, and "Add Event" jumps to the existing one.
		// Without this, callers that repeatedly request `event: ReceiveFoo` end up with
		// compile-breaking duplicates like "more than one function with the same name".
		if (NodeCls == UK2Node_Event::StaticClass())
		{
			FString EventName;
			if (Config.IsValid())
			{
				Config->TryGetStringField(TEXT("event_name"), EventName);
				if (EventName.IsEmpty()) Config->TryGetStringField(TEXT("name"), EventName);
			}
			if (!EventName.IsEmpty())
			{
				const FName WantName(*EventName);
				for (UEdGraphNode* Existing : Graph->Nodes)
				{
					UK2Node_Event* ExEvt = Cast<UK2Node_Event>(Existing);
					if (!ExEvt
						|| ExEvt->IsA<UK2Node_ComponentBoundEvent>()
						|| ExEvt->IsA<UK2Node_CustomEvent>()) continue;
					if (ExEvt->EventReference.GetMemberName() == WantName)
					{
						// Reuse existing. Register id mapping + return.
						if (!NodeId.IsEmpty())
						{
							Self->BpNodeIdMap.Add(
								MakeBpNodeKey(BpPath, NodeId),
								FBpNodeIdEntry{ ExEvt->NodeGuid, FPlatformTime::Seconds() });
						}
						UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_node: event '%s' already exists, reusing guid=%s"),
							*EventName, *ExEvt->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));

						TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
						Result->SetBoolField  (TEXT("success"), true);
						Result->SetBoolField  (TEXT("reused"),  true);
						Result->SetStringField(TEXT("node_id"), NodeId);
						Result->SetStringField(TEXT("node_guid"),
							ExEvt->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						Result->SetStringField(TEXT("class"), ExEvt->GetClass()->GetName());
						TArray<TSharedPtr<FJsonValue>> Pins;
						for (UEdGraphPin* Pin : ExEvt->Pins)
						{
							Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
						}
						Result->SetArrayField(TEXT("pins"), Pins);
						Callback(JsonOk(SerializeJson(Result)));
						return;
					}
				}
			}
		}

		// CustomEvents are matched by CustomFunctionName — re-adding a custom
		// event with the same name should reuse the existing one rather than
		// creating a second node that compiles to "more than one function with
		// the same name X".
		if (NodeCls == UK2Node_CustomEvent::StaticClass())
		{
			FString EventName;
			if (Config.IsValid())
			{
				Config->TryGetStringField(TEXT("event_name"), EventName);
				if (EventName.IsEmpty()) Config->TryGetStringField(TEXT("name"), EventName);
			}
			if (!EventName.IsEmpty())
			{
				const FName WantName(*EventName);
				for (UEdGraphNode* Existing : Graph->Nodes)
				{
					UK2Node_CustomEvent* ExCE = Cast<UK2Node_CustomEvent>(Existing);
					if (!ExCE) continue;
					if (ExCE->CustomFunctionName == WantName)
					{
						if (!NodeId.IsEmpty())
						{
							Self->BpNodeIdMap.Add(
								MakeBpNodeKey(BpPath, NodeId),
								FBpNodeIdEntry{ ExCE->NodeGuid, FPlatformTime::Seconds() });
						}
						UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_node: custom_event '%s' already exists, reusing guid=%s"),
							*EventName, *ExCE->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));

						TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
						Result->SetBoolField  (TEXT("success"), true);
						Result->SetBoolField  (TEXT("reused"),  true);
						Result->SetStringField(TEXT("node_id"), NodeId);
						Result->SetStringField(TEXT("node_guid"),
							ExCE->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						Result->SetStringField(TEXT("class"), ExCE->GetClass()->GetName());
						TArray<TSharedPtr<FJsonValue>> Pins;
						for (UEdGraphPin* Pin : ExCE->Pins)
						{
							Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
						}
						Result->SetArrayField(TEXT("pins"), Pins);
						Callback(JsonOk(SerializeJson(Result)));
						return;
					}
				}
			}
		}

		// Same for component_event: duplicate binding to the same comp+delegate
		// is illegal. Reuse existing if present.
		if (NodeCls == UK2Node_ComponentBoundEvent::StaticClass())
		{
			FString CompName, DelName;
			if (Config.IsValid())
			{
				Config->TryGetStringField(TEXT("component"), CompName);
				Config->TryGetStringField(TEXT("delegate"),  DelName);
				if (DelName.IsEmpty()) Config->TryGetStringField(TEXT("event_name"), DelName);
			}
			if (!CompName.IsEmpty() && !DelName.IsEmpty())
			{
				const FName WantComp(*CompName), WantDel(*DelName);
				for (UEdGraphNode* Existing : Graph->Nodes)
				{
					UK2Node_ComponentBoundEvent* ExCE = Cast<UK2Node_ComponentBoundEvent>(Existing);
					if (!ExCE) continue;
					if (ExCE->ComponentPropertyName == WantComp && ExCE->DelegatePropertyName == WantDel)
					{
						if (!NodeId.IsEmpty())
						{
							Self->BpNodeIdMap.Add(
								MakeBpNodeKey(BpPath, NodeId),
								FBpNodeIdEntry{ ExCE->NodeGuid, FPlatformTime::Seconds() });
						}
						UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_node: component_event '%s.%s' already exists, reusing guid=%s"),
							*CompName, *DelName, *ExCE->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
						Result->SetBoolField  (TEXT("success"), true);
						Result->SetBoolField  (TEXT("reused"),  true);
						Result->SetStringField(TEXT("node_id"), NodeId);
						Result->SetStringField(TEXT("node_guid"),
							ExCE->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						Result->SetStringField(TEXT("class"), ExCE->GetClass()->GetName());
						TArray<TSharedPtr<FJsonValue>> Pins;
						for (UEdGraphPin* Pin : ExCE->Pins) Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
						Result->SetArrayField(TEXT("pins"), Pins);
						Callback(JsonOk(SerializeJson(Result)));
						return;
					}
				}
			}
		}

		UK2Node* Node = NewObject<UK2Node>(Graph, NodeCls);
		if (!Node)
		{
			Callback(JsonError(500, FString::Printf(TEXT("failed to allocate node of class '%s'"), *NodeClass)));
			return;
		}
		Node->CreateNewGuid();
		// Configure BEFORE adding so AllocateDefaultPins sees the right function/event/variable ref
		const FString CfgErr = ConfigureNodePreAllocate(Node, BP, Config);
		if (!CfgErr.IsEmpty())
		{
			// Node was NewObject'd outered to the graph but never added/allocated.
			// Destroy it so we don't leave a half-initialized, pin-less node behind.
			Node->DestroyNode();
			UE_LOG(LogNGGBridge, Warning, TEXT("/bp/add_node: config failed: %s"), *CfgErr);
			Callback(JsonError(400, CfgErr));
			return;
		}

		// Undo-state hygiene: mark the graph (and node) modified before mutating.
		Graph->Modify();
		Node->Modify();
		Graph->AddNode(Node, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		Node->NodePosX = FMath::RoundToInt(PosX);
		Node->NodePosY = FMath::RoundToInt(PosY);
		Node->AllocateDefaultPins();

		// Apply pin defaults (after pins exist)
		if (PinDefaults.IsValid())
		{
			// UE5.8: FJsonObject::Values is keyed by UE::FSharedString, not FString.
			// Deref to const TCHAR* so the FString& parameter binds.
			for (const auto& KV : PinDefaults->Values)
			{
				UEdGraphPin* Pin = FindPinByName(Node, *KV.Key);
				if (!Pin) continue;
				FString ValStr;
				if (KV.Value.IsValid() && KV.Value->Type == EJson::String)
				{
					ValStr = KV.Value->AsString();
				}
				else if (KV.Value.IsValid())
				{
					// numbers / bools coerce to string
					KV.Value->TryGetString(ValStr);
					if (ValStr.IsEmpty() && KV.Value->Type == EJson::Boolean)
					{
						ValStr = KV.Value->AsBool() ? TEXT("true") : TEXT("false");
					}
					else if (ValStr.IsEmpty() && KV.Value->Type == EJson::Number)
					{
						ValStr = FString::SanitizeFloat(KV.Value->AsNumber());
					}
				}
				FString PinErr;
				ApplyPinDefault(Pin, ValStr, PinErr);
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		// Register the user-supplied node_id in the transient map
		if (!NodeId.IsEmpty())
		{
			Self->BpNodeIdMap.Add(
				MakeBpNodeKey(BpPath, NodeId),
				FBpNodeIdEntry{ Node->NodeGuid, FPlatformTime::Seconds() });
		}
		// Evict old entries (>600s)
		const double Now = FPlatformTime::Seconds();
		TArray<FString> ToRemove;
		for (const auto& KV : Self->BpNodeIdMap)
		{
			if ((Now - KV.Value.CreatedAtSeconds) > 600.0) ToRemove.Add(KV.Key);
		}
		for (const FString& K : ToRemove) Self->BpNodeIdMap.Remove(K);

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_node: %s::%s -> node %s guid=%s"),
			*BpPath, *GraphName, *NodeCls->GetName(),
			*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));

		// Build response
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField  (TEXT("success"), true);
		Result->SetStringField(TEXT("node_id"), NodeId);
		Result->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Result->SetStringField(TEXT("class"), NodeCls->GetName());
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
		}
		Result->SetArrayField(TEXT("pins"), Pins);
		Callback(JsonOk(SerializeJson(Result)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/connect_pins
// ============================================================================

bool FNGGHttpServer::HandleBpConnectPins(
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

	FString BpPath; Body->TryGetStringField(TEXT("blueprint"), BpPath);
	FString GraphName = TEXT("EventGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);
	FString FromNode; Body->TryGetStringField(TEXT("from_node"), FromNode);
	FString FromPin;  Body->TryGetStringField(TEXT("from_pin"),  FromPin);
	FString ToNode;   Body->TryGetStringField(TEXT("to_node"),   ToNode);
	FString ToPin;    Body->TryGetStringField(TEXT("to_pin"),    ToPin);

	if (BpPath.IsEmpty() || FromNode.IsEmpty() || ToNode.IsEmpty() || FromPin.IsEmpty() || ToPin.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint, from_node, from_pin, to_node, to_pin are all required")));
		return true;
	}

	TWeakPtr<FNGGHttpServer> WeakSelf = AsWeak();
	AsyncTask(ENamedThreads::GameThread,
		[WeakSelf, Callback, BpPath, GraphName, FromNode, FromPin, ToNode, ToPin]()
	{
		// Bail if the server was destroyed before this deferred task ran.
		TSharedPtr<FNGGHttpServer> Self = WeakSelf.Pin();
		if (!Self.IsValid()) { return; }

		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}
		UEdGraph* Graph = FindBlueprintGraph(BP, GraphName);
		if (!Graph)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Graph '%s' not found"), *GraphName)));
			return;
		}

		auto ResolveGuid = [&Self, &BpPath](const FString& In) -> FGuid
		{
			FGuid G;
			if (FGuid::Parse(In, G)) return G;
			if (const FBpNodeIdEntry* Entry = Self->BpNodeIdMap.Find(MakeBpNodeKey(BpPath, In)))
			{
				return Entry->NodeGuid;
			}
			return FGuid();
		};
		const FGuid FromGuid = ResolveGuid(FromNode);
		const FGuid ToGuid   = ResolveGuid(ToNode);

		UEdGraphNode* A = nullptr;
		UEdGraphNode* B = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->NodeGuid == FromGuid) A = N;
			if (N && N->NodeGuid == ToGuid)   B = N;
		}
		if (!A || !B)
		{
			Callback(JsonError(404, FString::Printf(TEXT("node not found: from='%s' to='%s'"), *FromNode, *ToNode)));
			return;
		}
		UEdGraphPin* PA = FindPinByName(A, FromPin);
		UEdGraphPin* PB = FindPinByName(B, ToPin);
		if (!PA && !PB)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("pin not found on either side: from_pin='%s' to_pin='%s' (call ue5_bp_read_graph to see real pin names)"),
				*FromPin, *ToPin)));
			return;
		}
		if (!PA)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("pin not found: from_pin='%s' (to_pin='%s' resolved OK; check the source pin's real name)"),
				*FromPin, *ToPin)));
			return;
		}
		if (!PB)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("pin not found: to_pin='%s' (from_pin='%s' resolved OK; check the destination pin's real name)"),
				*ToPin, *FromPin)));
			return;
		}

		// Direction-fix for variable_set: a UK2Node_VariableSet has both an
		// INPUT data pin named '<VarName>' (the new value) and an OUTPUT data
		// pin named 'Output_Get' (the just-set value). When a caller writes
		// {from: "set_X.<VarName>"} they almost certainly want the output
		// passthrough, but FindPinByName returns the input. TryCreateConnection
		// will then refuse with "directionality". Patch by swapping to
		// 'Output_Get' when the resolved pin's direction conflicts with its
		// connection role.
		if (PA && PA->Direction != EGPD_Output && Cast<UK2Node_VariableSet>(A))
		{
			if (UEdGraphPin* OutGet = A->FindPin(TEXT("Output_Get")))
			{
				PA = OutGet;
			}
		}
		if (PB && PB->Direction != EGPD_Input && Cast<UK2Node_VariableSet>(B))
		{
			if (UEdGraphPin* OutGet = B->FindPin(TEXT("Output_Get")))
			{
				PB = OutGet;
			}
		}

		const UEdGraphSchema_K2* K2 = GetDefault<UEdGraphSchema_K2>();

		// Probe first: TryCreateConnection silently breaks an existing single-link
		// connection on DISALLOW responses, so inspect CanCreateConnection and
		// reject up-front with the schema's own reason if the link is illegal.
		const FPinConnectionResponse Resp = K2->CanCreateConnection(PA, PB);
		if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
		{
			const FString Why = Resp.Message.ToString();
			UE_LOG(LogNGGBridge, Warning, TEXT("/bp/connect_pins: connection disallowed: %s.%s -> %s.%s (%s)"),
				*A->GetName(), *FromPin, *B->GetName(), *ToPin, *Why);
			Callback(JsonError(400, FString::Printf(
				TEXT("connection disallowed: %s.%s -> %s.%s (%s)"),
				*FromPin, *A->GetName(), *ToPin, *B->GetName(),
				Why.IsEmpty() ? TEXT("type mismatch or directionality") : *Why)));
			return;
		}

		// Undo-state hygiene: mark graph + both endpoint nodes before mutating links.
		Graph->Modify();
		A->Modify();
		B->Modify();
		const bool bOk = K2->TryCreateConnection(PA, PB);
		if (!bOk)
		{
			UE_LOG(LogNGGBridge, Warning, TEXT("/bp/connect_pins: TryCreateConnection failed: %s.%s -> %s.%s"),
				*A->GetName(), *FromPin, *B->GetName(), *ToPin);
			Callback(JsonError(400, TEXT("TryCreateConnection refused the link (type mismatch or directionality)")));
			return;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		UE_LOG(LogNGGBridge, Log, TEXT("/bp/connect_pins: connected %s.%s -> %s.%s"),
			*A->GetName(), *FromPin, *B->GetName(), *ToPin);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Callback(JsonOk(SerializeJson(Result)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/compile
// ============================================================================

bool FNGGHttpServer::HandleBpCompile(
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

	FString BpPath; Body->TryGetStringField(TEXT("blueprint"), BpPath);
	bool bSave = false;
	Body->TryGetBoolField(TEXT("save"), bSave);
	if (BpPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, bSave]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}
		// Auto-heal WidgetBlueprint variable GUIDs BEFORE marking structurally
		// modified — MarkBlueprintAsStructurallyModified triggers compile, which
		// runs FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids and
		// fires an ensure for every widget in the tree without a GUID entry in
		// WidgetBP->WidgetVariableNameToGuidMap. Older plugin builds populated the
		// tree without registering GUIDs (e.g. WBP_DispatchCallUI / LocationIcon),
		// so heal those entries up-front to silence the ensure.
		int32 GuidHeals = 0;
		if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP))
		{
			if (IsValid(WBP->WidgetTree))
			{
				WBP->WidgetTree->ForEachWidget([WBP, &GuidHeals](UWidget* W)
				{
					if (W && !WBP->WidgetVariableNameToGuidMap.Contains(W->GetFName()))
					{
						WBP->WidgetVariableNameToGuidMap.Add(W->GetFName(), FGuid::NewGuid());
						++GuidHeals;
					}
				});
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		// Auto-heal: remove empty duplicate override events that would block compile.
		const int32 HealedCount = AutoHealDuplicateEvents(BP);
		// Repair invalid override-events (CustomEvents misflagged as overrides,
		// or Events whose target function doesn't exist on the parent). Without
		// this, ConformImplementedEvents -> FixOverriddenEventSignature asserts
		// and kills the editor.
		AutoHealInvalidOverrideEvents(BP);
		// Repair stale override function-graphs (parent function renamed/removed,
		// child override graph still present, or graph FName=None). Without this,
		// ConformCallsToParentFunctions -> RenameGraph -> ReplaceFunctionReferences
		// asserts (OldName != None && NewName != None) and kills the editor.
		AutoHealStaleOverrideFunctions(BP);

		// Full lint + autofix pass (orphan pins, stale refs, reinst classes,
		// bound events without their component, etc.). Runs BEFORE compile so
		// compile sees a clean graph. Unfixable issues surface in the response.
		const TSharedRef<FJsonObject> LintReport = BuildLintReport(BP, /*bAutoFix=*/true);

		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		const bool bCompileOk = (Results.NumErrors == 0);
		if (HealedCount > 0) Out->SetNumberField(TEXT("auto_healed"), HealedCount);
		if (GuidHeals    > 0) Out->SetNumberField(TEXT("widget_guid_heals"), GuidHeals);
		Out->SetObjectField(TEXT("lint"), LintReport);
		Out->SetBoolField(TEXT("success"), bCompileOk);
		Out->SetNumberField(TEXT("errors"),   Results.NumErrors);
		Out->SetNumberField(TEXT("warnings"), Results.NumWarnings);

		TArray<TSharedPtr<FJsonValue>> MsgArr;
		for (const TSharedRef<FTokenizedMessage>& M : Results.Messages)
		{
			TSharedPtr<FJsonObject> MO = MakeShared<FJsonObject>();
			MO->SetStringField(TEXT("severity"),
				M->GetSeverity() == EMessageSeverity::Error   ? TEXT("error")   :
				M->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") :
				TEXT("info"));
			MO->SetStringField(TEXT("text"), M->ToText().ToString());
			MsgArr.Add(MakeShared<FJsonValueObject>(MO));
		}
		Out->SetArrayField(TEXT("messages"), MsgArr);

		if (bSave && bCompileOk)
		{
			UPackage* Pkg = BP->GetOutermost();
			if (Pkg)
			{
				Pkg->MarkPackageDirty();
				FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				const bool bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
				Out->SetBoolField(TEXT("saved"), bSaved);
			}
		}

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/compile: %s -> errors=%d warnings=%d saved=%s"),
			*BpPath, Results.NumErrors, Results.NumWarnings, bSave ? TEXT("yes") : TEXT("no"));

		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/create_variable
// Body: { blueprint, name, type, default_value?, category?, instance_editable?,
//         blueprint_read_only?, expose_on_spawn?, private?, compile?, save? }
// Adds a member variable to the Blueprint. Idempotent: if a var with the same
// name already exists, returns success with already_existed=true. Optionally
// compiles + saves.
// ============================================================================
bool FNGGHttpServer::HandleBpCreateVariable(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError)) { Callback(JsonError(400, ParseError)); return true; }

	FString BpPath, VarName, TypeStr, DefaultValue, Category;
	Body->TryGetStringField(TEXT("blueprint"),     BpPath);
	Body->TryGetStringField(TEXT("name"),          VarName);
	Body->TryGetStringField(TEXT("type"),          TypeStr);
	Body->TryGetStringField(TEXT("default_value"), DefaultValue);
	Body->TryGetStringField(TEXT("category"),      Category);
	bool bInstanceEditable = false, bReadOnly = false, bExposeOnSpawn = false, bPrivate = false;
	Body->TryGetBoolField(TEXT("instance_editable"),   bInstanceEditable);
	Body->TryGetBoolField(TEXT("blueprint_read_only"), bReadOnly);
	Body->TryGetBoolField(TEXT("expose_on_spawn"),     bExposeOnSpawn);
	Body->TryGetBoolField(TEXT("private"),             bPrivate);
	bool bCompile = true, bSave = true;
	Body->TryGetBoolField(TEXT("compile"), bCompile);
	Body->TryGetBoolField(TEXT("save"),    bSave);

	if (BpPath.IsEmpty() || VarName.IsEmpty() || TypeStr.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint, name, and type are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, VarName, TypeStr, DefaultValue, Category,
		bInstanceEditable, bReadOnly, bExposeOnSpawn, bPrivate, bCompile, bSave]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		// Build pin type from string
		FEdGraphPinType PinType;
		FString TypeErr;
		if (!ResolveVariablePinType(TypeStr, PinType, TypeErr))
		{
			Callback(JsonError(400, FString::Printf(TEXT("invalid type '%s': %s"), *TypeStr, *TypeErr)));
			return;
		}

		// Refuse if variable already exists (idempotent — return success)
		const FName VarFName(*VarName);
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			if (V.VarName == VarFName)
			{
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("success"), true);
				Out->SetBoolField(TEXT("already_existed"), true);
				Out->SetStringField(TEXT("name"), VarName);
				Callback(JsonOk(SerializeJson(Out)));
				return;
			}
		}

		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(BP, VarFName, PinType, DefaultValue);
		if (!bAdded)
		{
			Callback(JsonError(500, FString::Printf(TEXT("AddMemberVariable failed for '%s'"), *VarName)));
			return;
		}

		// Set flags + category on the new variable description
		for (FBPVariableDescription& V : BP->NewVariables)
		{
			if (V.VarName != VarFName) continue;
			if (!Category.IsEmpty()) V.Category = FText::FromString(Category);
			uint64 PF = V.PropertyFlags;
			if (bInstanceEditable)  PF &= ~CPF_DisableEditOnInstance; else PF |= CPF_DisableEditOnInstance;
			if (bReadOnly)          PF |= CPF_BlueprintReadOnly;       else PF &= ~CPF_BlueprintReadOnly;
			if (bExposeOnSpawn)     PF |= CPF_ExposeOnSpawn;           else PF &= ~CPF_ExposeOnSpawn;
			if (bPrivate)           PF |= CPF_DisableEditOnTemplate;   else PF &= ~CPF_DisableEditOnTemplate;
			V.PropertyFlags = PF;
			break;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("success"),         true);
		Out->SetBoolField(TEXT("already_existed"), false);
		Out->SetStringField(TEXT("name"),          VarName);
		Out->SetStringField(TEXT("type"),          TypeStr);

		// Compile + save
		if (bCompile)
		{
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			Out->SetBoolField(TEXT("compiled"),        Results.NumErrors == 0);
			Out->SetNumberField(TEXT("compile_errors"), Results.NumErrors);
		}
		bool bSaved = false;
		if (bSave)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
		UE_LOG(LogNGGBridge, Log, TEXT("/bp/create_variable: %s.%s (%s) added=%d saved=%d"),
			*BpPath, *VarName, *TypeStr, bAdded ? 1 : 0, bSaved ? 1 : 0);

		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}

// ============================================================================
// Handler: GET /bp/list_variables?blueprint=/Game/...
// Read-only inspection: returns all member variables on a Blueprint with their
// types, defaults, flags, and category. Also lists SCS components separately
// (they are accessible as variables in the graph) and reports the count of
// inherited BlueprintVisible properties on the parent class.
// Mirror image of /bp/create_variable — same files, same patterns.
// ============================================================================
bool FNGGHttpServer::HandleBpListVariables(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	const FString BpPath = GetQueryParam(Req, TEXT("blueprint"));
	if (BpPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint query param is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}

		// ---- Helper: reverse of ResolveVariablePinType — produce a friendly
		// type string from an FEdGraphPinType. Container modifiers wrap the
		// terminal type. -----------------------------------------------------
		auto TerminalTypeString = [](const FName& PinCategory, UObject* SubObj) -> FString
		{
			if (PinCategory == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
			if (PinCategory == UEdGraphSchema_K2::PC_Int)     return TEXT("int32");
			if (PinCategory == UEdGraphSchema_K2::PC_Int64)   return TEXT("int64");
			if (PinCategory == UEdGraphSchema_K2::PC_Real)    return TEXT("float");
			if (PinCategory == UEdGraphSchema_K2::PC_String)  return TEXT("FString");
			if (PinCategory == UEdGraphSchema_K2::PC_Name)    return TEXT("FName");
			if (PinCategory == UEdGraphSchema_K2::PC_Text)    return TEXT("FText");
			if (PinCategory == UEdGraphSchema_K2::PC_Byte)
			{
				// PC_Byte with a UEnum subcategory object is the canonical BP enum form.
				if (UEnum* E = Cast<UEnum>(SubObj))
				{
					return FString::Printf(TEXT("enum:%s"), *E->GetPathName());
				}
				return TEXT("byte");
			}
			if (PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				if (UUserDefinedStruct* UDS = Cast<UUserDefinedStruct>(SubObj))
				{
					return FString::Printf(TEXT("struct:%s"), *UDS->GetPathName());
				}
				if (SubObj) return TEXT("F") + SubObj->GetName();
				return TEXT("FStruct");
			}
			if (PinCategory == UEdGraphSchema_K2::PC_Object)
			{
				return SubObj ? SubObj->GetName() : TEXT("Object");
			}
			if (PinCategory == UEdGraphSchema_K2::PC_SoftObject)
			{
				return SubObj ? FString::Printf(TEXT("softobject:%s"), *SubObj->GetName()) : TEXT("softobject:Object");
			}
			if (PinCategory == UEdGraphSchema_K2::PC_Interface)
			{
				return SubObj ? FString::Printf(TEXT("interface:%s"), *SubObj->GetName()) : TEXT("interface:Object");
			}
			if (PinCategory == UEdGraphSchema_K2::PC_Class)
			{
				return SubObj ? (TEXT("class:") + SubObj->GetName()) : TEXT("class:Object");
			}
			if (PinCategory == UEdGraphSchema_K2::PC_SoftClass)
			{
				return SubObj ? FString::Printf(TEXT("softclass:%s"), *SubObj->GetName()) : TEXT("softclass:Object");
			}
			// Fallback: use the raw category name to keep round-tripping safe.
			return PinCategory.ToString();
		};

		auto FullTypeString = [&](const FEdGraphPinType& T) -> FString
		{
			const FString Inner = TerminalTypeString(T.PinCategory, T.PinSubCategoryObject.Get());
			switch (T.ContainerType)
			{
				case EPinContainerType::Array:
					return FString::Printf(TEXT("array<%s>"), *Inner);
				case EPinContainerType::Set:
					return FString::Printf(TEXT("set<%s>"),   *Inner);
				case EPinContainerType::Map:
				{
					const FString ValueInner = TerminalTypeString(
						T.PinValueType.TerminalCategory, T.PinValueType.TerminalSubCategoryObject.Get());
					return FString::Printf(TEXT("map<%s, %s>"), *Inner, *ValueInner);
				}
				default:
					return Inner;
			}
		};

		// ---- Build variables array ----------------------------------------
		TArray<TSharedPtr<FJsonValue>> Variables;
		UClass* GenClass = BP->GeneratedClass;
		UObject* CDO     = GenClass ? GenClass->GetDefaultObject() : nullptr;

		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			TSharedPtr<FJsonObject> VO = MakeShared<FJsonObject>();
			VO->SetStringField(TEXT("name"),     V.VarName.ToString());
			VO->SetStringField(TEXT("type"),     FullTypeString(V.VarType));
			// Full path of the variable's class/struct/enum, for dependency analysis
			// (object:/class: type strings above carry only the short name).
			if (UObject* SubObj = V.VarType.PinSubCategoryObject.Get())
			{
				VO->SetStringField(TEXT("type_object"), SubObj->GetPathName());
			}
			VO->SetStringField(TEXT("category"), V.Category.ToString());
			VO->SetStringField(TEXT("guid"),     V.VarGuid.ToString(EGuidFormats::DigitsWithHyphens));

			// Property flags — note CPF_DisableEditOnInstance is NEGATIVE.
			VO->SetBoolField(TEXT("instance_editable"),
				(V.PropertyFlags & CPF_DisableEditOnInstance) == 0);
			VO->SetBoolField(TEXT("blueprint_read_only"),
				(V.PropertyFlags & CPF_BlueprintReadOnly) != 0);
			VO->SetBoolField(TEXT("expose_on_spawn"),
				(V.PropertyFlags & CPF_ExposeOnSpawn) != 0);
			VO->SetBoolField(TEXT("private"),
				(V.PropertyFlags & CPF_DisableEditOnTemplate) != 0);

			// Container kind, useful to clients that don't want to parse the type string.
			switch (V.VarType.ContainerType)
			{
				case EPinContainerType::Array: VO->SetStringField(TEXT("container"), TEXT("array")); break;
				case EPinContainerType::Set:   VO->SetStringField(TEXT("container"), TEXT("set"));   break;
				case EPinContainerType::Map:   VO->SetStringField(TEXT("container"), TEXT("map"));   break;
				default: break;
			}

			// ---- Default value: prefer reading from the CDO, fall back to V.DefaultValue ----
			FString DefaultStr;
			FProperty* Prop = (GenClass) ? GenClass->FindPropertyByName(V.VarName) : nullptr;
			if (Prop && CDO)
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
				if (ValuePtr)
				{
					Prop->ExportTextItem_Direct(DefaultStr, ValuePtr, nullptr, nullptr, PPF_None);
				}

				// For object refs, also surface the pathname for clarity.
				if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
				{
					UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
					if (Obj)
					{
						VO->SetStringField(TEXT("default_object"), Obj->GetPathName());
					}
				}
			}
			if (DefaultStr.IsEmpty())
			{
				DefaultStr = V.DefaultValue;
			}
			VO->SetStringField(TEXT("default_value"), DefaultStr);

			Variables.Add(MakeShared<FJsonValueObject>(VO));
		}

		// ---- Components from the SCS (these are also accessible as variables) ----
		TArray<TSharedPtr<FJsonValue>> Components;
		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node) continue;
				TSharedPtr<FJsonObject> CO = MakeShared<FJsonObject>();
				CO->SetStringField(TEXT("name"),  Node->GetVariableName().ToString());
				CO->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
				Components.Add(MakeShared<FJsonValueObject>(CO));
			}
		}

		// ---- Inherited count (BlueprintVisible properties on the parent) ----
		int32 InheritedCount = 0;
		if (BP->ParentClass)
		{
			for (TFieldIterator<FProperty> It(BP->ParentClass); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_BlueprintVisible))
				{
					++InheritedCount;
				}
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetStringField(TEXT("blueprint"),       BpPath);
		Out->SetStringField(TEXT("parent_class"),    BP->ParentClass ? BP->ParentClass->GetPathName() : FString());
		Out->SetArrayField (TEXT("variables"),       Variables);
		Out->SetArrayField (TEXT("components"),      Components);
		Out->SetNumberField(TEXT("inherited_count"), InheritedCount);

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/list_variables: %s -> %d vars, %d components, %d inherited"),
			*BpPath, Variables.Num(), Components.Num(), InheritedCount);

		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/create_function
// Body: { blueprint, name, inputs:[{name,type}], outputs:[{name,type}],
//         is_pure?, is_const?, category?, compile?, save? }
// Adds a new function graph to the Blueprint with user-defined input/output
// pins on the auto-created entry/result terminator nodes.
// ============================================================================
bool FNGGHttpServer::HandleBpCreateFunction(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError)) { Callback(JsonError(400, ParseError)); return true; }

	FString BpPath, FunctionName, Category;
	Body->TryGetStringField(TEXT("blueprint"), BpPath);
	Body->TryGetStringField(TEXT("name"),      FunctionName);
	Body->TryGetStringField(TEXT("category"),  Category);
	bool bIsPure = false, bIsConst = false;
	Body->TryGetBoolField(TEXT("is_pure"),  bIsPure);
	Body->TryGetBoolField(TEXT("is_const"), bIsConst);
	bool bCompile = true, bSave = true;
	Body->TryGetBoolField(TEXT("compile"), bCompile);
	Body->TryGetBoolField(TEXT("save"),    bSave);

	if (BpPath.IsEmpty() || FunctionName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and name are required")));
		return true;
	}

	struct FPinSpec
	{
		FString Name;
		FString TypeStr;
	};
	auto ParsePins = [](const TArray<TSharedPtr<FJsonValue>>* Arr, TArray<FPinSpec>& Out)
	{
		if (!Arr) return;
		for (const auto& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) continue;
			FPinSpec P;
			O->TryGetStringField(TEXT("name"), P.Name);
			O->TryGetStringField(TEXT("type"), P.TypeStr);
			if (!P.Name.IsEmpty() && !P.TypeStr.IsEmpty()) Out.Add(MoveTemp(P));
		}
	};
	TArray<FPinSpec> Inputs, Outputs;
	const TArray<TSharedPtr<FJsonValue>>* InsArr  = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* OutsArr = nullptr;
	Body->TryGetArrayField(TEXT("inputs"),  InsArr);
	Body->TryGetArrayField(TEXT("outputs"), OutsArr);
	ParsePins(InsArr,  Inputs);
	ParsePins(OutsArr, Outputs);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BpPath, FunctionName, Category, bIsPure, bIsConst, bCompile, bSave, Inputs, Outputs]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}

		// Refuse if a graph with this name already exists.
		const FName FnName(*FunctionName);
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetFName() == FnName)
			{
				Callback(JsonError(409, FString::Printf(
					TEXT("function '%s' already exists on '%s'"), *FunctionName, *BpPath)));
				return;
			}
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FnName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph)
		{
			Callback(JsonError(500, TEXT("CreateNewGraph returned null")));
			return;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated*/true, /*Sig*/nullptr);

		// Find the entry node (auto-created).
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
			Callback(JsonError(500, TEXT("function entry node was not created")));
			return;
		}

		int32 ExtraFlags = 0;
		if (bIsPure)  ExtraFlags |= FUNC_BlueprintPure;
		if (bIsConst) ExtraFlags |= FUNC_Const;
		if (ExtraFlags) EntryNode->AddExtraFlags(ExtraFlags);
		if (!Category.IsEmpty())
		{
			EntryNode->MetaData.Category = FText::FromString(Category);
		}

		int32 InputsAdded = 0;
		for (const FPinSpec& In : Inputs)
		{
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!ResolveVariablePinType(In.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/bp/create_function: skipping input '%s' — invalid type '%s' (%s)"),
					*In.Name, *In.TypeStr, *TypeErr);
				continue;
			}
			if (EntryNode->CreateUserDefinedPin(FName(*In.Name), PinType, EGPD_Output, /*bUseUniqueName*/false))
			{
				++InputsAdded;
			}
		}

		// Locate or create the function result node for outputs.
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
			if (!ResultNode)
			{
				Callback(JsonError(500, TEXT("failed to allocate function result node")));
				return;
			}
			ResultNode->CreateNewGuid();
			ResultNode->PostPlacedNewNode();
			ResultNode->AllocateDefaultPins();
			NewGraph->AddNode(ResultNode, /*bUserAction*/false, /*bSelectNewNode*/false);
		}

		int32 OutputsAdded = 0;
		for (const FPinSpec& Out : Outputs)
		{
			if (!ResultNode) break;
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!ResolveVariablePinType(Out.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/bp/create_function: skipping output '%s' — invalid type '%s' (%s)"),
					*Out.Name, *Out.TypeStr, *TypeErr);
				continue;
			}
			if (ResultNode->CreateUserDefinedPin(FName(*Out.Name), PinType, EGPD_Input, /*bUseUniqueName*/false))
			{
				++OutputsAdded;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField   (TEXT("success"),        true);
		Out->SetStringField (TEXT("function_name"),  FunctionName);
		Out->SetStringField (TEXT("graph_name"),     NewGraph->GetName());
		Out->SetNumberField (TEXT("inputs_added"),   InputsAdded);
		Out->SetNumberField (TEXT("outputs_added"),  OutputsAdded);

		if (bCompile)
		{
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			Out->SetBoolField  (TEXT("compiled"),        Results.NumErrors == 0);
			Out->SetNumberField(TEXT("compile_errors"),  Results.NumErrors);
		}
		bool bSaved = false;
		if (bSave)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
		}
		Out->SetBoolField(TEXT("saved"), bSaved);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bp/create_function: %s::%s ins=%d outs=%d pure=%d const=%d saved=%d"),
			*BpPath, *FunctionName, InputsAdded, OutputsAdded,
			bIsPure ? 1 : 0, bIsConst ? 1 : 0, bSaved ? 1 : 0);

		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}

// ============================================================================
// Handler: POST /bp/heal_world_context
// Body: { blueprint?: "/Game/...", blueprints?: ["/Game/...", ...],
//         compile?: bool=true, save?: bool=true, dry_run?: bool=false }
// Repairs the UE5 migration corruption where a static Blueprint function's
// signature holds an explicit user-defined "__WorldContext" parameter. That
// name is reserved for the hidden auto world-context pin the engine adds to
// every static function graph; having it as a real user pin makes
// UK2Node_FunctionEntry::AllocateDefaultPins try to create the pin twice, which
// trips an ensure ("World context parameter pin already exists...") — harmless
// in-editor but fatal to a cook (run under -CrashForUAT). For each named
// Blueprint we scan every function graph's entry node and remove any
// user-defined pin literally named "__WorldContext", then compile + save.
// ============================================================================
bool FNGGHttpServer::HandleBpHealWorldContext(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError)) { Callback(JsonError(400, ParseError)); return true; }

	TArray<FString> BpPaths;
	FString SinglePath;
	if (Body->TryGetStringField(TEXT("blueprint"), SinglePath) && !SinglePath.IsEmpty())
	{
		BpPaths.Add(SinglePath);
	}
	const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
	if (Body->TryGetArrayField(TEXT("blueprints"), PathsArr) && PathsArr)
	{
		for (const auto& V : *PathsArr)
		{
			FString P = V->AsString();
			if (!P.IsEmpty()) { BpPaths.Add(MoveTemp(P)); }
		}
	}
	if (BpPaths.Num() == 0)
	{
		Callback(JsonError(400, TEXT("provide 'blueprint' (string) or 'blueprints' (array)")));
		return true;
	}

	bool bCompile = true, bSave = true, bDryRun = false;
	Body->TryGetBoolField(TEXT("compile"),  bCompile);
	Body->TryGetBoolField(TEXT("save"),     bSave);
	Body->TryGetBoolField(TEXT("dry_run"),  bDryRun);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BpPaths, bCompile, bSave, bDryRun]()
	{
		static const FName WorldContextName(TEXT("__WorldContext"));

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 GrandTotalRemoved = 0;

		for (const FString& BpPath : BpPaths)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("blueprint"), BpPath);

			UBlueprint* BP = LoadBlueprintFlexible(BpPath);
			if (!BP)
			{
				Entry->SetBoolField  (TEXT("success"), false);
				Entry->SetStringField(TEXT("error"), TEXT("blueprint not found"));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			TArray<TSharedPtr<FJsonValue>> HealedFns;
			TArray<TSharedPtr<FJsonValue>> Diag;
			int32 PinsRemoved = 0;

			auto FuncHasWorldContextParam = [](UFunction* Fn) -> bool
			{
				if (!Fn) { return false; }
				for (TFieldIterator<FProperty> It(Fn); It && (It->PropertyFlags & CPF_Parm); ++It)
				{
					if (It->GetFName() == WorldContextName) { return true; }
				}
				return false;
			};

			auto WcParamFlags = [](UFunction* Fn) -> uint64
			{
				if (!Fn) { return 0; }
				for (TFieldIterator<FProperty> It(Fn); It && (It->PropertyFlags & CPF_Parm); ++It)
				{
					if (It->GetFName() == WorldContextName) { return (uint64)It->PropertyFlags; }
				}
				return 0;
			};

			static const FName WorldContextMetaKey(TEXT("WorldContext"));

			for (UEdGraph* G : BP->FunctionGraphs)
			{
				if (!G) { continue; }

				UK2Node_FunctionEntry* EntryNode = nullptr;
				for (UEdGraphNode* N : G->Nodes)
				{
					if (UK2Node_FunctionEntry* E = Cast<UK2Node_FunctionEntry>(N))
					{
						EntryNode = E;
						break;
					}
				}
				if (!EntryNode) { continue; }

				// Entry-node pin named __WorldContext (auto/hidden or promoted), if any.
				UEdGraphPin* WcPin = EntryNode->FindPin(WorldContextName, EGPD_Output);

				bool bUserDefHasWc = false;
				for (const TSharedPtr<FUserPinInfo>& P : EntryNode->UserDefinedPins)
				{
					if (P.IsValid() && P->PinName == WorldContextName) { bUserDefHasWc = true; break; }
				}

				UFunction* SkelFn = BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass->FindFunctionByName(G->GetFName()) : nullptr;
				UFunction* GenFn  = BP->GeneratedClass         ? BP->GeneratedClass->FindFunctionByName(G->GetFName())         : nullptr;
				const bool bSkelParam = FuncHasWorldContextParam(SkelFn);
				const bool bGenParam  = FuncHasWorldContextParam(GenFn);

				// Only report functions that touch __WorldContext in some way.
				if (WcPin || bUserDefHasWc || bSkelParam || bGenParam)
				{
					TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
					D->SetStringField(TEXT("fn"), G->GetName());
					D->SetBoolField  (TEXT("entry_pin"),        WcPin != nullptr);
					D->SetBoolField  (TEXT("entry_pin_hidden"), WcPin ? WcPin->bHidden : false);
					D->SetBoolField  (TEXT("userdef_param"),    bUserDefHasWc);
					D->SetBoolField  (TEXT("skel_param"),       bSkelParam);
					D->SetBoolField  (TEXT("gen_param"),        bGenParam);
					D->SetStringField(TEXT("func_ref"),   EntryNode->FunctionReference.GetMemberName().ToString());
					D->SetStringField(TEXT("custom_name"), EntryNode->CustomGeneratedFunctionName.ToString());
					D->SetStringField(TEXT("gen_wc_flags"), FString::Printf(TEXT("0x%llX"), WcParamFlags(GenFn)));
					D->SetStringField(TEXT("skel_wc_flags"), FString::Printf(TEXT("0x%llX"), WcParamFlags(SkelFn)));
#if WITH_EDITORONLY_DATA
					D->SetBoolField  (TEXT("gen_has_wc_meta"), GenFn ? GenFn->HasMetaData(WorldContextMetaKey) : false);
					D->SetStringField(TEXT("gen_wc_meta"), GenFn && GenFn->HasMetaData(WorldContextMetaKey) ? GenFn->GetMetaData(WorldContextMetaKey) : TEXT(""));
					D->SetBoolField  (TEXT("entry_has_wc_meta"), EntryNode->MetaData.HasMetaData(WorldContextMetaKey));
#endif
					UClass* NodeClass = EntryNode->GetBlueprintClassFromNode();
					UFunction* Resolved = EntryNode->FunctionReference.ResolveMember<UFunction>(NodeClass);
					D->SetStringField(TEXT("node_class"), NodeClass ? NodeClass->GetName() : TEXT("null"));
					D->SetBoolField  (TEXT("resolved_nonnull"), Resolved != nullptr);
					D->SetBoolField  (TEXT("resolved_has_wc"), FuncHasWorldContextParam(Resolved));
					D->SetBoolField  (TEXT("fref_self"), EntryNode->FunctionReference.IsSelfContext());
					{
						UClass* ParentCls = EntryNode->FunctionReference.GetMemberParentClass();
						D->SetStringField(TEXT("fref_parent"), ParentCls ? ParentCls->GetPathName() : TEXT("null"));
					}
					D->SetBoolField  (TEXT("fref_guid_valid"), EntryNode->FunctionReference.GetMemberGuid().IsValid());
					Diag.Add(MakeShared<FJsonValueObject>(D));
				}

				// The fix. Root cause: a healthy locally-defined static function has
				// an UNRESOLVABLE entry FunctionReference (bSelfContext=false, no
				// parent, invalid guid) — its name comes from GetMemberName(), so
				// AllocateDefaultPins' path (A) (CreatePinsForFunctionEntryExit) is
				// skipped and only path (C) runs, creating a HIDDEN auto
				// __WorldContext pin. The corruption is that the entry
				// FunctionReference resolves (bSelfContext=true + valid guid, as if
				// it were an override), so path (A) rebuilds __WorldContext from the
				// resolved signature as a VISIBLE pin; path (C)'s ensure then trips
				// ("pin already exists") on reconstruct — fatal under the cook's
				// -CrashForUAT. Detect via a visible __WorldContext pin backed by a
				// resolvable self-reference, and reset the reference to the healthy
				// unresolvable state (name preserved). On the next reconstruct the
				// pin is regenerated hidden and the ensure never fires.
				const bool bResolves = EntryNode->FunctionReference.ResolveMember<UFunction>(EntryNode->GetBlueprintClassFromNode()) != nullptr;
				if (!bDryRun && WcPin && !WcPin->bHidden && bResolves)
				{
					const FName FnName = EntryNode->FunctionReference.GetMemberName();
					EntryNode->Modify();
					EntryNode->FunctionReference.SetDirect(FnName, FGuid(), nullptr, /*bIsConsideredSelfContext*/ false);
					HealedFns.Add(MakeShared<FJsonValueString>(G->GetName()));
					++PinsRemoved;
				}
			}

			Entry->SetBoolField (TEXT("success"), true);
			Entry->SetNumberField(TEXT("pins_fixed"), PinsRemoved);
			Entry->SetArrayField (TEXT("functions_healed"), HealedFns);
			Entry->SetArrayField (TEXT("diagnostics"), Diag);
			GrandTotalRemoved += PinsRemoved;

			if (PinsRemoved > 0 && !bDryRun)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

				if (bCompile)
				{
					FCompilerResultsLog CompileResults;
					CompileResults.bSilentMode = true;
					FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &CompileResults);
					Entry->SetBoolField  (TEXT("compiled"),       CompileResults.NumErrors == 0);
					Entry->SetNumberField(TEXT("compile_errors"), CompileResults.NumErrors);
				}

				bool bSaved = false;
				if (bSave)
				{
					if (UPackage* Pkg = BP->GetOutermost())
					{
						Pkg->MarkPackageDirty();
						const FString FileName = FPackageName::LongPackageNameToFilename(
							Pkg->GetName(), FPackageName::GetAssetPackageExtension());
						FSavePackageArgs Args;
						Args.TopLevelFlags = RF_Public | RF_Standalone;
						Args.SaveFlags     = SAVE_NoError;
						Args.Error         = GError;
						bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
					}
				}
				Entry->SetBoolField(TEXT("saved"), bSaved);
			}

			UE_LOG(LogNGGBridge, Log,
				TEXT("/bp/heal_world_context: %s pins_fixed=%d dry_run=%d"),
				*BpPath, PinsRemoved, bDryRun ? 1 : 0);

			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),            true);
		Out->SetBoolField  (TEXT("dry_run"),            bDryRun);
		Out->SetNumberField(TEXT("total_pins_fixed"), GrandTotalRemoved);
		Out->SetArrayField (TEXT("results"),            Results);
		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}

// ============================================================================
// Handler: POST /bp/create_macro
// Body: { blueprint, name, inputs:[{name,type}], outputs:[{name,type}],
//         category?, compile?, save? }
// Adds a new macro graph to the Blueprint. Macros use UK2Node_Tunnel for both
// entry and exit; entry has bCanHaveOutputs=true / bCanHaveInputs=false (and
// vice versa for exit). We disambiguate by those bools rather than node order.
// ============================================================================
bool FNGGHttpServer::HandleBpCreateMacro(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError)) { Callback(JsonError(400, ParseError)); return true; }

	FString BpPath, MacroName, Category;
	Body->TryGetStringField(TEXT("blueprint"), BpPath);
	Body->TryGetStringField(TEXT("name"),      MacroName);
	Body->TryGetStringField(TEXT("category"),  Category);
	bool bCompile = true, bSave = true;
	Body->TryGetBoolField(TEXT("compile"), bCompile);
	Body->TryGetBoolField(TEXT("save"),    bSave);

	if (BpPath.IsEmpty() || MacroName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and name are required")));
		return true;
	}

	struct FPinSpec
	{
		FString Name;
		FString TypeStr;
	};
	auto ParsePins = [](const TArray<TSharedPtr<FJsonValue>>* Arr, TArray<FPinSpec>& Out)
	{
		if (!Arr) return;
		for (const auto& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) continue;
			FPinSpec P;
			O->TryGetStringField(TEXT("name"), P.Name);
			O->TryGetStringField(TEXT("type"), P.TypeStr);
			if (!P.Name.IsEmpty() && !P.TypeStr.IsEmpty()) Out.Add(MoveTemp(P));
		}
	};
	TArray<FPinSpec> Inputs, Outputs;
	const TArray<TSharedPtr<FJsonValue>>* InsArr  = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* OutsArr = nullptr;
	Body->TryGetArrayField(TEXT("inputs"),  InsArr);
	Body->TryGetArrayField(TEXT("outputs"), OutsArr);
	ParsePins(InsArr,  Inputs);
	ParsePins(OutsArr, Outputs);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BpPath, MacroName, Category, bCompile, bSave, Inputs, Outputs]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}

		const FName MName(*MacroName);
		for (UEdGraph* G : BP->MacroGraphs)
		{
			if (G && G->GetFName() == MName)
			{
				Callback(JsonError(409, FString::Printf(
					TEXT("macro '%s' already exists on '%s'"), *MacroName, *BpPath)));
				return;
			}
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, MName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph)
		{
			Callback(JsonError(500, TEXT("CreateNewGraph returned null")));
			return;
		}
		FBlueprintEditorUtils::AddMacroGraph(BP, NewGraph, /*bIsUserCreated*/true, /*Sig*/nullptr);

		// Disambiguate entry/exit by tunnel direction flags rather than order:
		//   entry: bCanHaveOutputs == true  (data flows OUT of the entry into the body)
		//   exit:  bCanHaveInputs  == true  (data flows INTO the exit from the body)
		UK2Node_Tunnel* EntryTunnel = nullptr;
		UK2Node_Tunnel* ExitTunnel  = nullptr;
		for (UEdGraphNode* N : NewGraph->Nodes)
		{
			if (UK2Node_Tunnel* T = Cast<UK2Node_Tunnel>(N))
			{
				if (T->bCanHaveOutputs && !T->bCanHaveInputs)      EntryTunnel = T;
				else if (T->bCanHaveInputs && !T->bCanHaveOutputs) ExitTunnel  = T;
			}
		}
		if (!EntryTunnel || !ExitTunnel)
		{
			Callback(JsonError(500, TEXT("macro entry/exit tunnel nodes not created")));
			return;
		}

		if (!Category.IsEmpty())
		{
			EntryTunnel->MetaData.Category = FText::FromString(Category);
		}

		int32 InputsAdded = 0;
		for (const FPinSpec& In : Inputs)
		{
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!ResolveVariablePinType(In.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/bp/create_macro: skipping input '%s' — invalid type '%s' (%s)"),
					*In.Name, *In.TypeStr, *TypeErr);
				continue;
			}
			// Macro inputs go on the entry tunnel as Output pins.
			if (EntryTunnel->CreateUserDefinedPin(FName(*In.Name), PinType, EGPD_Output, /*bUseUniqueName*/false))
			{
				++InputsAdded;
			}
		}

		int32 OutputsAdded = 0;
		for (const FPinSpec& Out : Outputs)
		{
			FEdGraphPinType PinType;
			FString TypeErr;
			if (!ResolveVariablePinType(Out.TypeStr, PinType, TypeErr))
			{
				UE_LOG(LogNGGBridge, Warning,
					TEXT("/bp/create_macro: skipping output '%s' — invalid type '%s' (%s)"),
					*Out.Name, *Out.TypeStr, *TypeErr);
				continue;
			}
			// Macro outputs go on the exit tunnel as Input pins.
			if (ExitTunnel->CreateUserDefinedPin(FName(*Out.Name), PinType, EGPD_Input, /*bUseUniqueName*/false))
			{
				++OutputsAdded;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> OutJson = MakeShared<FJsonObject>();
		OutJson->SetBoolField   (TEXT("success"),       true);
		OutJson->SetStringField (TEXT("macro_name"),    MacroName);
		OutJson->SetStringField (TEXT("graph_name"),    NewGraph->GetName());
		OutJson->SetNumberField (TEXT("inputs_added"),  InputsAdded);
		OutJson->SetNumberField (TEXT("outputs_added"), OutputsAdded);

		if (bCompile)
		{
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			OutJson->SetBoolField  (TEXT("compiled"),        Results.NumErrors == 0);
			OutJson->SetNumberField(TEXT("compile_errors"),  Results.NumErrors);
		}
		bool bSaved = false;
		if (bSave)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
		}
		OutJson->SetBoolField(TEXT("saved"), bSaved);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bp/create_macro: %s::%s ins=%d outs=%d saved=%d"),
			*BpPath, *MacroName, InputsAdded, OutputsAdded, bSaved ? 1 : 0);

		Callback(JsonOk(SerializeJson(OutJson)));
	});
	return true;
}

// ============================================================================
// Handler: POST /bp/delete_function
// Body: { blueprint, name, compile?, save? }
// Removes a user-created function graph from the Blueprint (the inverse of
// /bp/create_function). Idempotent: if no function with that name exists,
// returns success with already_absent=true. Use this to clear a BP function
// whose name now collides with an inherited C++ function after a reparent.
// ============================================================================
bool FNGGHttpServer::HandleBpDeleteFunction(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError)) { Callback(JsonError(400, ParseError)); return true; }

	FString BpPath, FunctionName;
	Body->TryGetStringField(TEXT("blueprint"), BpPath);
	Body->TryGetStringField(TEXT("name"),      FunctionName);
	bool bCompile = true, bSave = true;
	Body->TryGetBoolField(TEXT("compile"), bCompile);
	Body->TryGetBoolField(TEXT("save"),    bSave);

	if (BpPath.IsEmpty() || FunctionName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and name are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, FunctionName, bCompile, bSave]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		const FName FnName(*FunctionName);
		UEdGraph* Target = nullptr;
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetFName() == FnName) { Target = G; break; }
		}

		if (!Target)
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"),        true);
			Out->SetBoolField(TEXT("already_absent"), true);
			Out->SetStringField(TEXT("name"),         FunctionName);
			Callback(JsonOk(SerializeJson(Out)));
			return;
		}

		FBlueprintEditorUtils::RemoveGraph(BP, Target, EGraphRemoveFlags::None);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("already_absent"), false);
		Out->SetStringField(TEXT("name"),         FunctionName);

		bool bCompileOk = true;
		if (bCompile)
		{
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			bCompileOk = Results.NumErrors == 0;
			Out->SetBoolField(TEXT("compiled"),         bCompileOk);
			Out->SetNumberField(TEXT("compile_errors"), Results.NumErrors);
		}
		bool bSaved = false;
		if (bSave)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
		// The delete itself succeeded, but if the caller asked us to compile and
		// the removal left the Blueprint non-compiling — or a requested save
		// failed — that is not a clean success: surface it rather than reporting
		// a phantom OK.
		Out->SetBoolField(TEXT("success"), bCompileOk && (!bSave || bSaved));
		UE_LOG(LogNGGBridge, Log, TEXT("/bp/delete_function: %s::%s removed compiled=%d saved=%d"),
			*BpPath, *FunctionName, bCompileOk ? 1 : 0, bSaved ? 1 : 0);

		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}

// ============================================================================
// Handler: POST /bp/delete_variable
// Body: { blueprint, name, compile?, save? }
// Removes a member variable from the Blueprint (the inverse of
// /bp/create_variable). Idempotent: if no variable with that name exists,
// returns success with already_absent=true. Use this to clear a BP variable
// whose name now collides with an inherited C++ UPROPERTY after a reparent.
// ============================================================================
bool FNGGHttpServer::HandleBpDeleteVariable(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body;
	FString ParseError;
	if (!ParseJsonBody(Req, Body, ParseError)) { Callback(JsonError(400, ParseError)); return true; }

	FString BpPath, VarName;
	Body->TryGetStringField(TEXT("blueprint"), BpPath);
	Body->TryGetStringField(TEXT("name"),      VarName);
	bool bCompile = true, bSave = true;
	Body->TryGetBoolField(TEXT("compile"), bCompile);
	Body->TryGetBoolField(TEXT("save"),    bSave);

	if (BpPath.IsEmpty() || VarName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and name are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, VarName, bCompile, bSave]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		const FName VarFName(*VarName);
		bool bExisted = false;
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			if (V.VarName == VarFName) { bExisted = true; break; }
		}

		if (!bExisted)
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("success"),        true);
			Out->SetBoolField(TEXT("already_absent"), true);
			Out->SetStringField(TEXT("name"),         VarName);
			Callback(JsonOk(SerializeJson(Out)));
			return;
		}

		BP->Modify();
		// Clean up any get/set nodes that reference the variable first, otherwise
		// removing the member leaves dangling variable nodes that fail to compile
		// (and we'd then save a broken BP as success).
		FBlueprintEditorUtils::RemoveVariableNodes(BP, VarFName);
		FBlueprintEditorUtils::RemoveMemberVariable(BP, VarFName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("already_absent"), false);
		Out->SetStringField(TEXT("name"),         VarName);

		bool bCompileOk = true;
		if (bCompile)
		{
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			bCompileOk = (Results.NumErrors == 0);
			Out->SetBoolField(TEXT("compiled"),         bCompileOk);
			Out->SetNumberField(TEXT("compile_errors"), Results.NumErrors);
		}
		// Don't save a BP that no longer compiles (e.g. a referencing node we
		// couldn't clean up); a failed compile must not be reported as success.
		bool bSaved = false;
		if (bSave && bCompileOk)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
		}
		Out->SetBoolField(TEXT("success"), bCompileOk);
		Out->SetBoolField(TEXT("saved"), bSaved);
		UE_LOG(LogNGGBridge, Log, TEXT("/bp/delete_variable: %s.%s removed saved=%d"),
			*BpPath, *VarName, bSaved ? 1 : 0);

		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}

// ============================================================================
// Handler: POST /bp/add_logic  (ergonomic wrapper over add_node + connect_pins)
// ============================================================================

bool FNGGHttpServer::HandleBpAddLogic(
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

	FString BpPath; Body->TryGetStringField(TEXT("blueprint"), BpPath);
	FString GraphName = TEXT("EventGraph");
	Body->TryGetStringField(TEXT("graph"), GraphName);
	bool bAutoLayout = true;
	Body->TryGetBoolField(TEXT("auto_layout"), bAutoLayout);
	bool bCompile = false;
	// compile AND save default to TRUE — BP graph edits without compile+save
	// leave the asset in an inconsistent state on disk. Callers can opt out
	// explicitly if they want to batch multiple edits before finalizing.
	bCompile = true;
	bool bSave = true;
	Body->TryGetBoolField(TEXT("compile"), bCompile);
	Body->TryGetBoolField(TEXT("save"),    bSave);

	const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
	Body->TryGetArrayField(TEXT("nodes"), NodesArr);
	const TArray<TSharedPtr<FJsonValue>>* ConnsArr = nullptr;
	Body->TryGetArrayField(TEXT("connections"), ConnsArr);

	if (BpPath.IsEmpty() || !NodesArr)
	{
		Callback(JsonError(400, TEXT("blueprint and nodes[] are required")));
		return true;
	}

	// Copy arrays (capture by value into async lambda)
	TArray<TSharedPtr<FJsonValue>> NodesCopy = *NodesArr;
	TArray<TSharedPtr<FJsonValue>> ConnsCopy = ConnsArr ? *ConnsArr : TArray<TSharedPtr<FJsonValue>>();

	TWeakPtr<FNGGHttpServer> WeakSelf = AsWeak();
	AsyncTask(ENamedThreads::GameThread,
		[WeakSelf, Callback, BpPath, GraphName, bAutoLayout, bCompile, bSave, NodesCopy, ConnsCopy]()
	{
		// Bail if the server was destroyed before this deferred task ran.
		TSharedPtr<FNGGHttpServer> Self = WeakSelf.Pin();
		if (!Self.IsValid()) { return; }

		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}
		UEdGraph* Graph = FindBlueprintGraph(BP, GraphName);
		if (!Graph)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Graph '%s' not found"), *GraphName)));
			return;
		}

		// Resolve ergonomic type -> UK2Node UClass + per-type config object
		auto ResolveErgoType = [](const FString& TypeStr) -> UClass*
		{
			const FString T = TypeStr.ToLower();
			if (T == TEXT("event"))            return UK2Node_Event::StaticClass();
			if (T == TEXT("component_event"))  return UK2Node_ComponentBoundEvent::StaticClass();
			if (T == TEXT("call_function"))    return UK2Node_CallFunction::StaticClass();
			if (T == TEXT("variable_get"))     return UK2Node_VariableGet::StaticClass();
			if (T == TEXT("variable_set"))     return UK2Node_VariableSet::StaticClass();
			if (T == TEXT("branch"))           return UK2Node_IfThenElse::StaticClass();
			if (T == TEXT("cast"))             return UK2Node_DynamicCast::StaticClass();
			if (T == TEXT("sequence"))         return UK2Node_ExecutionSequence::StaticClass();
			if (T == TEXT("self"))             return UK2Node_Self::StaticClass();
			if (T == TEXT("knot"))             return UK2Node_Knot::StaticClass();
			if (T == TEXT("macro"))            return UK2Node_MacroInstance::StaticClass();
			if (T == TEXT("custom_event"))     return UK2Node_CustomEvent::StaticClass();
			return nullptr;
		};

		// Build a synthetic config JSON from the ergonomic field set on each node spec
		auto BuildConfig = [](const TSharedPtr<FJsonObject>& NodeObj) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Cfg = MakeShared<FJsonObject>();
			// Pass-through fields that ConfigureNodePreAllocate understands
			FString S;
			bool B;
			if (NodeObj->TryGetStringField(TEXT("function"), S))   Cfg->SetStringField(TEXT("function"), S);
			if (NodeObj->TryGetStringField(TEXT("class"), S))      Cfg->SetStringField(TEXT("class"), S);
			if (NodeObj->TryGetBoolField  (TEXT("self_context"), B)) Cfg->SetBoolField (TEXT("self_context"), B);
			if (NodeObj->TryGetStringField(TEXT("event"), S))      Cfg->SetStringField(TEXT("event_name"), S);
			if (NodeObj->TryGetStringField(TEXT("event_name"), S)) Cfg->SetStringField(TEXT("event_name"), S);
			// `name` is the ergonomic shorthand for custom_event/event/variable specs.
			// Pass it through so ConfigureNodePreAllocate can fall back to it.
			if (NodeObj->TryGetStringField(TEXT("name"), S))       Cfg->SetStringField(TEXT("name"), S);
			if (NodeObj->TryGetBoolField  (TEXT("override"), B))   Cfg->SetBoolField  (TEXT("override"), B);
			if (NodeObj->TryGetStringField(TEXT("component"), S))  Cfg->SetStringField(TEXT("component"), S);
			if (NodeObj->TryGetStringField(TEXT("delegate"), S))   Cfg->SetStringField(TEXT("delegate"), S);
			if (NodeObj->TryGetStringField(TEXT("variable"), S))   Cfg->SetStringField(TEXT("variable"), S);
			if (NodeObj->TryGetStringField(TEXT("target_class"), S)) Cfg->SetStringField(TEXT("target_class"), S);
			if (NodeObj->TryGetStringField(TEXT("macro"), S))      Cfg->SetStringField(TEXT("macro"), S);
			// Nested object fallthroughs
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("function"), ObjPtr) && ObjPtr) Cfg->SetObjectField(TEXT("function"), *ObjPtr);
			return Cfg;
		};

		TMap<FString, FGuid> LocalIdToGuid;  // request-scope (duplicated into BpNodeIdMap below)
		TArray<UK2Node*> CreatedNodes;
		TSharedRef<FJsonObject> NodeIdsOut = MakeShared<FJsonObject>();
		int32 NumConnectionsMade = 0;

		// ---- 0. Pre-pass: auto-create missing self-context variables --------
		// If a variable_get/variable_set node references a name that doesn't
		// exist on the BP (NewVariables, SCS-derived components, parent class
		// properties), try to infer its FEdGraphPinType from the connections it
		// participates in. If inference succeeds, add a member variable so the
		// node configuration + compile stage can bind it. Best-effort only —
		// failures are silent here; the existing error path will surface them.
		TArray<FString> AutoCreatedVars;
		{
			// Helper: look up a node spec by id in NodesCopy.
			auto FindNodeSpec = [&](const FString& Id) -> TSharedPtr<FJsonObject>
			{
				for (const TSharedPtr<FJsonValue>& V : NodesCopy)
				{
					if (!V.IsValid()) continue;
					TSharedPtr<FJsonObject> O = V->AsObject();
					if (!O.IsValid()) continue;
					FString OId;
					O->TryGetStringField(TEXT("id"), OId);
					if (OId == Id) return O;
				}
				return nullptr;
			};

			// Helper: is `Name` a valid identifier (non-empty, not starting with digit)?
			auto IsValidIdent = [](const FString& Name) -> bool
			{
				if (Name.IsEmpty()) return false;
				const TCHAR C = Name[0];
				if (C >= TEXT('0') && C <= TEXT('9')) return false;
				return true;
			};

			// Helper: does the var already exist somewhere we can resolve it?
			auto VarAlreadyResolves = [&](const FName& VarFName) -> bool
			{
				for (const FBPVariableDescription& V : BP->NewVariables)
				{
					if (V.VarName == VarFName) return true;
				}
				if (BP->SkeletonGeneratedClass &&
					BP->SkeletonGeneratedClass->FindPropertyByName(VarFName))
				{
					return true;
				}
				if (BP->GeneratedClass &&
					BP->GeneratedClass->FindPropertyByName(VarFName))
				{
					return true;
				}
				return false;
			};

			// Helper: build pin type from a UProperty (best-effort; falls back to false on failure)
			auto PinTypeFromProperty = [](FProperty* Prop, FEdGraphPinType& Out) -> bool
			{
				if (!Prop) return false;
				const UEdGraphSchema_K2* Sch = GetDefault<UEdGraphSchema_K2>();
				return Sch && Sch->ConvertPropertyToPinType(Prop, Out);
			};

			// Helper: try to infer the pin type for variable named VarName by
			// scanning connections referencing NodeId.
			auto InferTypeFromConnections = [&](const FString& VarNodeId, const FString& VarName, FEdGraphPinType& Out) -> bool
			{
				for (const TSharedPtr<FJsonValue>& V : ConnsCopy)
				{
					if (!V.IsValid()) continue;
					TSharedPtr<FJsonObject> CO = V->AsObject();
					if (!CO.IsValid()) continue;
					FString From, To;
					CO->TryGetStringField(TEXT("from"), From);
					CO->TryGetStringField(TEXT("to"),   To);
					int32 DotF, DotT;
					if (!From.FindChar('.', DotF) || !To.FindChar('.', DotT)) continue;
					const FString FromNodeId = From.Left(DotF);
					const FString FromPin    = From.Mid(DotF + 1);
					const FString ToNodeId   = To.Left(DotT);
					const FString ToPin      = To.Mid(DotT + 1);

					// We're interested only in the destination side of an edge that
					// originates at our variable_get/variable_set node.
					if (FromNodeId != VarNodeId) continue;

					TSharedPtr<FJsonObject> ToSpec = FindNodeSpec(ToNodeId);
					if (!ToSpec.IsValid()) continue;
					FString ToType;
					ToSpec->TryGetStringField(TEXT("type"), ToType);
					ToType = ToType.ToLower();

					// Branch condition is always bool.
					if (ToType == TEXT("branch") && ToPin.ToLower() == TEXT("condition"))
					{
						Out = FEdGraphPinType();
						Out.PinCategory = UEdGraphSchema_K2::PC_Boolean;
						return true;
					}

					// Connected to call_function: look up function param type.
					if (ToType == TEXT("call_function"))
					{
						FString FuncName, FuncClassStr;
						const TSharedPtr<FJsonObject>* FuncObj = nullptr;
						if (ToSpec->TryGetObjectField(TEXT("function"), FuncObj) && FuncObj && (*FuncObj).IsValid())
						{
							(*FuncObj)->TryGetStringField(TEXT("name"),  FuncName);
							(*FuncObj)->TryGetStringField(TEXT("class"), FuncClassStr);
						}
						else
						{
							ToSpec->TryGetStringField(TEXT("function"), FuncName);
							ToSpec->TryGetStringField(TEXT("class"),    FuncClassStr);
						}
						bool bSelfCtx = false;
						ToSpec->TryGetBoolField(TEXT("self_context"), bSelfCtx);
						UClass* TargetClass = nullptr;
						if (bSelfCtx && BP->GeneratedClass) TargetClass = BP->GeneratedClass;
						else if (!FuncClassStr.IsEmpty())   TargetClass = ResolveGameClass(FuncClassStr);
						if (!TargetClass || FuncName.IsEmpty()) continue;
						UFunction* Func = TargetClass->FindFunctionByName(*FuncName);
						if (!Func) continue;
						FProperty* ParamProp = Func->FindPropertyByName(FName(*ToPin));
						if (!ParamProp) continue;
						if (PinTypeFromProperty(ParamProp, Out)) return true;
					}

					// Connected to another variable_get/_set of an existing var: copy its pin type.
					if (ToType == TEXT("variable_get") || ToType == TEXT("variable_set"))
					{
						FString OtherVar;
						ToSpec->TryGetStringField(TEXT("variable"), OtherVar);
						if (OtherVar.IsEmpty()) ToSpec->TryGetStringField(TEXT("name"), OtherVar);
						if (OtherVar.IsEmpty() || OtherVar == VarName) continue;
						const FName OtherFName(*OtherVar);
						// Try Skeleton/Generated first (existing var).
						auto FindOnClass = [](UClass* K, const FName& N) -> FProperty*
						{
							return K ? K->FindPropertyByName(N) : nullptr;
						};
						FProperty* OtherProp = FindOnClass(BP->SkeletonGeneratedClass, OtherFName);
						if (!OtherProp) OtherProp = FindOnClass(BP->GeneratedClass, OtherFName);
						if (OtherProp && PinTypeFromProperty(OtherProp, Out)) return true;
					}
				}
				return false;
			};

			for (const TSharedPtr<FJsonValue>& V : NodesCopy)
			{
				if (!V.IsValid()) continue;
				TSharedPtr<FJsonObject> NodeObj = V->AsObject();
				if (!NodeObj.IsValid()) continue;

				FString TypeStr;
				NodeObj->TryGetStringField(TEXT("type"), TypeStr);
				const FString TLow = TypeStr.ToLower();
				if (TLow != TEXT("variable_get") && TLow != TEXT("variable_set")) continue;

				// self_context defaults to true if unset.
				bool bSelfCtx = true;
				NodeObj->TryGetBoolField(TEXT("self_context"), bSelfCtx);
				if (!bSelfCtx) continue;

				FString VarName;
				NodeObj->TryGetStringField(TEXT("variable"), VarName);
				if (VarName.IsEmpty()) NodeObj->TryGetStringField(TEXT("name"), VarName);
				if (!IsValidIdent(VarName)) continue;

				const FName VarFName(*VarName);
				if (VarAlreadyResolves(VarFName)) continue;

				FString NodeId;
				NodeObj->TryGetStringField(TEXT("id"), NodeId);

				FEdGraphPinType InferredType;
				if (!InferTypeFromConnections(NodeId, VarName, InferredType))
				{
					// Type inference failed — best-effort, let the existing
					// flow surface its error.
					continue;
				}

				const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(
					BP, VarFName, InferredType, TEXT(""));
				if (bAdded)
				{
					AutoCreatedVars.Add(VarName);
					UE_LOG(LogNGGBridge, Log,
						TEXT("/bp/add_logic: auto-created self variable '%s' (inferred from connection)"),
						*VarName);
				}
			}
		}

		// ---- 1. Create nodes ------------------------------------------------
		for (const TSharedPtr<FJsonValue>& V : NodesCopy)
		{
			const TSharedPtr<FJsonObject>& NodeObj = V.IsValid() ? V->AsObject() : nullptr;
			if (!NodeObj.IsValid()) continue;

			FString Id, TypeStr;
			NodeObj->TryGetStringField(TEXT("id"),   Id);
			NodeObj->TryGetStringField(TEXT("type"), TypeStr);
			UClass* Cls = ResolveErgoType(TypeStr);
			if (!Cls)
			{
				Callback(JsonError(400, FString::Printf(TEXT("unknown node type '%s' (id=%s)"), *TypeStr, *Id)));
				return;
			}

			UK2Node* Node = NewObject<UK2Node>(Graph, Cls);
			if (!Node)
			{
				Callback(JsonError(500, FString::Printf(TEXT("[id=%s] failed to allocate node of type '%s'"), *Id, *TypeStr)));
				return;
			}
			Node->CreateNewGuid();
			const TSharedPtr<FJsonObject> Cfg = BuildConfig(NodeObj);
			const FString CfgErr = ConfigureNodePreAllocate(Node, BP, Cfg);
			if (!CfgErr.IsEmpty())
			{
				// Don't leave a half-initialized, pin-less node outered to the graph.
				Node->DestroyNode();
				Callback(JsonError(400, FString::Printf(TEXT("[id=%s] %s"), *Id, *CfgErr)));
				return;
			}
			Graph->Modify();
			Node->Modify();
			Graph->AddNode(Node, false, false);
			Node->AllocateDefaultPins();

			// Apply defaults
			const TSharedPtr<FJsonObject>* DefaultsPtr = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("defaults"), DefaultsPtr) && DefaultsPtr && (*DefaultsPtr).IsValid())
			{
				// UE5.8: FJsonObject::Values is keyed by UE::FSharedString, not FString.
				for (const auto& KV : (*DefaultsPtr)->Values)
				{
					UEdGraphPin* Pin = FindPinByName(Node, *KV.Key);
					if (!Pin || !KV.Value.IsValid()) continue;
					FString Val;
					if (KV.Value->Type == EJson::Boolean) Val = KV.Value->AsBool() ? TEXT("true") : TEXT("false");
					else if (KV.Value->Type == EJson::Number) Val = FString::SanitizeFloat(KV.Value->AsNumber());
					else KV.Value->TryGetString(Val);
					FString PinErr;
					ApplyPinDefault(Pin, Val, PinErr);
				}
			}

			// ExecutionSequence: add extra Then_N pins if requested (>2)
			if (auto* Seq = Cast<UK2Node_ExecutionSequence>(Node))
			{
				int32 DesiredOutputs = 2;
				NodeObj->TryGetNumberField(TEXT("outputs"), DesiredOutputs);
				// Default allocation gives Then_0 and Then_1; keep adding pins until
				// the node actually has the requested count. Cap iterations so we
				// never spin forever if AddInputPin can't grow the node.
				int32 Guard = 0;
				const int32 MaxIters = FMath::Max(0, DesiredOutputs) + 16;
				while (DesiredOutputs > 2
					&& Seq->GetThenPinGivenIndex(DesiredOutputs - 1) == nullptr
					&& Guard++ < MaxIters)
				{
					Seq->AddInputPin();
				}
			}

			if (!Id.IsEmpty())
			{
				LocalIdToGuid.Add(Id, Node->NodeGuid);
				Self->BpNodeIdMap.Add(MakeBpNodeKey(BpPath, Id),
					FBpNodeIdEntry{ Node->NodeGuid, FPlatformTime::Seconds() });
				NodeIdsOut->SetStringField(Id, Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			}
			CreatedNodes.Add(Node);
		}

		// ---- 2. Auto-layout -------------------------------------------------
		if (bAutoLayout)
		{
			int32 X = 0;
			for (UK2Node* N : CreatedNodes)
			{
				N->NodePosX = X;
				N->NodePosY = 0;
				X += 400;
			}
		}

		// ---- 3. Connections ------------------------------------------------
		const UEdGraphSchema_K2* K2 = GetDefault<UEdGraphSchema_K2>();
		TArray<FString> ConnErrors;
		for (const TSharedPtr<FJsonValue>& V : ConnsCopy)
		{
			const TSharedPtr<FJsonObject>& C = V.IsValid() ? V->AsObject() : nullptr;
			if (!C.IsValid()) continue;
			FString From, To;
			C->TryGetStringField(TEXT("from"), From);
			C->TryGetStringField(TEXT("to"),   To);
			int32 DotF, DotT;
			if (!From.FindChar('.', DotF) || !To.FindChar('.', DotT))
			{
				ConnErrors.Add(FString::Printf(TEXT("bad connection spec: %s -> %s (expected 'node.pin')"), *From, *To));
				continue;
			}
			const FString FromNodeId = From.Left(DotF);
			const FString FromPin    = From.Mid(DotF + 1);
			const FString ToNodeId   = To.Left(DotT);
			const FString ToPin      = To.Mid(DotT + 1);
			// Resolve a node id string into a graph node GUID. Accepts:
			//   1. A user-supplied id from THIS request's `nodes` array
			//      (LocalIdToGuid lookup).
			//   2. A user-supplied id from a PRIOR request (BpNodeIdMap, scoped
			//      to this Blueprint path).
			//   3. A raw GUID string (e.g. echoed back from a previous
			//      bp_add_logic / bp_add_node response).
			auto ResolveNodeGuid = [&](const FString& IdStr) -> FGuid
			{
				if (const FGuid* G = LocalIdToGuid.Find(IdStr))
				{
					return *G;
				}
				if (const FBpNodeIdEntry* Entry = Self->BpNodeIdMap.Find(MakeBpNodeKey(BpPath, IdStr)))
				{
					return Entry->NodeGuid;
				}
				FGuid Parsed;
				if (FGuid::Parse(IdStr, Parsed))
				{
					return Parsed;
				}
				return FGuid();
			};

			const FGuid AGuidVal = ResolveNodeGuid(FromNodeId);
			const FGuid BGuidVal = ResolveNodeGuid(ToNodeId);
			if (!AGuidVal.IsValid() || !BGuidVal.IsValid())
			{
				ConnErrors.Add(FString::Printf(
					TEXT("node id not found: '%s' or '%s' (accepted: this-request node ids, prior-request node ids, or raw GUIDs)"),
					*FromNodeId, *ToNodeId));
				continue;
			}
			UEdGraphNode *A = nullptr, *B = nullptr;
			for (UEdGraphNode* N : Graph->Nodes)
			{
				if (N && N->NodeGuid == AGuidVal) A = N;
				if (N && N->NodeGuid == BGuidVal) B = N;
			}
			if (!A || !B)
			{
				ConnErrors.Add(FString::Printf(TEXT("node lookup failed post-guid: %s or %s"), *FromNodeId, *ToNodeId));
				continue;
			}
			UEdGraphPin* PA = FindPinByName(A, FromPin);
			UEdGraphPin* PB = FindPinByName(B, ToPin);
			if (!PA && !PB)
			{
				ConnErrors.Add(FString::Printf(
					TEXT("pin not found on either side: from='%s.%s' to='%s.%s' (call ue5_bp_read_graph to see real pin names)"),
					*FromNodeId, *FromPin, *ToNodeId, *ToPin));
				continue;
			}
			if (!PA)
			{
				ConnErrors.Add(FString::Printf(
					TEXT("pin not found: from='%s.%s' (to='%s.%s' resolved OK; check the source pin's real name)"),
					*FromNodeId, *FromPin, *ToNodeId, *ToPin));
				continue;
			}
			if (!PB)
			{
				ConnErrors.Add(FString::Printf(
					TEXT("pin not found: to='%s.%s' (from='%s.%s' resolved OK; check the destination pin's real name)"),
					*ToNodeId, *ToPin, *FromNodeId, *FromPin));
				continue;
			}

			// Direction-fix for variable_set: see HandleBpConnectPins for
			// rationale. A UK2Node_VariableSet's data pin named '<VarName>' is
			// the input value; the output passthrough is 'Output_Get'.
			if (PA && PA->Direction != EGPD_Output && Cast<UK2Node_VariableSet>(A))
			{
				if (UEdGraphPin* OutGet = A->FindPin(TEXT("Output_Get")))
				{
					PA = OutGet;
				}
			}
			if (PB && PB->Direction != EGPD_Input && Cast<UK2Node_VariableSet>(B))
			{
				if (UEdGraphPin* OutGet = B->FindPin(TEXT("Output_Get")))
				{
					PB = OutGet;
				}
			}

			// Probe before connecting: a DISALLOW response means TryCreateConnection
			// may silently break an existing single-link connection. Record the
			// schema's own reason instead of relying on TryCreateConnection's bool.
			const FPinConnectionResponse Resp = K2->CanCreateConnection(PA, PB);
			if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
			{
				const FString Why = Resp.Message.ToString();
				ConnErrors.Add(FString::Printf(TEXT("connection disallowed: %s.%s -> %s.%s (%s)"),
					*FromNodeId, *FromPin, *ToNodeId, *ToPin,
					Why.IsEmpty() ? TEXT("type mismatch or directionality") : *Why));
				continue;
			}

			Graph->Modify();
			A->Modify();
			B->Modify();
			if (K2->TryCreateConnection(PA, PB))
			{
				++NumConnectionsMade;
			}
			else
			{
				ConnErrors.Add(FString::Printf(TEXT("TryCreateConnection refused: %s.%s -> %s.%s"),
					*FromNodeId, *FromPin, *ToNodeId, *ToPin));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_logic: %s nodes=%d connections=%d errors=%d"),
			*BpPath, CreatedNodes.Num(), NumConnectionsMade, ConnErrors.Num());

		// Auto-heal: drop empty duplicate override events (e.g. the leftover
		// empty Event AnyDamage that blocks compile with "more than one
		// function with the same name"). Done regardless of bCompile because
		// the caller may follow up with a separate compile call.
		const int32 HealedCount = AutoHealDuplicateEvents(BP);
		// Repair invalid override-events (CustomEvents misflagged as overrides,
		// or Events whose target function doesn't exist on the parent). Without
		// this, ConformImplementedEvents -> FixOverriddenEventSignature asserts
		// and kills the editor.
		AutoHealInvalidOverrideEvents(BP);
		// Repair stale override function-graphs (parent function renamed/removed,
		// or graph with FName=None). Without this, ConformCallsToParentFunctions
		// asserts on the next compile.
		AutoHealStaleOverrideFunctions(BP);

		// Full lint + autofix pass so add_logic doesn't leave orphan pins,
		// stale refs, or reinst class refs behind — the common "dirty graph"
		// symptom when opening the actor after a write sequence. Runs BEFORE
		// compile so compile sees the cleaned graph; unfixable issues are
		// returned to the caller via the "lint" field so they can act.
		const TSharedRef<FJsonObject> LintReport = BuildLintReport(BP, /*bAutoFix=*/true);

		// ---- 4. Compile + save (default-on) --------------------------------
		TSharedPtr<FJsonObject> CompileInfo;
		bool bCompileOk = true;
		if (bCompile)
		{
			FCompilerResultsLog Results;
			Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			bCompileOk = (Results.NumErrors == 0);
			CompileInfo = MakeShared<FJsonObject>();
			CompileInfo->SetBoolField(TEXT("success"), bCompileOk);
			CompileInfo->SetNumberField(TEXT("errors"),   Results.NumErrors);
			CompileInfo->SetNumberField(TEXT("warnings"), Results.NumWarnings);
			TArray<TSharedPtr<FJsonValue>> MsgArr;
			for (const TSharedRef<FTokenizedMessage>& M : Results.Messages)
			{
				TSharedPtr<FJsonObject> MO = MakeShared<FJsonObject>();
				MO->SetStringField(TEXT("severity"),
					M->GetSeverity() == EMessageSeverity::Error ? TEXT("error") :
					M->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
				MO->SetStringField(TEXT("text"), M->ToText().ToString());
				MsgArr.Add(MakeShared<FJsonValueObject>(MO));
			}
			CompileInfo->SetArrayField(TEXT("messages"), MsgArr);
		}

		// Don't persist a graph with failed connections — that's a broken graph,
		// and saving it would falsely advertise success. Require clean connections
		// AND a clean compile before writing to disk.
		const bool bConnectionsOk = (ConnErrors.Num() == 0);
		bool bSaved = false;
		if (bSave && bCompileOk && bConnectionsOk)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
			UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_logic: %s saved=%s"),
				*BpPath, bSaved ? TEXT("yes") : TEXT("no"));
		}

		// success requires connections to have succeeded AND, when a save was
		// requested, that the save actually wrote to disk.
		const bool bOverallSuccess = bConnectionsOk && (!bSave || bSaved);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),           bOverallSuccess);
		Out->SetNumberField(TEXT("nodes_created"),     CreatedNodes.Num());
		Out->SetNumberField(TEXT("connections_made"),  NumConnectionsMade);
		Out->SetObjectField(TEXT("node_ids"),          NodeIdsOut);
		if (ConnErrors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Errs;
			for (const FString& E : ConnErrors) Errs.Add(MakeShared<FJsonValueString>(E));
			Out->SetArrayField(TEXT("connection_errors"), Errs);
		}
		if (CompileInfo.IsValid())
		{
			Out->SetObjectField(TEXT("compile"), CompileInfo);
		}
		Out->SetBoolField(TEXT("saved"), bSaved);
		if (HealedCount > 0) Out->SetNumberField(TEXT("auto_healed"), HealedCount);
		if (AutoCreatedVars.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FString& Name : AutoCreatedVars) Arr.Add(MakeShared<FJsonValueString>(Name));
			Out->SetArrayField(TEXT("auto_created_variables"), Arr);
		}
		Out->SetObjectField(TEXT("lint"), LintReport);
		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: GET /bp/read_graph
// ============================================================================

bool FNGGHttpServer::HandleBpReadGraph(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;

	const FString BpPath    = GetQueryParam(Req, TEXT("blueprint"));
	const FString GraphName = GetQueryParam(Req, TEXT("graph"));

	if (BpPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint query param is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, GraphName]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}

		UClass* SelfScope = BP ? BP->GeneratedClass : nullptr;
		TFunction<TSharedPtr<FJsonObject>(UEdGraph*)> SerializeGraph;
			SerializeGraph = [SelfScope, &SerializeGraph](UEdGraph* G) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> GO = MakeShared<FJsonObject>();
			if (!G) return GO;
			TArray<TSharedPtr<FJsonValue>> NodesArr;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (!N) continue;
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("node_id"), N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				NodeObj->SetStringField(TEXT("class"),   N->GetClass()->GetName());
				NodeObj->SetStringField(TEXT("name"),    N->GetName());

				// Target reference: what other class/function/asset this node points at.
				// Self-member references report no parent class, so fall back to the
				// owning Blueprint's generated class (a self-edge the order tool drops).
				if (UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(N))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("function"));
					UClass* ParentCls = CF->FunctionReference.GetMemberParentClass();
					if (!ParentCls) ParentCls = SelfScope;
					if (ParentCls) T->SetStringField(TEXT("class"), ParentCls->GetPathName());
					T->SetStringField(TEXT("name"), CF->FunctionReference.GetMemberName().ToString());
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(N))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("cast"));
					if (CastNode->TargetType)
					{
						T->SetStringField(TEXT("class"), CastNode->TargetType->GetPathName());
					}
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				else if (UK2Node_Variable* VN = Cast<UK2Node_Variable>(N))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("variable"));
					UClass* ParentCls = VN->VariableReference.GetMemberParentClass();
					if (!ParentCls) ParentCls = SelfScope;
					if (ParentCls) T->SetStringField(TEXT("class"), ParentCls->GetPathName());
					T->SetStringField(TEXT("name"), VN->VariableReference.GetMemberName().ToString());
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				else if (UK2Node_MacroInstance* MI = Cast<UK2Node_MacroInstance>(N))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("macro"));
					if (UEdGraph* MG = MI->GetMacroGraph())
					{
						T->SetStringField(TEXT("graph"), MG->GetPathName());
					}
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				else if (UK2Node_Composite* CompNode = Cast<UK2Node_Composite>(N))
				{
					// Collapsed-to-graph node: its logic lives in a BoundGraph subgraph that
					// read_graph never visits otherwise. Recurse into it (and any nested
					// composites) so the internals are inlined under "composite" -- unblocks
					// porting functions whose body is inside a collapse node (e.g. IsPivoting).
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("composite"));
					if (CompNode->BoundGraph)
					{
						T->SetStringField(TEXT("graph"), CompNode->BoundGraph->GetName());
						NodeObj->SetObjectField(TEXT("composite"), SerializeGraph(CompNode->BoundGraph));
					}
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				// PropertyAccess (fast-path AnimGraph property reads) stores its bound path in private UPROPERTYs
					// Path (TArray<FString> segments) + TextPath (display). UK2Node_PropertyAccess is in a plugin
					// Private folder, so resolve it generically by reflection -- otherwise the bound path is
					// invisible to read_graph (blocks porting PropertyAccess-driven functions).
					else if (N->GetClass()->GetName() == TEXT("K2Node_PropertyAccess"))
					{
						TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
						T->SetStringField(TEXT("kind"), TEXT("property_access"));
						if (FTextProperty* TextProp = FindFProperty<FTextProperty>(N->GetClass(), TEXT("TextPath")))
						{
							T->SetStringField(TEXT("text_path"), TextProp->GetPropertyValue_InContainer(N).ToString());
						}
						if (FArrayProperty* PathProp = FindFProperty<FArrayProperty>(N->GetClass(), TEXT("Path")))
						{
							if (const FStrProperty* Inner = CastField<FStrProperty>(PathProp->Inner))
							{
								FScriptArrayHelper Helper(PathProp, PathProp->ContainerPtrToValuePtr<void>(N));
								TArray<TSharedPtr<FJsonValue>> Segments;
								for (int32 SegIdx = 0; SegIdx < Helper.Num(); ++SegIdx)
								{
									Segments.Add(MakeShared<FJsonValueString>(Inner->GetPropertyValue(Helper.GetRawPtr(SegIdx))));
								}
								T->SetArrayField(TEXT("path"), Segments);
							}
						}
						NodeObj->SetObjectField(TEXT("target"), T);
					}
					// Enhanced Input nodes (GetInputActionValue, EnhancedInputAction event) bind an
				// InputAction asset stored as a UPROPERTY, not a pin. Surface it generically via
				// reflection so the conversion tooling can resolve which IA a node reads.
				else if (FObjectPropertyBase* IAProp =
							 FindFProperty<FObjectPropertyBase>(N->GetClass(), TEXT("InputAction")))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("input_action"));
					if (const UObject* IA = IAProp->GetObjectPropertyValue_InContainer(N))
					{
						T->SetStringField(TEXT("class"), IA->GetClass()->GetPathName());
						T->SetStringField(TEXT("name"),  IA->GetName());
						T->SetStringField(TEXT("path"),  IA->GetPathName());
					}
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				// Custom events expose their authored name; override events (BeginPlay, Landed, ...)
				// expose the overridden function name + its declaring class. (CustomEvent derives
				// from Event, so it must be tested first.)
				else if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("custom_event"));
					T->SetStringField(TEXT("name"), CE->CustomFunctionName.ToString());
					NodeObj->SetObjectField(TEXT("target"), T);
				}
				else if (UK2Node_Event* EV = Cast<UK2Node_Event>(N))
				{
					TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
					T->SetStringField(TEXT("kind"), TEXT("event"));
					T->SetStringField(TEXT("name"), EV->EventReference.GetMemberName().ToString());
					if (UClass* EvtCls = EV->EventReference.GetMemberParentClass())
					{
						T->SetStringField(TEXT("class"), EvtCls->GetPathName());
					}
					NodeObj->SetObjectField(TEXT("target"), T);
				}

				TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
				Pos->SetNumberField(TEXT("x"), N->NodePosX);
				Pos->SetNumberField(TEXT("y"), N->NodePosY);
				NodeObj->SetObjectField(TEXT("position"), Pos);
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (UEdGraphPin* P : N->Pins)
				{
					Pins.Add(MakeShared<FJsonValueObject>(PinToJson(P)));
				}
				NodeObj->SetArrayField(TEXT("pins"), Pins);
				NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			GO->SetArrayField(TEXT("nodes"), NodesArr);
			return GO;
		};

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),   true);
		Out->SetStringField(TEXT("blueprint"), BpPath);

		if (!GraphName.IsEmpty())
		{
			UEdGraph* G = FindBlueprintGraph(BP, GraphName);
			if (!G)
			{
				Callback(JsonError(404, FString::Printf(TEXT("Graph '%s' not found"), *GraphName)));
				return;
			}
			Out->SetStringField(TEXT("graph"), G->GetName());
			TSharedPtr<FJsonObject> GO = SerializeGraph(G);
			Out->SetArrayField(TEXT("nodes"), GO->GetArrayField(TEXT("nodes")));
		}
		else
		{
			TSharedPtr<FJsonObject> GraphsObj = MakeShared<FJsonObject>();
			auto AddSet = [&](const TArray<TObjectPtr<UEdGraph>>& Set)
			{
				for (UEdGraph* G : Set)
				{
					if (!G) continue;
					GraphsObj->SetObjectField(G->GetName(), SerializeGraph(G));
				}
			};
			AddSet(BP->UbergraphPages);
			AddSet(BP->FunctionGraphs);
			AddSet(BP->MacroGraphs);
			Out->SetObjectField(TEXT("graphs"), GraphsObj);
		}
		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: GET /bp/get_selection
// Query: ?blueprint=/Game/...&include_pins=1&include_boundary=1
//
// Returns the nodes the developer currently has selected in an open Blueprint
// editor, plus an optional "boundary" classification of pins whose links cross
// the selection set (the data needed to extract a selection into a function).
//
// If 'blueprint' is omitted, picks the most recently-active BP editor that
// has a non-empty selection (falls back to any open BP editor with empty
// selection so callers get a deterministic answer).
// ============================================================================
bool FNGGHttpServer::HandleBpGetSelection(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	using namespace NGGBpPriv;

	FHttpResultCallback Callback = OnComplete;

	const FString BpPath          = GetQueryParam(Req, TEXT("blueprint"));
	const FString IncludePinsStr  = GetQueryParam(Req, TEXT("include_pins"));
	const FString IncludeBoundStr = GetQueryParam(Req, TEXT("include_boundary"));
	const bool    bIncludePins    = IncludePinsStr.IsEmpty()  ? true : !IncludePinsStr.Equals(TEXT("0"));
	const bool    bIncludeBound   = IncludeBoundStr.IsEmpty() ? true : !IncludeBoundStr.Equals(TEXT("0"));

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, bIncludePins, bIncludeBound]()
	{
		UAssetEditorSubsystem* AES = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!AES)
		{
			Callback(JsonError(500, TEXT("AssetEditorSubsystem unavailable (editor not running?)")));
			return;
		}

		// Resolve which BP editor to inspect.
		FBlueprintEditor* BPEditor = nullptr;
		UBlueprint*       BP       = nullptr;

		auto AsBlueprintEditor = [](IAssetEditorInstance* Inst) -> FBlueprintEditor*
		{
			if (!Inst) return nullptr;
			// FBlueprintEditor toolkits report this name; "WidgetBlueprintEditor" is a
			// distinct subclass we currently exclude.
			if (Inst->GetEditorName() == FName("BlueprintEditor"))
			{
				return static_cast<FBlueprintEditor*>(Inst);
			}
			return nullptr;
		};

		if (!BpPath.IsEmpty())
		{
			BP = LoadBlueprintFlexible(BpPath);
			if (!BP)
			{
				Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
				return;
			}
			IAssetEditorInstance* Inst = AES->FindEditorForAsset(BP, /*bFocusIfOpen=*/false);
			if (!Inst)
			{
				Callback(JsonError(409, TEXT("Blueprint is not open in the editor")));
				return;
			}
			BPEditor = AsBlueprintEditor(Inst);
			if (!BPEditor)
			{
				Callback(JsonError(409, TEXT("Open editor for this asset is not a BlueprintEditor")));
				return;
			}
		}
		else
		{
			// No BP specified — pick the most-recently-active BP editor with a
			// non-empty selection; fall back to any open BP editor.
			FBlueprintEditor* AnyBPEditor = nullptr;
			UBlueprint*       AnyBP       = nullptr;
			TArray<UObject*>  OpenAssets  = AES->GetAllEditedAssets();
			for (UObject* Asset : OpenAssets)
			{
				UBlueprint* CandidateBP = Cast<UBlueprint>(Asset);
				if (!CandidateBP) continue;
				IAssetEditorInstance* Inst = AES->FindEditorForAsset(CandidateBP, /*bFocusIfOpen=*/false);
				FBlueprintEditor* Cand = AsBlueprintEditor(Inst);
				if (!Cand) continue;
				if (!AnyBPEditor) { AnyBPEditor = Cand; AnyBP = CandidateBP; }
				if (Cand->GetSelectedNodes().Num() > 0)
				{
					BPEditor = Cand;
					BP       = CandidateBP;
					break;
				}
			}
			if (!BPEditor)
			{
				BPEditor = AnyBPEditor;
				BP       = AnyBP;
			}
			if (!BPEditor)
			{
				Callback(JsonError(409, TEXT("No Blueprint editors are open")));
				return;
			}
		}

		// Query focused graph + selection set.
		UEdGraph* Focused = BPEditor->GetFocusedGraph();
		const FGraphPanelSelectionSet Selected = BPEditor->GetSelectedNodes();

		auto DescribeGraphType = [](UBlueprint* InBP, UEdGraph* G) -> FString
		{
			if (!InBP || !G) return TEXT("Unknown");
			if (InBP->UbergraphPages.Contains(G))    return TEXT("Ubergraph");
			if (InBP->FunctionGraphs.Contains(G))    return TEXT("Function");
			if (InBP->MacroGraphs.Contains(G))       return TEXT("Macro");
			if (InBP->IntermediateGeneratedGraphs.Contains(G)) return TEXT("Intermediate");
			return TEXT("Other");
		};

		// Build selected-node GUID set up front for boundary classification.
		TSet<FGuid> SelectedGuids;
		for (UObject* Obj : Selected)
		{
			if (UEdGraphNode* N = Cast<UEdGraphNode>(Obj))
			{
				SelectedGuids.Add(N->NodeGuid);
			}
		}

		// Serialise one selected node — same shape HandleBpReadGraph emits, with
		// pins optional.
		auto NodeToJson = [bIncludePins](UEdGraphNode* N) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			if (!N) return NodeObj;
			NodeObj->SetStringField(TEXT("node_id"), N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			NodeObj->SetStringField(TEXT("node_guid"), N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			NodeObj->SetStringField(TEXT("class"),   N->GetClass()->GetName());
			NodeObj->SetStringField(TEXT("name"),    N->GetName());
			NodeObj->SetStringField(TEXT("title"),   N->GetNodeTitle(ENodeTitleType::ListView).ToString());
			TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
			Pos->SetNumberField(TEXT("x"), N->NodePosX);
			Pos->SetNumberField(TEXT("y"), N->NodePosY);
			NodeObj->SetObjectField(TEXT("position"), Pos);
			if (bIncludePins)
			{
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (UEdGraphPin* P : N->Pins)
				{
					Pins.Add(MakeShared<FJsonValueObject>(PinToJson(P)));
				}
				NodeObj->SetArrayField(TEXT("pins"), Pins);
			}
			return NodeObj;
		};

		// Boundary classification: a pin is a boundary pin if at least one of
		// its LinkedTo endpoints is a node OUTSIDE the selection.
		auto MakeBoundaryEntry = [](UEdGraphNode* SelfNode, UEdGraphPin* SelfPin,
		                            const TArray<UEdGraphPin*>& ExternalLinks) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("node_guid"), SelfNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			E->SetStringField(TEXT("pin_name"),  SelfPin->PinName.ToString());
			E->SetStringField(TEXT("pin_category"), SelfPin->PinType.PinCategory.ToString());
			E->SetStringField(TEXT("direction"), SelfPin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			TArray<TSharedPtr<FJsonValue>> Links;
			for (UEdGraphPin* Ext : ExternalLinks)
			{
				if (!Ext || !Ext->GetOwningNode()) continue;
				TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
				L->SetStringField(TEXT("node_guid"), Ext->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				L->SetStringField(TEXT("pin_name"),  Ext->PinName.ToString());
				Links.Add(MakeShared<FJsonValueObject>(L));
			}
			E->SetArrayField(TEXT("links"), Links);
			return E;
		};

		TArray<TSharedPtr<FJsonValue>> NodesJson;
		TArray<TSharedPtr<FJsonValue>> ExtIn, ExtOut, ExtExecIn, ExtExecOut;

		for (UObject* Obj : Selected)
		{
			UEdGraphNode* Node = Cast<UEdGraphNode>(Obj);
			if (!Node) continue;
			NodesJson.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));

			if (!bIncludeBound) continue;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->LinkedTo.Num() == 0) continue;
				TArray<UEdGraphPin*> ExternalLinks;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					if (!SelectedGuids.Contains(Linked->GetOwningNode()->NodeGuid))
					{
						ExternalLinks.Add(Linked);
					}
				}
				if (ExternalLinks.Num() == 0) continue;

				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				TSharedPtr<FJsonObject> Entry = MakeBoundaryEntry(Node, Pin, ExternalLinks);
				if (bExec)
				{
					if (Pin->Direction == EGPD_Input)
						ExtExecIn.Add(MakeShared<FJsonValueObject>(Entry));
					else
						ExtExecOut.Add(MakeShared<FJsonValueObject>(Entry));
				}
				else
				{
					if (Pin->Direction == EGPD_Input)
						ExtIn.Add(MakeShared<FJsonValueObject>(Entry));
					else
						ExtOut.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),         true);
		Out->SetStringField(TEXT("blueprint"),       BP ? BP->GetPathName() : FString());
		Out->SetStringField(TEXT("graph"),           Focused ? Focused->GetName() : FString());
		Out->SetStringField(TEXT("graph_type"),      DescribeGraphType(BP, Focused));
		Out->SetNumberField(TEXT("selection_count"), Selected.Num());
		Out->SetArrayField (TEXT("nodes"),           NodesJson);

		if (bIncludeBound)
		{
			TSharedPtr<FJsonObject> Boundary = MakeShared<FJsonObject>();
			Boundary->SetArrayField(TEXT("external_inputs"),   ExtIn);
			Boundary->SetArrayField(TEXT("external_outputs"),  ExtOut);
			Boundary->SetArrayField(TEXT("external_exec_in"),  ExtExecIn);
			Boundary->SetArrayField(TEXT("external_exec_out"), ExtExecOut);
			Out->SetObjectField(TEXT("boundary"), Boundary);
		}

		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/delete_node
// Body: { blueprint, graph?, node_id?, node_guid?, compile?, save? }
// Deletes a node identified by user-supplied node_id (from the transient map)
// OR by raw node_guid. Optionally compiles + saves after deletion. This is the
// escape hatch for cleaning up stale / duplicate nodes.
// ============================================================================
// ============================================================================
// Handler: POST /bp/add_interface
// Body: { blueprint, interface, compile?, save? }
// Adds an existing interface (BP interface asset path, or a /Script class path) to the Blueprint's
// implemented-interfaces list via FBlueprintEditorUtils::ImplementNewInterface. Optionally recompiles.
// ============================================================================
bool FNGGHttpServer::HandleBpAddInterface(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, FString::Printf(TEXT("invalid json: %s"), *ParseErr)));
		return true;
	}

	FString BpPath;    Body->TryGetStringField(TEXT("blueprint"), BpPath);
	FString IfacePath; Body->TryGetStringField(TEXT("interface"), IfacePath);
	bool bCompile = true; Body->TryGetBoolField(TEXT("compile"), bCompile);

	if (BpPath.IsEmpty() || IfacePath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and interface are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, IfacePath, bCompile]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		// Resolve the interface UClass: try a direct class path first, then a BP-interface asset.
		UClass* IfaceClass = LoadObject<UClass>(nullptr, *IfacePath);
		if (!IfaceClass)
		{
			if (UBlueprint* IfaceBP = LoadBlueprintFlexible(IfacePath))
			{
				IfaceClass = IfaceBP->GeneratedClass;
			}
		}
		if (!IfaceClass)
		{
			Callback(JsonError(404, FString::Printf(TEXT("interface class not found: %s"), *IfacePath)));
			return;
		}

		const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(
			BP, FTopLevelAssetPath(IfaceClass->GetPathName()));

		if (bCompile && BP->ParentClass != nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		BP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/add_interface: %s += %s (added=%d)"),
			*BpPath, *IfaceClass->GetName(), bAdded ? 1 : 0);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),   true);
		Out->SetStringField(TEXT("blueprint"), BpPath);
		Out->SetStringField(TEXT("interface"), IfaceClass->GetPathName());
		Out->SetBoolField  (TEXT("added"),     bAdded);
		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/implement_interface_function
// Body: { blueprint, function, compile?, save? }
// Creates the implementation graph for an interface function the Blueprint already implements
// (functions WITH return values aren't auto-graphed by ImplementNewInterface; this mirrors what
// the editor's "implement" action does -- CreateNewGraph + AddFunctionGraph with the interface
// class as the signature source). Unlike create_function this binds as the interface override
// instead of creating a colliding standalone function.
// ============================================================================
bool FNGGHttpServer::HandleBpImplementInterfaceFunction(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, FString::Printf(TEXT("invalid json: %s"), *ParseErr)));
		return true;
	}

	FString BpPath;   Body->TryGetStringField(TEXT("blueprint"), BpPath);
	FString FuncName; Body->TryGetStringField(TEXT("function"), FuncName);
	bool bCompile = true; Body->TryGetBoolField(TEXT("compile"), bCompile);

	if (BpPath.IsEmpty() || FuncName.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint and function are required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, FuncName, bCompile]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		const FName FuncFName(*FuncName);

		// Already implemented? (graph exists) -> nothing to do.
		if (FindObject<UEdGraph>(BP, *FuncName))
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetBoolField(TEXT("success"), true);
			O->SetStringField(TEXT("blueprint"), BpPath);
			O->SetStringField(TEXT("function"), FuncName);
			O->SetBoolField(TEXT("already_implemented"), true);
			Callback(JsonOk(SerializeJson(O)));
			return;
		}

		// Find which implemented interface owns the function.
		UClass* OwnerClass = nullptr;
		for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
		{
			if (Desc.Interface && Desc.Interface->FindFunctionByName(FuncFName))
			{
				OwnerClass = Desc.Interface;
				break;
			}
		}
		if (!OwnerClass)
		{
			Callback(JsonError(404, FString::Printf(
				TEXT("function '%s' not found on any implemented interface of '%s'"), *FuncName, *BpPath)));
			return;
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FuncFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph(BP, NewGraph, /*bIsUserCreated*/ false, OwnerClass);

		if (bCompile && BP->ParentClass != nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		BP->MarkPackageDirty();

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/implement_interface_function: %s::%s (owner=%s)"),
			*BpPath, *FuncName, *OwnerClass->GetName());

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),   true);
		Out->SetStringField(TEXT("blueprint"), BpPath);
		Out->SetStringField(TEXT("function"),  FuncName);
		Out->SetStringField(TEXT("interface"), OwnerClass->GetPathName());
		Out->SetBoolField  (TEXT("implemented"), true);
		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Handler: POST /bp/refresh_all_nodes
// Body: { blueprint, compile?, save? }
// Reconstructs every node in the Blueprint against its current backing
// function/variable signatures (FBlueprintEditorUtils::RefreshAllNodes) -- the
// canonical fix for nodes left stale after an engine/plugin migration (pins
// renamed/removed, signatures drifted). Optionally recompiles and reports
// whether the Blueprint now compiles clean.
// ============================================================================
bool FNGGHttpServer::HandleBpRefreshAllNodes(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, FString::Printf(TEXT("invalid json: %s"), *ParseErr)));
		return true;
	}

	FString BpPath; Body->TryGetStringField(TEXT("blueprint"), BpPath);
	bool bCompile = true;  Body->TryGetBoolField(TEXT("compile"), bCompile);
	bool bSave    = false; Body->TryGetBoolField(TEXT("save"),    bSave);

	if (BpPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread, [Callback, BpPath, bCompile, bSave]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		// Reconstruct every node against its current backing signatures.
		FBlueprintEditorUtils::RefreshAllNodes(BP);

		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
		}
		const bool bClean = (BP->Status == EBlueprintStatus::BS_UpToDate
			|| BP->Status == EBlueprintStatus::BS_UpToDateWithWarnings);

		if (bSave)
		{
			BP->MarkPackageDirty();
		}

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/refresh_all_nodes: %s compiled_clean=%d status=%d"),
			*BpPath, bClean ? 1 : 0, static_cast<int32>(BP->Status));

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),        true);
		Out->SetStringField(TEXT("blueprint"),      BpPath);
		Out->SetBoolField  (TEXT("refreshed"),      true);
		Out->SetBoolField  (TEXT("compiled"),       bCompile);
		Out->SetBoolField  (TEXT("compiled_clean"), bClean);
		Out->SetNumberField(TEXT("status"),         static_cast<double>(static_cast<int32>(BP->Status)));
		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

bool FNGGHttpServer::HandleBpDeleteNode(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, FString::Printf(TEXT("invalid json: %s"), *ParseErr)));
		return true;
	}

	FString BpPath;          Body->TryGetStringField(TEXT("blueprint"), BpPath);
	FString GraphName;       Body->TryGetStringField(TEXT("graph"),     GraphName);
	FString NodeId;          Body->TryGetStringField(TEXT("node_id"),   NodeId);
	FString NodeGuidStr;     Body->TryGetStringField(TEXT("node_guid"), NodeGuidStr);
	bool bCompile = true;    Body->TryGetBoolField  (TEXT("compile"),   bCompile);
	bool bSave    = true;    Body->TryGetBoolField  (TEXT("save"),      bSave);

	if (BpPath.IsEmpty() || (NodeId.IsEmpty() && NodeGuidStr.IsEmpty()))
	{
		Callback(JsonError(400, TEXT("blueprint and (node_id or node_guid) required")));
		return true;
	}

	TWeakPtr<FNGGHttpServer> WeakSelf = AsWeak();
	AsyncTask(ENamedThreads::GameThread,
		[WeakSelf, Callback, BpPath, GraphName, NodeId, NodeGuidStr, bCompile, bSave]()
	{
		// Bail if the server was destroyed before this deferred task ran.
		TSharedPtr<FNGGHttpServer> Self = WeakSelf.Pin();
		if (!Self.IsValid()) { return; }

		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP) { Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath))); return; }

		// Resolve target guid
		FGuid TargetGuid;
		if (!NodeId.IsEmpty())
		{
			const FString Key = MakeBpNodeKey(BpPath, NodeId);
			if (const FBpNodeIdEntry* Hit = Self->BpNodeIdMap.Find(Key))
			{
				TargetGuid = Hit->NodeGuid;
			}
		}
		if (!TargetGuid.IsValid() && !NodeGuidStr.IsEmpty())
		{
			FGuid::Parse(NodeGuidStr, TargetGuid);
		}
		if (!TargetGuid.IsValid())
		{
			Callback(JsonError(404, TEXT("could not resolve node id or guid")));
			return;
		}

		// Search all relevant graphs if graph not specified
		TArray<UEdGraph*> Graphs;
		if (!GraphName.IsEmpty())
		{
			if (UEdGraph* G = FindBlueprintGraph(BP, GraphName)) Graphs.Add(G);
		}
		else
		{
			Graphs.Append(BP->UbergraphPages);
			Graphs.Append(BP->FunctionGraphs);
			Graphs.Append(BP->MacroGraphs);
		}

		UEdGraphNode* FoundNode = nullptr;
		UEdGraph* OwningGraph = nullptr;
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (N && N->NodeGuid == TargetGuid) { FoundNode = N; OwningGraph = G; break; }
			}
			if (FoundNode) break;
		}
		if (!FoundNode)
		{
			Callback(JsonError(404, TEXT("node not found in blueprint")));
			return;
		}

		const FString ClassName = FoundNode->GetClass()->GetName();
		UE_LOG(LogNGGBridge, Log, TEXT("/bp/delete_node: %s guid=%s class=%s"),
			*BpPath, *TargetGuid.ToString(EGuidFormats::DigitsWithHyphens), *ClassName);

		// Remove from graph (handles pin disconnection internally).
		// Mark graph + node modified first so undo state isn't corrupted.
		OwningGraph->Modify();
		FoundNode->Modify();
		OwningGraph->RemoveNode(FoundNode);

		// Drop stale id map entries pointing at this guid
		TArray<FString> ToRemove;
		for (const auto& KV : Self->BpNodeIdMap)
		{
			if (KV.Value.NodeGuid == TargetGuid) ToRemove.Add(KV.Key);
		}
		for (const FString& K : ToRemove) Self->BpNodeIdMap.Remove(K);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		// Compile + save (default on)
		TSharedPtr<FJsonObject> CompileInfo;
		bool bCompileOk = true;
		if (bCompile)
		{
			FCompilerResultsLog Results; Results.bSilentMode = true;
			FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
			bCompileOk = (Results.NumErrors == 0);
			CompileInfo = MakeShared<FJsonObject>();
			CompileInfo->SetBoolField  (TEXT("success"),  bCompileOk);
			CompileInfo->SetNumberField(TEXT("errors"),   Results.NumErrors);
			CompileInfo->SetNumberField(TEXT("warnings"), Results.NumWarnings);
		}
		bool bSaved = false;
		if (bSave && bCompileOk)
		{
			if (UPackage* Pkg = BP->GetOutermost())
			{
				Pkg->MarkPackageDirty();
				const FString FileName = FPackageName::LongPackageNameToFilename(
					Pkg->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags     = SAVE_NoError;
				Args.Error         = GError;
				bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),   true);
		Out->SetStringField(TEXT("class"),     ClassName);
		Out->SetStringField(TEXT("node_guid"), TargetGuid.ToString(EGuidFormats::DigitsWithHyphens));
		if (CompileInfo.IsValid()) Out->SetObjectField(TEXT("compile"), CompileInfo);
		Out->SetBoolField(TEXT("saved"), bSaved);
		Callback(JsonOk(SerializeJson(Out)));
	});

	return true;
}

// ============================================================================
// Blueprint lint ( /bp/lint and /bp/lint_project )
// ============================================================================

namespace NGGBpPriv
{
	/** One issue entry in a lint report. */
	struct FBpLintIssue
	{
		FString  Severity;   // "error" | "warning" | "info"
		FString  Kind;       // check name (e.g. "duplicate_event")
		FString  GraphName;
		FGuid    NodeGuid;   // may be invalid
		FString  Detail;
		bool     bFixable = false;
		bool     bFixed   = false;
		FString  Action;     // populated when bFixed
	};

	static bool ReferencesStaleClassName(const FString& N)
	{
		return N.Contains(TEXT("REINST_"),     ESearchCase::CaseSensitive)
			|| N.Contains(TEXT("TRASHCLASS_"), ESearchCase::CaseSensitive)
			|| N.Contains(TEXT("SKEL_"),       ESearchCase::CaseSensitive);
	}

	/** Run every lint check on BP. If bAutoFix, mutate BP in-place and record actions. */
	static void RunBlueprintLint(UBlueprint* BP, bool bAutoFix, TArray<FBpLintIssue>& OutIssues)
	{
		if (!BP) return;

		UClass* SelfScope = BP->GeneratedClass ? BP->GeneratedClass.Get()
			: (BP->SkeletonGeneratedClass ? BP->SkeletonGeneratedClass.Get() : nullptr);

		TArray<UEdGraph*> Graphs;
		Graphs.Append(BP->UbergraphPages);
		Graphs.Append(BP->FunctionGraphs);
		// Intentionally skip MacroGraphs per spec.

		// --- Check: duplicate_event (delegates to AutoHealDuplicateEvents when fixing) ----
		{
			// Walk graphs independently so we can attribute issues to a graph name.
			for (UEdGraph* G : Graphs)
			{
				if (!G) continue;
				TMap<FName, TArray<UK2Node_Event*>> Buckets;
				for (UEdGraphNode* N : G->Nodes)
				{
					UK2Node_Event* Evt = Cast<UK2Node_Event>(N);
					if (!Evt
						|| Evt->IsA<UK2Node_ComponentBoundEvent>()
						|| Evt->IsA<UK2Node_CustomEvent>()) continue;
					const FName MemberName = Evt->EventReference.GetMemberName();
					if (MemberName.IsNone()) continue;
					Buckets.FindOrAdd(MemberName).Add(Evt);
				}
				for (auto& KV : Buckets)
				{
					if (KV.Value.Num() < 2) continue;
					// Report each duplicate pair as an issue against the "loser" guid (non-winner).
					auto ExecLinkCount = [](UK2Node_Event* E) -> int32
					{
						int32 C = 0;
						for (UEdGraphPin* P : E->Pins)
						{
							if (P && P->Direction == EGPD_Output &&
								P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
							{
								C += P->LinkedTo.Num();
							}
						}
						return C;
					};
					TArray<UK2Node_Event*> Sorted = KV.Value;
					Sorted.Sort([&](UK2Node_Event& A, UK2Node_Event& B)
						{ return ExecLinkCount(&A) > ExecLinkCount(&B); });
					for (int32 i = 1; i < Sorted.Num(); ++i)
					{
						UK2Node_Event* Loser = Sorted[i];
						if (!Loser) continue;
						FBpLintIssue Issue;
						Issue.Severity = TEXT("error");
						Issue.Kind     = TEXT("duplicate_event");
						Issue.GraphName= G->GetName();
						Issue.NodeGuid = Loser->NodeGuid;
						Issue.Detail   = FString::Printf(
							TEXT("duplicate event '%s' in graph '%s' (%d total); empty duplicate can be auto-removed"),
							*KV.Key.ToString(), *G->GetName(), KV.Value.Num());
						Issue.bFixable = true;
						UE_LOG(LogNGGBridge, Log, TEXT("lint duplicate_event: %s guid=%s detail=%s"),
							*BP->GetPathName(), *Issue.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *Issue.Detail);
						OutIssues.Add(Issue);
					}
				}
			}

			if (bAutoFix)
			{
				const int32 Removed = AutoHealDuplicateEvents(BP);
				if (Removed > 0)
				{
					// Mark the first N still-fixable duplicate_event issues as fixed.
					int32 Remaining = Removed;
					for (FBpLintIssue& I : OutIssues)
					{
						if (Remaining <= 0) break;
						if (I.Kind == TEXT("duplicate_event") && I.bFixable && !I.bFixed)
						{
							I.bFixed = true;
							I.Action = TEXT("deleted empty duplicate event (auto-heal)");
							UE_LOG(LogNGGBridge, Display, TEXT("lint fix duplicate_event: %s %s"),
								*BP->GetPathName(), *I.Action);
							--Remaining;
						}
					}
				}
			}
		}

		// --- Remaining checks: iterate nodes once per graph -----------------
		for (UEdGraph* G : Graphs)
		{
			if (!G) continue;
			const FString GName = G->GetName();

			// Collect nodes up-front — we may mutate (ReconstructNode / RemovePin)
			TArray<UEdGraphNode*> Nodes = G->Nodes;
			for (UEdGraphNode* N : Nodes)
			{
				if (!N) continue;

				// compiler_error_flag
				if (N->bHasCompilerMessage && N->ErrorType == 1 /*EMessageSeverity::Error*/)
				{
					FBpLintIssue I;
					I.Severity = TEXT("error");
					I.Kind     = TEXT("compiler_error_flag");
					I.GraphName= GName;
					I.NodeGuid = N->NodeGuid;
					I.Detail   = FString::Printf(TEXT("node '%s' has compiler error: %s"),
						*N->GetClass()->GetName(), *N->ErrorMsg);
					I.bFixable = false;
					UE_LOG(LogNGGBridge, Log, TEXT("lint compiler_error_flag: guid=%s %s"),
						*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *I.Detail);
					OutIssues.Add(I);
				}

				// orphaned_pins
				{
					TArray<UEdGraphPin*> Orphans;
					for (UEdGraphPin* P : N->Pins)
					{
						if (P && P->bOrphanedPin) Orphans.Add(P);
					}
					if (Orphans.Num() > 0)
					{
						FBpLintIssue I;
						I.Severity = TEXT("warning");
						I.Kind     = TEXT("orphaned_pins");
						I.GraphName= GName;
						I.NodeGuid = N->NodeGuid;
						I.Detail   = FString::Printf(TEXT("node '%s' has %d orphaned pin(s)"),
							*N->GetClass()->GetName(), Orphans.Num());
						I.bFixable = true;
						UE_LOG(LogNGGBridge, Log, TEXT("lint orphaned_pins: guid=%s count=%d"),
							*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), Orphans.Num());

						if (bAutoFix)
						{
							for (UEdGraphPin* P : Orphans) { N->RemovePin(P); }
							I.bFixed = true;
							I.Action = FString::Printf(TEXT("removed %d orphaned pin(s)"), Orphans.Num());
							UE_LOG(LogNGGBridge, Display, TEXT("lint fix orphaned_pins: guid=%s %s"),
								*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *I.Action);
						}
						OutIssues.Add(I);
					}
				}

				// stale_function_ref
				if (UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(N))
				{
					UFunction* Resolved = SelfScope
						? CF->FunctionReference.ResolveMember<UFunction>(SelfScope)
						: nullptr;
					if (!Resolved)
					{
						FBpLintIssue I;
						I.Severity = TEXT("error");
						I.Kind     = TEXT("stale_function_ref");
						I.GraphName= GName;
						I.NodeGuid = N->NodeGuid;
						UClass* ParentCls = CF->FunctionReference.GetMemberParentClass();
						I.Detail = FString::Printf(
							TEXT("CallFunction '%s' on class '%s' could not be resolved"),
							*CF->FunctionReference.GetMemberName().ToString(),
							ParentCls ? *ParentCls->GetName() : TEXT("<null>"));
						I.bFixable = false;
						UE_LOG(LogNGGBridge, Log, TEXT("lint stale_function_ref: guid=%s %s"),
							*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *I.Detail);
						OutIssues.Add(I);
					}
				}

				// stale_variable_ref
				if (UK2Node_Variable* VN = Cast<UK2Node_Variable>(N))
				{
					FProperty* Resolved = SelfScope
						? VN->VariableReference.ResolveMember<FProperty>(SelfScope)
						: nullptr;
					if (!Resolved)
					{
						FBpLintIssue I;
						I.Severity = TEXT("error");
						I.Kind     = TEXT("stale_variable_ref");
						I.GraphName= GName;
						I.NodeGuid = N->NodeGuid;
						UClass* ParentCls = VN->VariableReference.GetMemberParentClass();
						I.Detail = FString::Printf(
							TEXT("Variable '%s' on class '%s' could not be resolved"),
							*VN->VariableReference.GetMemberName().ToString(),
							ParentCls ? *ParentCls->GetName() : TEXT("<null>"));
						I.bFixable = false;
						UE_LOG(LogNGGBridge, Log, TEXT("lint stale_variable_ref: guid=%s %s"),
							*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *I.Detail);
						OutIssues.Add(I);
					}
				}

				// reinst_class_ref — scan pin subcategory objects & (for call-funcs) parent class
				{
					bool bReinst = false;
					FString Offender;
					if (UK2Node_CallFunction* CF2 = Cast<UK2Node_CallFunction>(N))
					{
						if (UClass* ParentCls = CF2->FunctionReference.GetMemberParentClass())
						{
							if (ReferencesStaleClassName(ParentCls->GetName()))
							{
								bReinst = true; Offender = ParentCls->GetName();
							}
						}
					}
					if (!bReinst)
					{
						for (UEdGraphPin* P : N->Pins)
						{
							if (!P) continue;
							if (UObject* Sub = P->PinType.PinSubCategoryObject.Get())
							{
								if (ReferencesStaleClassName(Sub->GetName()))
								{
									bReinst = true; Offender = Sub->GetName(); break;
								}
							}
						}
					}
					if (bReinst)
					{
						FBpLintIssue I;
						I.Severity = TEXT("warning");
						I.Kind     = TEXT("reinst_class_ref");
						I.GraphName= GName;
						I.NodeGuid = N->NodeGuid;
						I.Detail   = FString::Printf(TEXT("node references stale class '%s'"), *Offender);
						I.bFixable = true;
						UE_LOG(LogNGGBridge, Log, TEXT("lint reinst_class_ref: guid=%s offender=%s"),
							*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *Offender);

						if (bAutoFix)
						{
							if (UK2Node* K2 = Cast<UK2Node>(N))
							{
								K2->ReconstructNode();
							}
							else
							{
								N->ReconstructNode();
							}
							I.bFixed = true;
							I.Action = TEXT("called ReconstructNode() to refresh stale reference");
							UE_LOG(LogNGGBridge, Display, TEXT("lint fix reinst_class_ref: guid=%s"),
								*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						}
						OutIssues.Add(I);
					}
				}

				// bound_event_missing_component
				if (UK2Node_ComponentBoundEvent* CBE = Cast<UK2Node_ComponentBoundEvent>(N))
				{
					const FName CompName = CBE->ComponentPropertyName;
					FProperty* CompProp = nullptr;
					if (SelfScope && !CompName.IsNone())
					{
						CompProp = FindFProperty<FProperty>(SelfScope, CompName);
					}
					if (!CompProp)
					{
						FBpLintIssue I;
						I.Severity = TEXT("error");
						I.Kind     = TEXT("bound_event_missing_component");
						I.GraphName= GName;
						I.NodeGuid = N->NodeGuid;
						I.Detail   = FString::Printf(
							TEXT("ComponentBoundEvent references missing component '%s'"),
							*CompName.ToString());
						I.bFixable = false;
						UE_LOG(LogNGGBridge, Log, TEXT("lint bound_event_missing_component: guid=%s %s"),
							*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), *I.Detail);
						OutIssues.Add(I);
					}
				}

				// disconnected_exec_input (info) — non-event, non-entry-point nodes
				// with an input exec pin that has zero connections.
				{
					const bool bIsEvent   = N->IsA<UK2Node_Event>();
					const bool bIsEntry   = N->GetClass()->GetName().Contains(TEXT("FunctionEntry"))
										 || N->GetClass()->GetName().Contains(TEXT("Tunnel"));
					if (!bIsEvent && !bIsEntry)
					{
						for (UEdGraphPin* P : N->Pins)
						{
							if (P && !P->bOrphanedPin
								&& P->Direction == EGPD_Input
								&& P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
								&& P->LinkedTo.Num() == 0)
							{
								FBpLintIssue I;
								I.Severity = TEXT("info");
								I.Kind     = TEXT("disconnected_exec_input");
								I.GraphName= GName;
								I.NodeGuid = N->NodeGuid;
								I.Detail   = FString::Printf(
									TEXT("node '%s' has disconnected input exec pin '%s'"),
									*N->GetClass()->GetName(), *P->PinName.ToString());
								I.bFixable = false;
								UE_LOG(LogNGGBridge, Log, TEXT("lint disconnected_exec_input: guid=%s"),
									*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
								OutIssues.Add(I);
								break; // one per node is enough
							}
						}
					}
				}

				// empty_override_event — singleton override events with no outgoing exec
				if (UK2Node_Event* EvtNode = Cast<UK2Node_Event>(N))
				{
					if (EvtNode->bOverrideFunction && !EvtNode->IsA<UK2Node_ComponentBoundEvent>())
					{
						int32 OutExec = 0;
						for (UEdGraphPin* P : EvtNode->Pins)
						{
							if (P && P->Direction == EGPD_Output &&
								P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
							{
								OutExec += P->LinkedTo.Num();
							}
						}
						if (OutExec == 0)
						{
							// Skip if this event was already reported as a duplicate — avoid double-report.
							bool bAlreadyDuplicate = false;
							for (const FBpLintIssue& Prev : OutIssues)
							{
								if (Prev.Kind == TEXT("duplicate_event") && Prev.NodeGuid == EvtNode->NodeGuid)
								{
									bAlreadyDuplicate = true; break;
								}
							}
							if (!bAlreadyDuplicate)
							{
								FBpLintIssue I;
								I.Severity = TEXT("info");
								I.Kind     = TEXT("empty_override_event");
								I.GraphName= GName;
								I.NodeGuid = EvtNode->NodeGuid;
								I.Detail   = FString::Printf(
									TEXT("override event '%s' has no outgoing exec connections"),
									*EvtNode->EventReference.GetMemberName().ToString());
								I.bFixable = false;
								UE_LOG(LogNGGBridge, Log, TEXT("lint empty_override_event: guid=%s"),
									*I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
								OutIssues.Add(I);
							}
						}
					}
				}
			}
		}
	}

	/** Serialize a single issue to JSON. */
	static TSharedPtr<FJsonObject> IssueToJson(const FBpLintIssue& I)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("severity"), I.Severity);
		O->SetStringField(TEXT("kind"),     I.Kind);
		O->SetStringField(TEXT("graph"),    I.GraphName);
		if (I.NodeGuid.IsValid())
		{
			O->SetStringField(TEXT("node_guid"),
				I.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
		else
		{
			O->SetField(TEXT("node_guid"), MakeShared<FJsonValueNull>());
		}
		O->SetStringField(TEXT("detail"),   I.Detail);
		O->SetBoolField  (TEXT("fixable"),  I.bFixable);
		O->SetBoolField  (TEXT("fixed"),    I.bFixed);
		if (!I.Action.IsEmpty())
		{
			O->SetStringField(TEXT("action"), I.Action);
		}
		return O;
	}

	/**
	 * Run lint on a single BP and produce a complete per-BP report (issues,
	 * summary, optionally compile + saved). Does NOT populate "blueprint" field
	 * — caller adds that for top-level endpoint responses only.
	 */
	static TSharedRef<FJsonObject> LintBlueprintToJson(
		UBlueprint* BP, bool bAutoFix, bool bCompile, bool bSave)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<FBpLintIssue> Issues;
		RunBlueprintLint(BP, bAutoFix, Issues);

		// Issues array
		TArray<TSharedPtr<FJsonValue>> IssueArr;
		int32 NumErrors = 0, NumWarnings = 0, NumInfo = 0, NumFixed = 0;
		for (const FBpLintIssue& I : Issues)
		{
			IssueArr.Add(MakeShared<FJsonValueObject>(IssueToJson(I)));
			if      (I.Severity == TEXT("error"))   ++NumErrors;
			else if (I.Severity == TEXT("warning")) ++NumWarnings;
			else                                    ++NumInfo;
			if (I.bFixed) ++NumFixed;
		}
		Out->SetArrayField(TEXT("issues"), IssueArr);

		TSharedPtr<FJsonObject> Sum = MakeShared<FJsonObject>();
		Sum->SetNumberField(TEXT("total"),    Issues.Num());
		Sum->SetNumberField(TEXT("errors"),   NumErrors);
		Sum->SetNumberField(TEXT("warnings"), NumWarnings);
		Sum->SetNumberField(TEXT("info"),     NumInfo);
		Sum->SetNumberField(TEXT("fixed"),    NumFixed);
		Out->SetObjectField(TEXT("summary"), Sum);

		// Post-fix: mark structurally modified, optionally compile + save.
		bool bCompileOk = true;
		bool bSaved     = false;
		if (bAutoFix && NumFixed > 0 && BP)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

			if (bCompile)
			{
				FCompilerResultsLog Results;
				Results.bSilentMode = true;
				FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
				bCompileOk = (Results.NumErrors == 0);
				TSharedPtr<FJsonObject> CompileInfo = MakeShared<FJsonObject>();
				CompileInfo->SetBoolField  (TEXT("success"),  bCompileOk);
				CompileInfo->SetNumberField(TEXT("errors"),   Results.NumErrors);
				CompileInfo->SetNumberField(TEXT("warnings"), Results.NumWarnings);
				Out->SetObjectField(TEXT("compile"), CompileInfo);

				if (bSave && bCompileOk)
				{
					if (UPackage* Pkg = BP->GetOutermost())
					{
						Pkg->MarkPackageDirty();
						const FString FileName = FPackageName::LongPackageNameToFilename(
							Pkg->GetName(), FPackageName::GetAssetPackageExtension());
						FSavePackageArgs Args;
						Args.TopLevelFlags = RF_Public | RF_Standalone;
						Args.SaveFlags     = SAVE_NoError;
						Args.Error         = GError;
						bSaved = UPackage::SavePackage(Pkg, nullptr, *FileName, Args);
					}
					Out->SetBoolField(TEXT("saved"), bSaved);
				}
			}
		}

		Out->SetBoolField(TEXT("success"), true);
		return Out;
	}

	/**
	 * Lint-only, auto-fix sub-report for attachment under "lint" in write-handler
	 * responses. No compile, no save — the calling handler owns those.
	 */
	TSharedRef<FJsonObject> BuildLintReport(UBlueprint* BP, bool bAutoFix)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<FBpLintIssue> Issues;
		if (BP) RunBlueprintLint(BP, bAutoFix, Issues);

		TArray<TSharedPtr<FJsonValue>> IssueArr;
		int32 NumErrors = 0, NumWarnings = 0, NumInfo = 0, NumFixed = 0, NumUnfixedBad = 0;
		for (const FBpLintIssue& I : Issues)
		{
			IssueArr.Add(MakeShared<FJsonValueObject>(IssueToJson(I)));
			const bool bIsError   = (I.Severity == TEXT("error"));
			const bool bIsWarning = (I.Severity == TEXT("warning"));
			if      (bIsError)   ++NumErrors;
			else if (bIsWarning) ++NumWarnings;
			else                 ++NumInfo;
			if (I.bFixed) ++NumFixed;
			else if (bIsError || bIsWarning) ++NumUnfixedBad;
		}
		Out->SetArrayField(TEXT("issues"), IssueArr);

		TSharedPtr<FJsonObject> Sum = MakeShared<FJsonObject>();
		Sum->SetNumberField(TEXT("total"),    Issues.Num());
		Sum->SetNumberField(TEXT("errors"),   NumErrors);
		Sum->SetNumberField(TEXT("warnings"), NumWarnings);
		Sum->SetNumberField(TEXT("info"),     NumInfo);
		Sum->SetNumberField(TEXT("fixed"),    NumFixed);
		Out->SetObjectField(TEXT("summary"), Sum);

		// "clean" = no unfixed error/warning issues remain. Info-level is tolerated.
		Out->SetBoolField(TEXT("clean"), NumUnfixedBad == 0);
		return Out;
	}
} // namespace NGGBpPriv

// ============================================================================
// Handler: POST /bp/lint
// Body: { blueprint, auto_fix?:false, compile?:true, save?:true }
// ============================================================================

bool FNGGHttpServer::HandleBpLint(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, FString::Printf(TEXT("invalid json: %s"), *ParseErr)));
		return true;
	}

	FString BpPath; Body->TryGetStringField(TEXT("blueprint"), BpPath);
	bool bAutoFix = false;  Body->TryGetBoolField(TEXT("auto_fix"), bAutoFix);
	bool bCompile = true;   Body->TryGetBoolField(TEXT("compile"),  bCompile);
	bool bSave    = true;   Body->TryGetBoolField(TEXT("save"),     bSave);

	if (BpPath.IsEmpty())
	{
		Callback(JsonError(400, TEXT("blueprint is required")));
		return true;
	}

	AsyncTask(ENamedThreads::GameThread,
		[Callback, BpPath, bAutoFix, bCompile, bSave]()
	{
		UBlueprint* BP = LoadBlueprintFlexible(BpPath);
		if (!BP)
		{
			Callback(JsonError(404, FString::Printf(TEXT("Blueprint not found: %s"), *BpPath)));
			return;
		}
		TSharedRef<FJsonObject> Out =
			NGGBpPriv::LintBlueprintToJson(BP, bAutoFix, bCompile, bSave);
		Out->SetStringField(TEXT("blueprint"), BpPath);

		UE_LOG(LogNGGBridge, Log, TEXT("/bp/lint: %s auto_fix=%s total=%d fixed=%d"),
			*BpPath,
			bAutoFix ? TEXT("yes") : TEXT("no"),
			(int32)Out->GetObjectField(TEXT("summary"))->GetNumberField(TEXT("total")),
			(int32)Out->GetObjectField(TEXT("summary"))->GetNumberField(TEXT("fixed")));

		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}

// ============================================================================
// Handler: POST /bp/lint_project
// Body: { path_prefix?:"/Game", auto_fix?:false, compile?:true, save?:true }
// Iterates every /Game/** BP_* asset under path_prefix and lints each.
// ============================================================================

bool FNGGHttpServer::HandleBpLintProject(
	const FHttpServerRequest& Req, const FHttpResultCallback& OnComplete)
{
	FHttpResultCallback Callback = OnComplete;
	TSharedPtr<FJsonObject> Body; FString ParseErr;
	if (!ParseJsonBody(Req, Body, ParseErr) || !Body.IsValid())
	{
		Callback(JsonError(400, FString::Printf(TEXT("invalid json: %s"), *ParseErr)));
		return true;
	}

	FString PathPrefix = TEXT("/Game");
	Body->TryGetStringField(TEXT("path_prefix"), PathPrefix);
	bool bAutoFix = false;  Body->TryGetBoolField(TEXT("auto_fix"), bAutoFix);
	bool bCompile = true;   Body->TryGetBoolField(TEXT("compile"),  bCompile);
	bool bSave    = true;   Body->TryGetBoolField(TEXT("save"),     bSave);

	AsyncTask(ENamedThreads::GameThread,
		[Callback, PathPrefix, bAutoFix, bCompile, bSave]()
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry")).Get();

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(*PathPrefix));
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		TSharedPtr<FJsonObject> ResultsObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> SkippedArr;
		int32 NumLinted = 0, TotalIssues = 0, TotalFixed = 0;

		for (const FAssetData& AD : Assets)
		{
			const FString AssetName = AD.AssetName.ToString();
			// Only BP_* assets per spec
			if (!AssetName.StartsWith(TEXT("BP_"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			// Skip cooked assets (no source available for mutation)
			if (AD.PackageFlags & PKG_FilterEditorOnly)
			{
				TSharedPtr<FJsonObject> Sk = MakeShared<FJsonObject>();
				Sk->SetStringField(TEXT("path"),   AD.GetObjectPathString());
				Sk->SetStringField(TEXT("reason"), TEXT("cooked asset"));
				SkippedArr.Add(MakeShared<FJsonValueObject>(Sk));
				continue;
			}

			const FString ObjPath = AD.GetObjectPathString();
			UBlueprint* BP = LoadBlueprintFlexible(ObjPath);
			if (!BP)
			{
				TSharedPtr<FJsonObject> Sk = MakeShared<FJsonObject>();
				Sk->SetStringField(TEXT("path"),   ObjPath);
				Sk->SetStringField(TEXT("reason"), TEXT("failed to load"));
				SkippedArr.Add(MakeShared<FJsonValueObject>(Sk));
				UE_LOG(LogNGGBridge, Warning, TEXT("/bp/lint_project: failed to load %s"), *ObjPath);
				continue;
			}

			TSharedRef<FJsonObject> Report =
				NGGBpPriv::LintBlueprintToJson(BP, bAutoFix, bCompile, bSave);
			Report->SetStringField(TEXT("blueprint"), ObjPath);
			ResultsObj->SetObjectField(ObjPath, Report);
			++NumLinted;

			if (Report->HasTypedField<EJson::Object>(TEXT("summary")))
			{
				TSharedPtr<FJsonObject> S = Report->GetObjectField(TEXT("summary"));
				TotalIssues += (int32)S->GetNumberField(TEXT("total"));
				TotalFixed  += (int32)S->GetNumberField(TEXT("fixed"));
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField  (TEXT("success"),      true);
		Out->SetStringField(TEXT("path_prefix"),  PathPrefix);
		Out->SetNumberField(TEXT("linted_count"), NumLinted);
		Out->SetObjectField(TEXT("results"),      ResultsObj);
		Out->SetArrayField (TEXT("skipped"),      SkippedArr);

		TSharedPtr<FJsonObject> Sum = MakeShared<FJsonObject>();
		Sum->SetNumberField(TEXT("blueprints"),    NumLinted);
		Sum->SetNumberField(TEXT("total_issues"),  TotalIssues);
		Sum->SetNumberField(TEXT("total_fixed"),   TotalFixed);
		Sum->SetNumberField(TEXT("skipped"),       SkippedArr.Num());
		Out->SetObjectField(TEXT("summary"), Sum);

		UE_LOG(LogNGGBridge, Log,
			TEXT("/bp/lint_project: prefix=%s linted=%d issues=%d fixed=%d skipped=%d"),
			*PathPrefix, NumLinted, TotalIssues, TotalFixed, SkippedArr.Num());

		Callback(JsonOk(SerializeJson(Out)));
	});
	return true;
}
