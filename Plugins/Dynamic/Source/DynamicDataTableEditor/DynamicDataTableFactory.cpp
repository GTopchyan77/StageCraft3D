// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableFactory.h"
#include "DynamicDataTableEditorUtils.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "Editor.h"

#include "Modules/ModuleManager.h"
#include "StructViewerModule.h"
#include "StructViewerFilter.h"

#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Input/Reply.h"

#define LOCTEXT_NAMESPACE "DynamicDataTableFactory"

UDynamicDataTableFactory::UDynamicDataTableFactory(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SupportedClass = UDynamicDataTable::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

bool UDynamicDataTableFactory::ConfigureProperties()
{
	class FDynamicDataTableStructFilter : public IStructViewerFilter
	{
	public:
		virtual bool IsStructAllowed(const FStructViewerInitializationOptions& InInitOptions, const UScriptStruct* InStruct, TSharedRef<FStructViewerFilterFuncs> InFilterFuncs) override
		{
			return FDynamicDataTableEditorUtils::IsValidTableStruct(InStruct);
		}

		virtual bool IsUnloadedStructAllowed(const FStructViewerInitializationOptions& InInitOptions, const FSoftObjectPath& InStructPath, TSharedRef<FStructViewerFilterFuncs> InFilterFuncs) override
		{
			// Unloaded structs are always User Defined Structs, and User Defined Structs are always allowed
			// They will be re-validated by IsStructAllowed once loaded during the pick
			return true;
		}
	};

	class FDynamicDataTableFactoryUI : public TSharedFromThis<FDynamicDataTableFactoryUI>
	{
	public:
		FReply OnCreate()
		{
			check(ResultStruct);
			if (PickerWindow.IsValid())
			{
				PickerWindow->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		FReply OnCancel()
		{
			ResultStruct = nullptr;
			if (PickerWindow.IsValid())
			{
				PickerWindow->RequestDestroyWindow();
			}
			return FReply::Handled();
		}

		bool IsStructSelected() const
		{
			return ResultStruct != nullptr;
		}

		void OnPickedStruct(const UScriptStruct* ChosenStruct)
		{
			ResultStruct = ChosenStruct;
			StructPickerAnchor->SetIsOpen(false);
		}

		FText OnGetComboTextValue() const
		{
			return ResultStruct
				? FText::AsCultureInvariant(ResultStruct->GetName())
				: LOCTEXT("None", "None");
		}

		TSharedRef<SWidget> GenerateStructPicker()
		{
			FStructViewerModule& StructViewerModule = FModuleManager::LoadModuleChecked<FStructViewerModule>("StructViewer");

			// Fill in options
			FStructViewerInitializationOptions Options;
			Options.Mode = EStructViewerMode::StructPicker;
			Options.StructFilter = MakeShared<FDynamicDataTableStructFilter>();

			return
				SNew(SBox)
				.WidthOverride(330)
				[
					SNew(SVerticalBox)

					+SVerticalBox::Slot()
					.FillHeight(1.0f)
					.MaxHeight(500)
					[
						SNew(SBorder)
						.Padding(4)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						[
							StructViewerModule.CreateStructViewer(Options, FOnStructPicked::CreateSP(this, &FDynamicDataTableFactoryUI::OnPickedStruct))
						]
					]
				];
		}

		const UScriptStruct* OpenStructSelector()
		{
			FStructViewerModule& StructViewerModule = FModuleManager::LoadModuleChecked<FStructViewerModule>("StructViewer");
			ResultStruct = nullptr;

			// Fill in options
			FStructViewerInitializationOptions Options;
			Options.Mode = EStructViewerMode::StructPicker;
			Options.StructFilter = MakeShared<FDynamicDataTableStructFilter>();

			PickerWindow = SNew(SWindow)
				.Title(LOCTEXT("DynamicDataTableFactoryOptions", "Pick Row Structure"))
				.ClientSize(FVector2D(350, 100))
				.SupportsMinimize(false)
				.SupportsMaximize(false)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Menu.Background"))
					.Padding(10)
					[
						SNew(SVerticalBox)
						+SVerticalBox::Slot()
						.AutoHeight()
						[
							SAssignNew(StructPickerAnchor, SComboButton)
							.ContentPadding(FMargin(2,2,2,1))
							.MenuPlacement(MenuPlacement_BelowAnchor)
							.ButtonContent()
							[
								SNew(STextBlock)
								.Text(this, &FDynamicDataTableFactoryUI::OnGetComboTextValue)
							]
							.OnGetMenuContent(this, &FDynamicDataTableFactoryUI::GenerateStructPicker)
						]
						+SVerticalBox::Slot()
						.HAlign(HAlign_Right)
						.AutoHeight()
						[
							SNew(SHorizontalBox)
							+SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(LOCTEXT("OK", "OK"))
								.IsEnabled(this, &FDynamicDataTableFactoryUI::IsStructSelected)
								.OnClicked(this, &FDynamicDataTableFactoryUI::OnCreate)
							]
							+SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(LOCTEXT("Cancel", "Cancel"))
								.OnClicked(this, &FDynamicDataTableFactoryUI::OnCancel)
							]
						]
					]
				];

			GEditor->EditorAddModalWindow(PickerWindow.ToSharedRef());
			PickerWindow.Reset();

			return ResultStruct;
		}

	private:
		TSharedPtr<SWindow> PickerWindow;
		TSharedPtr<SComboButton> StructPickerAnchor;
		const UScriptStruct* ResultStruct = nullptr;
	};

	TSharedRef<FDynamicDataTableFactoryUI> StructSelector = MakeShareable(new FDynamicDataTableFactoryUI());
	Struct = StructSelector->OpenStructSelector();

	return Struct != nullptr;
}

UObject* UDynamicDataTableFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	UDynamicDataTable* DynamicDataTable = nullptr;
	if (Struct && ensure(SupportedClass == Class))
	{
		ensure(0 != (RF_Public & Flags));
		DynamicDataTable = MakeNewDynamicDataTable(InParent, Name, Flags);
		if (DynamicDataTable)
		{
			DynamicDataTable->RowStruct = const_cast<UScriptStruct*>(ToRawPtr(Struct));
		}
	}
	return DynamicDataTable;
}

UDynamicDataTable* UDynamicDataTableFactory::MakeNewDynamicDataTable(UObject* InParent, FName Name, EObjectFlags Flags)
{
	return NewObject<UDynamicDataTable>(InParent, Name, Flags);
}

#undef LOCTEXT_NAMESPACE // "DynamicDataTableFactory"
