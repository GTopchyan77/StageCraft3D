#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "K2Node_StructOperation.h"
#include "DynamicDatatable/Public/DynamicDatatableBPLibrary.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_StructMemberGet.h"
#include "KismetCompiler.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/UnrealType.h"
#include "UObject/UnrealTypePrivate.h"

#include "K2Node_DtGetProperty.generated.h"


#define LOCTEXT_NAMESPACE "K2Node_DtGetProperty"

UCLASS()
class DYNAMICDATATABLEPLUGINEDITOR_API UK2Node_DtGetProperty : public UK2Node {
	GENERATED_UCLASS_BODY()

public:
	UK2Node_DtGetProperty();

	UPROPERTY(EditAnywhere, Category="Node", meta=(GetOptions="GetStructOptions"))
	FString StructName = "";
	
	UPROPERTY(EditAnywhere, Category="Node")
	bool ShowAllStructTypes = false;
	
	UPROPERTY()
	TObjectPtr<UScriptStruct> StructType;

	UPROPERTY(EditAnywhere, Category="Node", meta=(GetOptions="GetMemberOptions"))
	FString MemberName = "";

	virtual void AllocateDefaultPins() override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual class FNodeHandlingFunctor* CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const override;
#endif

	FText GetNodeTitle(ENodeTitleType::Type TitleType) const override {
		FString str = "DT Get Property";
		FText res = FText::FromString(str);
		
		return res;
	}
	FLinearColor GetNodeTitleColor() const override {
		if(const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>()) {
			FEdGraphPinType PinType;
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = StructType;
			return K2Schema->GetPinTypeColor(PinType);
		}
		return FLinearColor(0.1686f, 0.3961f, 0.4824f, 1.0f);
	}
	virtual FText GetCompactNodeTitle() const override {FString str = "DT Get Prop"; return FText::FromString(str);}
	virtual FText GetTooltipText() const override;
	virtual bool ShouldShowNodeProperties() const override { return true; }
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override; // {
	virtual bool DrawNodeAsVariable() const override {return false;}
	virtual FString GetPinMetaData(FName InPinName, FName InKey) override;
	virtual bool HasExternalDependencies(TArray<UStruct*>* OptionalOutput) const override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
	virtual FString GetFindReferenceSearchString_Impl(EGetFindReferenceSearchStringFlags InFlags) const override;
	virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override {RefreshPins();}
	
	virtual FText GetMenuCategory() const override {return FText::FromString("DynamicDatatable|Utility");}
	
	virtual bool IncludeParentNodeContextMenu() const override { return true; }
	virtual int32 GetNodeRefreshPriority() const override { return EBaseNodeRefreshPriority::Low_UsesDependentWildcard; }
	
	virtual bool IsActionFilteredOut(class FBlueprintActionFilter const& Filter) override {
		if (Filter.Context.Pins.Num()>0) {
			for (const UEdGraphPin* Pin : Filter.Context.Pins) {
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) {
					return false;
				}
			}
			return true;
		}
		return false;
	}

	virtual void ReconstructNode() override {
		Super::ReconstructNode();
		RefreshPins();
	}

	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	
	virtual bool IsNodePure() const override {return true;}

	

	void RefreshPins();

	UFUNCTION()
	TArray<FString> GetMemberOptions() const {
		TArray<FString> res = {};

		if (StructType) {
			for (TFieldIterator<FProperty> It(StructType, EFieldIteratorFlags::IncludeSuper); It; ++It) {
				res.Add(It->GetAuthoredName().TrimStartAndEnd());
			}
		}

		return res;
	}

	UFUNCTION()
	TArray<FString> GetStructOptions() const {
		TArray<FString> res = {};
		FString ProjectScriptPrefix = FString::Printf(TEXT("/Script/%s"), FApp::GetProjectName());
		for (TObjectIterator<UScriptStruct> It; It; ++It) {
			UScriptStruct* Struct = *It;
			const FString PkgName  = Struct->GetOutermost()->GetName();
			const bool bBlueprintStruct = Struct->IsA(UUserDefinedStruct::StaticClass());
			const bool bNativeInProject = PkgName.StartsWith(ProjectScriptPrefix);
			if (ShowAllStructTypes || bBlueprintStruct || bNativeInProject) {
				res.AddUnique(Struct->GetName());
			}
		}
		res.Sort();
		return res;
	}
	
	virtual void PostReconstructNode() override;
	
};

#undef LOCTEXT_NAMESPACE
