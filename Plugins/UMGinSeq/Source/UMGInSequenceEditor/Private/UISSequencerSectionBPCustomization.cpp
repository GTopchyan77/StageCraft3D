//  Copyright WASD Studio 2025 All Rights Reserved.

#include "UISSequencerSectionBPCustomization.h"

#include "UISSequencerSectionBP.h"      // 你刚写的新类
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "IDetailsView.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "UISSequencerSectionBP.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UISSequencerSectionBPCustomization"

TSharedRef<IDetailCustomization> FUISSequencerSectionBPCustomization::MakeInstance()
{
    return MakeShareable(new FUISSequencerSectionBPCustomization());
}

void FUISSequencerSectionBPCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{

    TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
    DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);

    if (ObjectsBeingCustomized.Num() == 0)
    {
        return;
    }

    UUISSequencerSectionBP* TargetSection = Cast<UUISSequencerSectionBP>(ObjectsBeingCustomized[0].Get());
    if (!TargetSection)
    {
        return;
    }


    FPropertyEditorModule& PropertyModule =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

    FDetailsViewArgs DetailsViewArgs;
    DetailsViewArgs.bAllowSearch = true;
    DetailsViewArgs.bHideSelectionTip = true;
    DetailsViewArgs.bLockable = false;
    DetailsViewArgs.bUpdatesFromSelection = false;
    DetailsViewArgs.bShowOptions = true;
    DetailsViewArgs.bShowPropertyMatrixButton = false;
    DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
    DetailsViewArgs.ViewIdentifier = "UISSequencerSectionBP_WidgetDetails";

    WidgetDetailsView = PropertyModule.CreateDetailView(DetailsViewArgs);
    WidgetDetailsView->SetIsPropertyVisibleDelegate(
    FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& InPropertyAndParent)
    {
        const FProperty* Property = &InPropertyAndParent.Property;
        if (!Property)
        {
            return false;
        }
        
        const UStruct* OwnerStruct = Property->GetOwnerStruct();
        const UClass*  OwnerClass  = Cast<const UClass>(OwnerStruct);

        if (!OwnerClass)
        {
            return true;
        }
        const bool bIsNativeClass = OwnerClass->HasAnyClassFlags(CLASS_Native);

        if (bIsNativeClass)
        {
            return false;
        }
        return true;
    })
);

    IDetailCategoryBuilder& UMGCategory =
        DetailBuilder.EditCategory("UMG", LOCTEXT("UMGCategory", "UMG"), ECategoryPriority::Important);


    TSharedRef<IPropertyHandle> WidgetPropHandle =
        DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UUISSequencerSectionBP, WidgetInstance));
    //WidgetPropHandle->MarkHiddenByCustomization();
    UMGCategory.AddProperty(WidgetPropHandle);

    UMGCategory.AddCustomRow(LOCTEXT("WidgetInstanceDetailsFilter", "Widget Instance Details"))
    .WholeRowContent()
    [
        SNew(SVerticalBox)
    
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.f, 4.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("WidgetPropertiesLabel", "Widget Public Properties"))
        ]
    
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SBox)
            .MaxDesiredHeight(300.f)
            [
                WidgetDetailsView.ToSharedRef()
            ]
        ]
    ];


    if (TargetSection->WidgetInstance)
    {
        WidgetDetailsView->SetObject(TargetSection->WidgetInstance);
    }
    else
    {
        WidgetDetailsView->SetObject(nullptr);
    }
    
    WidgetPropHandle->SetOnPropertyValueChanged(
        FSimpleDelegate::CreateLambda([this, &DetailBuilder]()
        {
            TArray<TWeakObjectPtr<UObject>> Objects;
            DetailBuilder.GetObjectsBeingCustomized(Objects);

            if (Objects.Num() > 0)
            {
                if (UUISSequencerSectionBP* Section = Cast<UUISSequencerSectionBP>(Objects[0].Get()))
                {
                    if (WidgetDetailsView.IsValid())
                    {
                        WidgetDetailsView->SetObject(Section->WidgetInstance);
                    }
                }
            }
        })
    );
}

#undef LOCTEXT_NAMESPACE
