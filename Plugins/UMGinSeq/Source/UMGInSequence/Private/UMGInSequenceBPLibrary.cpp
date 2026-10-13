//  Copyright WASD Studio 2025 All Rights Reserved.

#include "UMGInSequenceBPLibrary.h"
#include "UMGInSequence.h"
#include "Blueprint/UserWidget.h"

UUMGInSequenceBPLibrary::UUMGInSequenceBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

float UUMGInSequenceBPLibrary::UMGInSequenceSampleFunction(float Param)
{
	return -1;
}

void UUMGInSequenceBPLibrary::CopyWidgetUserProperties(UUserWidget* TemplateWidget, UUserWidget* TargetWidget)
{
	if (!TemplateWidget || !TargetWidget)
	{
		return;
	}

	UClass* TemplateClass = TemplateWidget->GetClass();
	UClass* TargetClass   = TargetWidget->GetClass();
	
	if (!TargetClass->IsChildOf(TemplateClass) && !TemplateClass->IsChildOf(TargetClass))
	{

		return;
	}
	for (TFieldIterator<FProperty> PropIt(TemplateClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property)
		{
			continue;
		}

		// 只拷贝可编辑字段（EditAnywhere、EditDefaultsOnly 等）
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}
		
		const UStruct* OwnerStruct = Property->GetOwnerStruct();
		const UClass*  OwnerClass  = Cast<const UClass>(OwnerStruct);

		if (!OwnerClass)
		{
			continue;
		}
		const bool bIsNativeClass = OwnerClass->HasAnyClassFlags(CLASS_Native);
		if (bIsNativeClass)
		{
			continue;
		}
		
		FProperty* TargetProp = TargetClass->FindPropertyByName(Property->GetFName());
		if (!TargetProp || TargetProp->GetClass() != Property->GetClass())
		{
			continue;
		}

		void* SrcPtr = Property->ContainerPtrToValuePtr<void>(TemplateWidget);
		void* DstPtr = TargetProp->ContainerPtrToValuePtr<void>(TargetWidget);

		if (SrcPtr && DstPtr)
		{
			TargetProp->CopyCompleteValue(DstPtr, SrcPtr);
		}
	}
}

