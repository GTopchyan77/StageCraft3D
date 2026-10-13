// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeCategories.h"
#include "IAssetTypeActions.h"
#include "Modules/ModuleInterface.h"
#include "Templates/SharedPointer.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "Styling/SlateStyle.h"
#include "Interfaces/IPluginManager.h"


DYNAMICDATATABLEEDITOR_API extern EAssetTypeCategories::Type GDepricatedCategory;

class FName;
class IDynamicDataTableEditor;
class UDynamicDataTable;

/** DynamicDataTable Editor module */
class FDynamicDataTableEditorModule : public IModuleInterface,
	public IHasMenuExtensibility, public IHasToolBarExtensibility
{

public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Creates an instance of table editor object.  Only virtual so that it can be called across the DLL boundary.
	 *
	 * @param	Mode					Mode that this editor should operate in
	 * @param	InitToolkitHost			When Mode is WorldCentric, this is the level editor instance to spawn this editor within
	 * @param	Table					The table to start editing
	 *
	 * @return	Interface to the new table editor
	 */
	virtual TSharedRef<IDynamicDataTableEditor> CreateDynamicDataTableEditor( const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UDynamicDataTable* Table );

	/** Gets the extensibility managers for outside entities to extend static mesh editor's menus and toolbars */
	virtual TSharedPtr<FExtensibilityManager> GetMenuExtensibilityManager() override {return MenuExtensibilityManager;}

	virtual TSharedPtr<FExtensibilityManager> GetToolBarExtensibilityManager() override { return ToolBarExtensibilityManager; }
	
	/** DynamicDataTable Editor app identifier string */
	static const FName DynamicDataTableEditorAppIdentifier;

private:
	TSharedRef<IDynamicDataTableEditor> CreateStandardDynamicDataTableEditor(const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UDynamicDataTable* Table);

	TSharedPtr<IAssetTypeActions> DynAA;
	UPROPERTY()
	FSlateStyleSet DynStyle = FSlateStyleSet("DynamicDataTableStyle");
	FString basedir = IPluginManager::Get().FindPlugin("DynamicDatatable")->GetBaseDir();
	FString dir1 = basedir+"/Resources/Icon128.png";
	FString dir2 = basedir+"/Resources/Icon24.png";
	UPROPERTY()
	FSlateImageBrush SlateImageBrush = FSlateImageBrush(dir1, FVector2D(128.0f,128.0f));
	UPROPERTY()
	FSlateImageBrush SlateImageBrush2 = FSlateImageBrush(dir2, FVector2D(24.0f,24.0f));
	TSharedPtr<FExtensibilityManager> MenuExtensibilityManager;
	TSharedPtr<FExtensibilityManager> ToolBarExtensibilityManager;
;

};


