// Copyright 2025-2026 NGG. All Rights Reserved.

#include "NGGChatTabSpawner.h"
#include "SNGGChatWindow.h"
#include "SidecarLauncher.h" // LogNGGChat

#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "ToolMenus.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "NGGChat"

const FName FNGGChatTabSpawner::TabId(TEXT("NGGChatTab"));
bool FNGGChatTabSpawner::bRegistered = false;

TSharedRef<SDockTab> FNGGChatTabSpawner::SpawnChatTab(const FSpawnTabArgs& /*Args*/)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("TabLabel", "Claude Chat"))
		[
			SNew(SNGGChatWindow)
		];
}

void FNGGChatTabSpawner::Register()
{
	if (bRegistered) return;

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TabId,
		FOnSpawnTab::CreateStatic(&FNGGChatTabSpawner::SpawnChatTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Claude Chat"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Open the Claude Chat window (NGG)"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "MainFrame.VisitForums"));

	RegisterToolbarExtension();

	bRegistered = true;
	UE_LOG(LogNGGChat, Log, TEXT("NGGChatTabSpawner: registered tab '%s' and toolbar button"),
		*TabId.ToString());
}

void FNGGChatTabSpawner::Unregister()
{
	if (!bRegistered) return;

	UnregisterToolbarExtension();

	if (FGlobalTabmanager::Get()->HasTabSpawner(TabId))
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
	}

	bRegistered = false;
	UE_LOG(LogNGGChat, Log, TEXT("NGGChatTabSpawner: unregistered"));
}

void FNGGChatTabSpawner::RegisterToolbarExtension()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (!ToolMenus) return;

	UToolMenu* Toolbar = ToolMenus->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
	if (!Toolbar) return;

	FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("NGGChat"));

	FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
		TEXT("OpenNGGChat"),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			UE_LOG(LogNGGChat, Log, TEXT("Toolbar: opening Claude Chat tab"));
			FGlobalTabmanager::Get()->TryInvokeTab(FNGGChatTabSpawner::TabId);
		})),
		LOCTEXT("ToolbarLabel", "Claude"),
		LOCTEXT("ToolbarTooltip", "Open the Claude Chat window"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "MainFrame.VisitForums"));

	Section.AddEntry(Entry);
}

void FNGGChatTabSpawner::UnregisterToolbarExtension()
{
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		// Removing the entire section is sufficient — it only contains our entry.
		if (UToolMenu* Toolbar = ToolMenus->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar")))
		{
			Toolbar->RemoveSection(TEXT("NGGChat"));
		}
	}
}

#undef LOCTEXT_NAMESPACE
