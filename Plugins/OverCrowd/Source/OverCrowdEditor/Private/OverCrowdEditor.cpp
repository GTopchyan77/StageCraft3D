// This file is Copyright (C) 2025 Overcrowd Development Engineering Ltd, all rights reserved

#include "OverCrowdEditor.h"
#include "OverCrowdEditorStyle.h"
#include "OverCrowdEditorCommands.h"
#include "Misc/MessageDialog.h"
#include "ToolMenus.h"
#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidgetBlueprint.h"


static const FName OverCrowdEditorTabName("OverCrowdEditor");

#define LOCTEXT_NAMESPACE "FOverCrowdEditorModule"


DEFINE_LOG_CATEGORY(LogOverCrowdEditor);

void FOverCrowdEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FOverCrowdEditorStyle::Initialize();
	FOverCrowdEditorStyle::ReloadTextures();

	FOverCrowdEditorCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FOverCrowdEditorCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FOverCrowdEditorModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FOverCrowdEditorModule::RegisterMenus));
}

void FOverCrowdEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FOverCrowdEditorStyle::Shutdown();

	FOverCrowdEditorCommands::Unregister();
}

void FOverCrowdEditorModule::PluginButtonClicked()
{
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, L"/Script/Blutility.EditorUtilityWidgetBlueprint'/OverCrowd/Main/UI_OverCrowd.UI_OverCrowd'");

	UEditorUtilityWidgetBlueprint* EditorWidget = Cast<UEditorUtilityWidgetBlueprint>(BP);

	if (EditorWidget)
	{
		UEditorUtilitySubsystem* EditorUtilitySubsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();
		EditorUtilitySubsystem->SpawnAndRegisterTab(EditorWidget);
	}
	
	// // Put your "OnButtonClicked" stuff here
	// FText DialogText = FText::Format(
	// 						LOCTEXT("PluginButtonDialogText", "Add code to {0} in {1} to override this button's actions"),
	// 						FText::FromString(TEXT("FOverCrowdEditorModule::PluginButtonClicked()")),
	// 						FText::FromString(TEXT("OverCrowdEditor.cpp"))
	// 				   );
	// FMessageDialog::Open(EAppMsgType::Ok, DialogText);
}

void FOverCrowdEditorModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FOverCrowdEditorCommands::Get().PluginAction, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FOverCrowdEditorCommands::Get().PluginAction));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FOverCrowdEditorModule, OverCrowdEditor)