// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageWorkspaceShell.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "StageWorkspaceShell"

namespace StageWorkspaceShell
{
	/** Slate tab type of the single major tab that hosts the panel tab manager. Internal to the shell; never saved in panel layouts. */
	const FName WorkspaceTabId(TEXT("StageCraft.Workspace"));

	/** Slate's layout version keys. Bump one whenever its structure changes, so stale saved layouts are not restored. */
	const FName RootLayoutName(TEXT("StageCraft_Root_v1"));
	const FName DefaultPanelLayoutName(TEXT("StageCraft_Workspace_v1"));

	FTabId ToTabId(const FGameplayTag& PanelTag)
	{
		return FTabId(PanelTag.GetTagName());
	}

	FText GetPanelLabel(const FGameplayTag& PanelTag)
	{
		if (PanelTag == StageCraftTags::Panel_Viewport)
		{
			return LOCTEXT("ViewportLabel", "Viewport");
		}
		if (PanelTag == StageCraftTags::Panel_Inspector)
		{
			return LOCTEXT("InspectorLabel", "Inspector");
		}
		if (PanelTag == StageCraftTags::Panel_FaderBank)
		{
			return LOCTEXT("FaderBankLabel", "Faders");
		}
		return FText::FromName(PanelTag.GetTagName());
	}
}

FStageWorkspaceShell::FStageWorkspaceShell(const TSharedRef<SWindow>& InMainWindow, const TSharedRef<SViewport>& InViewportWidget, FStageWorkspaceShellCallbacks InCallbacks)
	: MainWindow(InMainWindow)
	, ViewportWidget(InViewportWidget)
	, Callbacks(MoveTemp(InCallbacks))
{
	check(Callbacks.CreatePanelContent);
	check(Callbacks.PanelHostChanged);
	check(Callbacks.SetMainWindowContent);
	check(Callbacks.RestoreMainWindowViewport);
}

TArray<FGameplayTag> FStageWorkspaceShell::GetKnownPanels()
{
	return { StageCraftTags::Panel_Viewport, StageCraftTags::Panel_Inspector, StageCraftTags::Panel_FaderBank };
}

TSharedRef<FTabManager::FLayout> FStageWorkspaceShell::MakeDefaultLayout()
{
	using namespace StageWorkspaceShell;

	// Viewport top-left, faders below it, inspector on the right: the same arrangement as the fixed HUD.
	return FTabManager::NewLayout(DefaultPanelLayoutName)
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.76f)
				->Split
				(
					FTabManager::NewStack()->SetSizeCoefficient(0.74f)
					->AddTab(ToTabId(StageCraftTags::Panel_Viewport), ETabState::OpenedTab)
				)
				->Split
				(
					FTabManager::NewStack()->SetSizeCoefficient(0.26f)
					->AddTab(ToTabId(StageCraftTags::Panel_FaderBank), ETabState::OpenedTab)
				)
			)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.24f)
				->AddTab(ToTabId(StageCraftTags::Panel_Inspector), ETabState::OpenedTab)
			)
		);
}

void FStageWorkspaceShell::Install(const TSharedRef<FTabManager::FLayout>& PanelLayout)
{
	using namespace StageWorkspaceShell;

	const TSharedPtr<SWindow> Window = MainWindow.Pin();
	if (!ensureMsgf(!bInstalled && Window.IsValid() && ViewportWidget.IsValid(), TEXT("Install called twice or without a main window / viewport.")))
	{
		return;
	}

	const TSharedRef<FGlobalTabmanager> GlobalTabManager = FGlobalTabmanager::Get();

	// Torn-off windows become native children of the main window rather than separate taskbar apps (ADR F11).
	GlobalTabManager->SetRootWindow(Window.ToSharedRef());

	// The layout files are ours (ADR §3.4); never let Slate write the editor's layout ini (empty in game builds, ADR F3).
	GlobalTabManager->SetCanSavePersistentLayouts(false);

	GlobalTabManager->RegisterNomadTabSpawner(WorkspaceTabId, FOnSpawnTab::CreateSP(this, &FStageWorkspaceShell::SpawnWorkspaceTab))
		.SetDisplayName(LOCTEXT("WorkspaceLabel", "StageCraft"));

	bInstalled = true;
	PendingPanelLayout = PanelLayout;

	const TSharedRef<FTabManager::FLayout> RootLayout = FTabManager::NewLayout(RootLayoutName)
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->Split(FTabManager::NewStack()->SetHideTabWell(true)->AddTab(WorkspaceTabId, ETabState::OpenedTab))
		);

	{
		TGuardValue<bool> RestoreGuard(bRestoringLayout, true);

		// A widget has one parent. Detach the bare viewport from the window before the viewport panel adopts it.
		Callbacks.SetMainWindowContent(SNullWidget::NullWidget);
		const TSharedPtr<SWidget> RootArea = GlobalTabManager->RestoreFrom(RootLayout, Window, false, EOutputCanBeNullptr::Never);
		Callbacks.SetMainWindowContent(RootArea.ToSharedRef());
	}
	SyncReportedHosts();
	HandleViewportMoved();
}

void FStageWorkspaceShell::Shutdown()
{
	if (!bInstalled)
	{
		return;
	}
	bInstalled = false;

	if (FSlateApplication::IsInitialized())
	{
		TGuardValue<bool> RestoreGuard(bRestoringLayout, true);
		TearDownPanels();
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(StageWorkspaceShell::WorkspaceTabId);

		// Leave the window as UGameEngine expects it: the bare viewport as its content. This also releases the root dock area.
		Callbacks.RestoreMainWindowViewport();
	}

	PanelTabManager.Reset();
	PendingPanelLayout.Reset();
	ReportedHosts.Reset();
}

void FStageWorkspaceShell::ApplyLayout(const TSharedRef<FTabManager::FLayout>& PanelLayout)
{
	const TSharedPtr<SDockTab> Tab = WorkspaceTab.Pin();
	if (!bInstalled || !Tab.IsValid())
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("ApplyLayout '%s' ignored: the workspace is not installed."), *PanelLayout->GetLayoutName().ToString());
		return;
	}

	PendingPanelLayout = PanelLayout;
	{
		TGuardValue<bool> RestoreGuard(bRestoringLayout, true);
		TearDownPanels();
		CreatePanelTabManager(Tab.ToSharedRef());
		Tab->SetContent(RestorePanels(PanelLayout, MainWindow.Pin()));
	}
	SyncReportedHosts();
	HandleViewportMoved();
}

TSharedRef<SDockTab> FStageWorkspaceShell::SpawnWorkspaceTab(const FSpawnTabArgs& Args)
{
	const TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::MajorTab)
		.Label(LOCTEXT("WorkspaceLabel", "StageCraft"))
		// The Workspace is the app's main window content. Closing the window quits the app (ADR F7).
		.OnCanCloseTab_Lambda([]() { return false; });

	WorkspaceTab = Tab;
	CreatePanelTabManager(Tab);

	const TSharedRef<FTabManager::FLayout> Layout = PendingPanelLayout.IsValid() ? PendingPanelLayout.ToSharedRef() : MakeDefaultLayout();
	Tab->SetContent(RestorePanels(Layout, MainWindow.Pin()));
	return Tab;
}

void FStageWorkspaceShell::TearDownPanels()
{
	if (!PanelTabManager.IsValid())
	{
		return;
	}

	// Collect the floating panel windows before their tabs go away.
	TSet<TSharedRef<SWindow>> FloatingWindows;
	const TSharedPtr<SWindow> Main = MainWindow.Pin();
	for (const FGameplayTag& PanelTag : GetKnownPanels())
	{
		if (const TSharedPtr<SDockTab> Tab = FindLiveTab(PanelTag))
		{
			const TSharedPtr<SWindow> TabWindow = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
			if (TabWindow.IsValid() && TabWindow != Main)
			{
				FloatingWindows.Add(TabWindow.ToSharedRef());
			}
		}
	}

	// Releasing the Workspace tab's content destroys the docked panel area and its tabs.
	if (const TSharedPtr<SDockTab> Tab = WorkspaceTab.Pin())
	{
		Tab->SetContent(SNullWidget::NullWidget);
	}
	for (const TSharedRef<SWindow>& Window : FloatingWindows)
	{
		FSlateApplication::Get().DestroyWindowImmediately(Window);
	}
	PanelTabManager.Reset();
}

void FStageWorkspaceShell::CreatePanelTabManager(const TSharedRef<SDockTab>& InWorkspaceTab)
{
	using namespace StageWorkspaceShell;

	PanelTabManager = FGlobalTabmanager::Get()->NewTabManager(InWorkspaceTab);
	PanelTabManager->SetOnPersistLayout(FTabManager::FOnPersistLayout()); // persistence is ours (ADR §3.4)

	PanelTabManager->RegisterTabSpawner(StageCraftTags::Panel_Viewport.GetTag().GetTagName(), FOnSpawnTab::CreateSP(this, &FStageWorkspaceShell::SpawnViewportTab))
		.SetDisplayName(GetPanelLabel(StageCraftTags::Panel_Viewport));

	for (const FGameplayTag& PanelTag : GetKnownPanels())
	{
		if (PanelTag == StageCraftTags::Panel_Viewport)
		{
			continue;
		}
		PanelTabManager->RegisterTabSpawner(PanelTag.GetTagName(), FOnSpawnTab::CreateSP(this, &FStageWorkspaceShell::SpawnWidgetPanelTab, PanelTag))
			.SetDisplayName(GetPanelLabel(PanelTag));
	}
}

TSharedRef<SWidget> FStageWorkspaceShell::RestorePanels(const TSharedRef<FTabManager::FLayout>& PanelLayout, const TSharedPtr<SWindow>& OwnerWindow)
{
	const TSharedPtr<SWidget> PanelArea = PanelTabManager->RestoreFrom(PanelLayout, OwnerWindow, false, EOutputCanBeNullptr::Never);
	UE_LOG(LogStageWorkspace, Log, TEXT("Panel layout '%s' restored."), *PanelLayout->GetLayoutName().ToString());
	return PanelArea.IsValid() ? PanelArea.ToSharedRef() : SNullWidget::NullWidget;
}

void FStageWorkspaceShell::SyncReportedHosts()
{
	// Re-sync the derived host map from Slate without reporting: a restore is not a user action.
	ReportedHosts.Reset();
	for (const FGameplayTag& PanelTag : GetKnownPanels())
	{
		ReportedHosts.Add(PanelTag, GetPanelHost(PanelTag));
	}
	UE_LOG(LogStageWorkspace, Log, TEXT("Panels: Viewport %s, Inspector %s, Faders %s."),
		*UEnum::GetValueAsString(ReportedHosts[StageCraftTags::Panel_Viewport]),
		*UEnum::GetValueAsString(ReportedHosts[StageCraftTags::Panel_Inspector]),
		*UEnum::GetValueAsString(ReportedHosts[StageCraftTags::Panel_FaderBank]));
}

void FStageWorkspaceShell::RefreshPanelContent()
{
	for (const FGameplayTag& PanelTag : GetKnownPanels())
	{
		if (PanelTag == StageCraftTags::Panel_Viewport)
		{
			continue;
		}
		if (TSharedPtr<SDockTab> Tab = FindLiveTab(PanelTag))
		{
			Tab->SetContent(Callbacks.CreatePanelContent(PanelTag));
		}
	}
}

bool FStageWorkspaceShell::OpenPanel(const FGameplayTag& PanelTag)
{
	if (!bInstalled || !PanelTabManager.IsValid() || !GetKnownPanels().Contains(PanelTag))
	{
		return false;
	}
	return PanelTabManager->TryInvokeTab(StageWorkspaceShell::ToTabId(PanelTag)).IsValid();
}

EStagePanelHost FStageWorkspaceShell::GetPanelHost(const FGameplayTag& PanelTag) const
{
	const TSharedPtr<SDockTab> Tab = FindLiveTab(PanelTag);
	if (!Tab.IsValid())
	{
		return EStagePanelHost::Closed;
	}
	// SDockTab::GetParentWindow only knows windows its dock area was created with (null inside the Workspace tab),
	// so walk the real widget hierarchy. A live tab outside any window is not visible to the user: it counts as closed.
	const TSharedPtr<SWindow> TabWindow = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
	if (!TabWindow.IsValid())
	{
		return EStagePanelHost::Closed;
	}
	return TabWindow == MainWindow.Pin() ? EStagePanelHost::MainWindow : EStagePanelHost::FloatingWindow;
}

TSharedPtr<SWindow> FStageWorkspaceShell::GetViewportWindow() const
{
	const TSharedPtr<SViewport> Viewport = ViewportWidget.Pin();
	return Viewport.IsValid() && FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindWidgetWindow(Viewport.ToSharedRef()) : nullptr;
}

TSharedPtr<SDockTab> FStageWorkspaceShell::FindLiveTab(const FGameplayTag& PanelTag) const
{
	return bInstalled && PanelTabManager.IsValid() ? PanelTabManager->FindExistingLiveTab(StageWorkspaceShell::ToTabId(PanelTag)) : nullptr;
}

TSharedRef<SDockTab> FStageWorkspaceShell::SpawnViewportTab(const FSpawnTabArgs& Args)
{
	const FGameplayTag PanelTag = StageCraftTags::Panel_Viewport;
	const TSharedPtr<SViewport> Viewport = ViewportWidget.Pin();

	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(StageWorkspaceShell::GetPanelLabel(PanelTag))
		// The viewport is the app. It can move anywhere but never disappear (ADR §3.5).
		.OnCanCloseTab_Lambda([]() { return false; })
		.OnTabRelocated(FSimpleDelegate::CreateSP(this, &FStageWorkspaceShell::HandleTabRelocated, PanelTag))
		.OnTabClosed(SDockTab::FOnTabClosedCallback::CreateSP(this, &FStageWorkspaceShell::HandleTabClosed, PanelTag))
		[
			Viewport.IsValid() ? Viewport.ToSharedRef() : SNullWidget::NullWidget
		];
}

TSharedRef<SDockTab> FStageWorkspaceShell::SpawnWidgetPanelTab(const FSpawnTabArgs& Args, FGameplayTag PanelTag)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(StageWorkspaceShell::GetPanelLabel(PanelTag))
		.OnTabRelocated(FSimpleDelegate::CreateSP(this, &FStageWorkspaceShell::HandleTabRelocated, PanelTag))
		.OnTabClosed(SDockTab::FOnTabClosedCallback::CreateSP(this, &FStageWorkspaceShell::HandleTabClosed, PanelTag))
		[
			Callbacks.CreatePanelContent(PanelTag)
		];
}

void FStageWorkspaceShell::HandleTabRelocated(FGameplayTag PanelTag)
{
	if (PanelTag == StageCraftTags::Panel_Viewport)
	{
		HandleViewportMoved();
	}
	if (!bRestoringLayout)
	{
		NotifyHostChanged(PanelTag, GetPanelHost(PanelTag));
	}
}

void FStageWorkspaceShell::HandleTabClosed(TSharedRef<SDockTab> Tab, FGameplayTag PanelTag)
{
	if (!bRestoringLayout)
	{
		NotifyHostChanged(PanelTag, EStagePanelHost::Closed);
	}
}

void FStageWorkspaceShell::HandleViewportMoved()
{
	const TSharedPtr<SViewport> Viewport = ViewportWidget.Pin();
	if (!Viewport.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}

	// Slate routes game input and focus through the window that owns the registered game viewport,
	// so registration must follow the viewport into its new window (SlateApplication.cpp:2524-2551, ADR F6).
	FSlateApplication::Get().RegisterGameViewport(Viewport.ToSharedRef());

	const TSharedPtr<SWindow> Window = GetViewportWindow();
	UE_LOG(LogStageWorkspace, Log, TEXT("Viewport is now in window '%s' (%s)."),
		Window.IsValid() ? *Window->GetTitle().ToString() : TEXT("<none>"),
		!Window.IsValid() ? TEXT("not shown") : Window == MainWindow.Pin() ? TEXT("main") : TEXT("floating"));
}

void FStageWorkspaceShell::NotifyHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost)
{
	EStagePanelHost& Reported = ReportedHosts.FindOrAdd(PanelTag, EStagePanelHost::Closed);
	if (Reported == NewHost)
	{
		return;
	}
	const EStagePanelHost PreviousHost = Reported;
	Reported = NewHost;
	Callbacks.PanelHostChanged(PanelTag, NewHost, PreviousHost);
}

#undef LOCTEXT_NAMESPACE
