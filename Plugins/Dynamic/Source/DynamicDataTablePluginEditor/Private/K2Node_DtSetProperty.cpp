#pragma once

#include "K2Node_DtSetProperty.h"
#include "EdGraph/EdGraphPin.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "KismetCompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CallArrayFunction.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node.h"
#include "K2Node_Literal.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
//#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UnrealType.h"
#include "UObject/UnrealTypePrivate.h"
#include "DynamicDatatable/Public/DynamicDatatableBPLibrary.h"

#define LOCTEXT_NAMESPACE "K2Node_DtSetProperty"

UK2Node_DtSetProperty::UK2Node_DtSetProperty(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {
	
}


void UK2Node_DtSetProperty::AllocateDefaultPins() {
	auto* ExecPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, TEXT("Exec"));
	auto* ThenPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("Then"));
	auto* TablePin = CreatePin(EGPD_Input,       UEdGraphSchema_K2::PC_Struct, "Table");
	TablePin->PinType.PinSubCategoryObject = FDynamicTable::StaticStruct();
	TablePin->PinType.bIsReference = true;
	TablePin->bDisplayAsMutableRef = true;
	auto* RowNamePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, "RowName");
	auto* ValuePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, "Value");
	auto* ExistsPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Boolean, "Exists");

}

void UK2Node_DtSetProperty::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) {
	UEdGraphPin* TablePin = FindPin(TEXT("Table"));
	auto* RowNamePin = FindPin(TEXT("RowName"));
	UEdGraphPin* ValuePin = FindPin(TEXT("Value"));
	auto* ExistsPin = FindPin(TEXT("Exists"));
	
	const FName FunctionName = GET_FUNCTION_NAME_CHECKED(UDynamicDatatableBPLibrary, SetTableRowMember);
	UK2Node_CallFunction* CallFunc = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	CallFunc->FunctionReference.SetExternalMember(FunctionName, UDynamicDatatableBPLibrary::StaticClass());
	CallFunc->AllocateDefaultPins();
		
	auto* ExecPin = FindPin(TEXT("Exec"));
	auto* ThenPin = FindPin(TEXT("Then"));
	UEdGraphPin* FuncExecPin = CallFunc->GetExecPin();
	UEdGraphPin* FuncThenPin = CallFunc->GetThenPin();
	CompilerContext.MovePinLinksToIntermediate(*ThenPin, *FuncThenPin);
	CompilerContext.MovePinLinksToIntermediate(*ExecPin, *FuncExecPin);
	auto* FuncTablePin = CallFunc->FindPin(TEXT("Table"));
	auto* FuncRowNamePin = CallFunc->FindPin(TEXT("RowName"));
	auto* FuncValuePin = CallFunc->FindPin(TEXT("Value"));
	auto* FuncExistsPin = CallFunc->GetReturnValuePin();
	CompilerContext.MovePinLinksToIntermediate(*TablePin,*FuncTablePin);
	CompilerContext.MovePinLinksToIntermediate(*RowNamePin,*FuncRowNamePin);
	CompilerContext.MovePinLinksToIntermediate(*ExistsPin,*FuncExistsPin);

	CallFunc->FindPin(TEXT("MemberName"))->DefaultValue = MemberName;
	
	FuncValuePin->PinType = ValuePin->PinType;
	CompilerContext.MovePinLinksToIntermediate(*ValuePin,*FuncValuePin);
}

FText UK2Node_DtSetProperty::GetTooltipText() const {
    return LOCTEXT("GetMember_ToolTip", "Sets a member value of the row.");
}

void UK2Node_DtSetProperty::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const {
	Super::GetMenuActions(ActionRegistrar);
	auto PostSpawnSetupLambda = [](UEdGraphNode* NewNode, bool bIsTemplateNode) {};
	UClass* Action = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(Action)) {
		UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(GetClass());
		Spawner->DefaultMenuSignature.Keywords = LOCTEXT("Keywords", "Value");
		Spawner->DefaultMenuSignature.MenuName = FText::FromString("");
		Spawner->CustomizeNodeDelegate = UBlueprintNodeSpawner::FCustomizeNodeDelegate::CreateLambda(PostSpawnSetupLambda);
		check(Spawner != nullptr);
		ActionRegistrar.AddBlueprintAction(Action, Spawner);
	}
}

FSlateIcon UK2Node_DtSetProperty::GetIconAndTint(FLinearColor& OutColor) const {
    static FSlateIcon Icon("EditorStyle", "GraphEditor.StructGlyph");
    return Icon;
}

#if WITH_EDITOR
void UK2Node_DtSetProperty::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) {
	if (StructName.Len()>0) {
		StructType = FindFirstObjectSafe<UScriptStruct>(*StructName);
	}
	RefreshPins();
}

FString UK2Node_DtSetProperty::GetPinMetaData(FName InPinName, FName InKey) {
	if (UScriptStruct* Struct = StructType) {
		if (FProperty* Prop = Struct->FindPropertyByName(InPinName)) {
			const FString Value = Prop->GetMetaData(InKey);
			if (!Value.IsEmpty()) {
				return Value;
			}
		}
	}
	return "";
}

bool UK2Node_DtSetProperty::HasExternalDependencies(TArray<UStruct*>* OptionalOutput) const {
	if (UScriptStruct* Struct = StructType) {
		if (!Struct->IsNative()) {
			if (OptionalOutput) {
				OptionalOutput->AddUnique(Struct);
			}
			return true;
		}
	}
	return false;
}

void UK2Node_DtSetProperty::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const {
	
}

FString UK2Node_DtSetProperty::GetFindReferenceSearchString_Impl(EGetFindReferenceSearchStringFlags InFlags) const {
	FString SearchString = "";
	if (const UScriptStruct* Struct = StructType) {
		SearchString += TEXT(" ");
		SearchString += Struct->GetName();
	}

	return SearchString;
}

void UK2Node_DtSetProperty::PreEditChange(FProperty* PropertyAboutToChange) {
	
}

class FNodeHandlingFunctor* UK2Node_DtSetProperty::CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const {
	return nullptr;
}

#endif

void UK2Node_DtSetProperty::RefreshPins() {
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* TablePin   = FindPin(TEXT("Table"));
	auto* RowNamePin = FindPin(TEXT("RowName"));
	UEdGraphPin* ValuePin = FindPin(TEXT("Value"));
	auto* ExistsPin = FindPin(TEXT("Exists"));
	
	ValuePin->PinFriendlyName = FText::FromString((MemberName=="") ? "Value" : MemberName);
	ValuePin->bAllowFriendlyName = true;
	FEdGraphPinType Wildcard;
	Wildcard.PinCategory = Schema->PC_Wildcard;
	Wildcard.ContainerType = EPinContainerType::None;

	if (UScriptStruct* ValueStruct = StructType) {
		StructType = ValueStruct;
		FProperty* MemberProp = nullptr;
		if (MemberName=="") {
			for (TFieldIterator<FProperty> It(ValueStruct, EFieldIteratorFlags::IncludeSuper); It; ++It) {
				FProperty* Prop = *It;
				if (Prop) {
					MemberName = Prop->GetAuthoredName().TrimStartAndEnd();
					break;
				}
			}
		}
		for (TFieldIterator<FProperty> It(ValueStruct, EFieldIteratorFlags::IncludeSuper); It; ++It) {
			FProperty* Prop = *It;
			if (Prop) {
				if (Prop->GetAuthoredName().TrimStartAndEnd()==MemberName) {
					MemberProp = Prop;
					break;
				}
			}
		}
		if (MemberProp) {
			Schema->ConvertPropertyToPinType(MemberProp, ValuePin->PinType);
		}
	}
	else {
		StructType = nullptr;
	}
}

void UK2Node_DtSetProperty::NotifyPinConnectionListChanged(UEdGraphPin* Pin) {
	Super::NotifyPinConnectionListChanged(Pin);
	RefreshPins();
}

void UK2Node_DtSetProperty::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) {
	Super::ReallocatePinsDuringReconstruction(OldPins);
	RefreshPins();
}

void UK2Node_DtSetProperty::PostReconstructNode() {
	UEdGraphPin* TablePin   = FindPin(TEXT("Table"));
	UEdGraphPin* ValuePin = FindPin(TEXT("Value"));

	PinConnectionListChanged(TablePin);
	PinConnectionListChanged(ValuePin);

	Super::PostReconstructNode();
}


#undef LOCTEXT_NAMESPACE
