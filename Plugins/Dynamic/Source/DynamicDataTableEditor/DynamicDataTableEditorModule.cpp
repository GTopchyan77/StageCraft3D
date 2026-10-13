// Copyright Epic Games, Inc. All Rights Reserved.

#include "DynamicDataTableEditorModule.h"

#include "AssetTypeCategories.h"
#include "DynamicDataTableEditor.h"
#include "AssetTypeActions/AssetTypeActions_DynamicDataTable.h"
#include "DynamicDatatable/DynamicDataTable/DynamicDataTable.h"
#include "HAL/Platform.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateStyleRegistry.h"
#include "Templates/Casts.h"
#include "UObject/NameTypes.h"
#include "Styling/SlateStyle.h"
#include "Interfaces/IPluginManager.h"

class IToolkitHost;

IMPLEMENT_MODULE( FDynamicDataTableEditorModule, DynamicDataTableEditor );


const FName FDynamicDataTableEditorModule::DynamicDataTableEditorAppIdentifier( TEXT( "DynamicDataTableEditorApp" ) );
DYNAMICDATATABLEEDITOR_API EAssetTypeCategories::Type GDepricatedCategory = EAssetTypeCategories::Misc;

void FDynamicDataTableEditorModule::StartupModule()
{
	MenuExtensibilityManager = MakeShareable(new FExtensibilityManager);
	ToolBarExtensibilityManager = MakeShareable(new FExtensibilityManager);
	if (!DynAA) {
		DynAA = MakeShareable(new FAssetTypeActions_DynamicDataTable());
		DynStyle.Set("ClassThumbnail.DynamicDataTable", &SlateImageBrush);
		DynStyle.Set("ClassIcon.DynamicDataTable", &SlateImageBrush2);
		if (!FModuleManager::Get().IsModuleLoaded("AssetTools")) {return;}
		FAssetToolsModule::GetModule().Get().RegisterAssetTypeActions(DynAA.ToSharedRef());
		FSlateStyleRegistry::RegisterSlateStyle(DynStyle);
		GDepricatedCategory = FAssetToolsModule::GetModule().Get().RegisterAdvancedAssetCategory(
			FName("Depricated"),
			NSLOCTEXT("DTAsset", "Depricated", "Depricated")
		);
	}
}


void FDynamicDataTableEditorModule::ShutdownModule()
{
	//if (DynAA) {
	//	if (!FModuleManager::Get().IsModuleLoaded("AssetTools")) {return;}
	//	FSlateStyleRegistry::UnRegisterSlateStyle(DynStyle);
	//	FAssetToolsModule::GetModule().Get().UnregisterAssetTypeActions(DynAA.ToSharedRef());
	//}
	MenuExtensibilityManager.Reset();
	ToolBarExtensibilityManager.Reset();
}


TSharedRef<IDynamicDataTableEditor> FDynamicDataTableEditorModule::CreateDynamicDataTableEditor(const EToolkitMode::Type Mode, const TSharedPtr< IToolkitHost >& InitToolkitHost, UDynamicDataTable* Table)
{
	return CreateStandardDynamicDataTableEditor(Mode, InitToolkitHost, Table);
}

TSharedRef<IDynamicDataTableEditor> FDynamicDataTableEditorModule::CreateStandardDynamicDataTableEditor(const EToolkitMode::Type Mode, const TSharedPtr< IToolkitHost >& InitToolkitHost, UDynamicDataTable* Table)
{
	TSharedRef< FDynamicDataTableEditor > NewDynamicDataTableEditor( new FDynamicDataTableEditor() );
	NewDynamicDataTableEditor->InitDynamicDataTableEditor( Mode, InitToolkitHost, Table );
	return NewDynamicDataTableEditor;
}

