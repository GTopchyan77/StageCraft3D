// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageWorkspaceShell.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "StageWorkspaceShell"

const FName FStageWorkspaceShell::PanelLayoutVersion(TEXT("StageCraft_Workspace_v1"));

namespace StageWorkspaceShell
{
	/** Slate tab type of the single major tab that hosts the panel tab manager. Internal to the shell; never saved in panel layouts. */
	const FName WorkspaceTabId(TEXT("StageCraft.Workspace"));

	/** Version key of the root layout (one hidden Workspace tab). Never saved. */
	const FName RootLayoutName(TEXT("StageCraft_Root_v1"));
}

FStageWorkspaceShell::FStageWorkspaceShell(const TSharedRef<SWindow>& InMainWindow, const TSharedRef<SViewport>& InViewportWidget, FStageWorkspaceShellCallbacks InCallbacks)
	: MainWindow(InMainWindow)
	, ViewportWidget(InViewportWidget)
	, Callbacks(MoveTemp(InCallbacks))
{
	check(Callbacks.GetWidgetPanels);
	check(Callbacks.GetPanelLabel);
	check(Callbacks.CreatePanelContent);
	check(Callbacks.PanelHostChanged);
	check(Callbacks.LayoutChanged);
	check(Callbacks.MainWindowClosing);
	check(Callbacks.SetMainWindowContent);
	check(Callbacks.RestoreMainWindowViewport);
}

FTabId FStageWorkspaceShell::ToTabId(const FGameplayTag& PanelTag)
{
	return FTabId(PanelTag.GetTagName());
}

TArray<FGameplayTag> FStageWorkspaceShell::GetPanels() const
{
	TArray<FGameplayTag> Panels = Callbacks.GetWidgetPanels();
	Panels.Remove(StageCraftTags::Panel_Viewport);
	Panels.Insert(StageCraftTags::Panel_Viewport, 0);
	return Panels;
}

TSharedRef<FTabManager::FLayout> FStageWorkspaceShell::NewPanelLayout()
{
	return FTabManager::NewLayout(PanelLayoutVersion);
}

TSharedRef<FTabManager::FLayout> FStageWorkspaceShell::MakeDefaultLayout()
{
	return NewPanelLayout()
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.16f)
				->AddTab(ToTabId(StageCraftTags::Panel_Library), ETabState::OpenedTab)
			)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.62f)
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
				FTabManager::NewStack()->SetSizeCoefficient(0.22f)
				->AddTab(ToTabId(StageCraftTags::Panel_Inspector), ETabState::OpenedTab)
			)
		);
}

TSharedRef<FTabManager::FLayout> FStageWorkspaceShell::MakeViewportOnlyLayout()
{
	return NewPanelLayout()
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->Split(FTabManager::NewStack()->AddTab(ToTabId(StageCraftTags::Panel_Viewport), ETabState::OpenedTab))
		);
}

void FStageWorkspaceShell::Install(const TSharedRef<FTabManager::FLayout>& PanelLayout, const TSharedRef<SWidget>& MenuBar)
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

	// Slate destroys child windows before their parent, so by the time anything else hears about the app closing,
	// the floating panel windows are gone. This is the last point where the whole layout can be captured.
	// The engine does not use this override on its game window (it binds SetOnWindowClosed, GameEngine.cpp:247).
	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateSP(this, &FStageWorkspaceShell::HandleMainWindowCloseRequested));

	bInstalled = true;
	MenuBarWidget = MenuBar;
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
		if (const TSharedPtr<SWindow> Window = MainWindow.Pin())
		{
			Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		}

		TGuardValue<bool> RestoreGuard(bRestoringLayout, true);
		TearDownPanels();
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(StageWorkspaceShell::WorkspaceTabId);

		// Leave the window as UGameEngine expects it: the bare viewport as its content. This also releases the root dock area.
		Callbacks.RestoreMainWindowViewport();
	}

	PanelTabManager.Reset();
	PendingPanelLayout.Reset();
	MenuBarWidget.Reset();
	StatusBarSlot.Reset();
	ReportedHosts.Reset();
}

void FStageWorkspaceShell::ApplyLayout(const TSharedRef<FTabManager::FLayout>& PanelLayout)
{
	const TSharedPtr<SDockTab> Tab = WorkspaceTab.Pin();
	if (!bInstalled || !Tab.IsValid())
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("ApplyLayout ignored: the workspace is not installed."));
		return;
	}

	PendingPanelLayout = PanelLayout;
	{
		TGuardValue<bool> RestoreGuard(bRestoringLayout, true);
		TearDownPanels();
		CreatePanelTabManager(Tab.ToSharedRef());
		Tab->SetContent(BuildWorkspaceContent(PanelLayout));
	}
	SyncReportedHosts();
	HandleViewportMoved();
}

TSharedPtr<FTabManager::FLayout> FStageWorkspaceShell::CaptureLayout() const
{
	return bInstalled && PanelTabManager.IsValid() ? PanelTabManager->PersistLayout().ToSharedPtr() : nullptr;
}

TOptional<FStageWindowPlacement> FStageWorkspaceShell::GetMainWindowPlacement() const
{
	const TSharedPtr<SWindow> Window = MainWindow.Pin();
	if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
	{
		return {};
	}

	FStageWindowPlacement Placement;
	Placement.bMaximized = Window->IsWindowMaximized();
	Placement.Position = Window->GetPositionInScreen();
	Placement.Size = Window->GetSizeInScreen();

	// A maximized window's live rect is the whole monitor. Save the rect it returns to instead.
	int32 X = 0, Y = 0, Width = 0, Height = 0;
	if (Placement.bMaximized && ConstCastSharedPtr<FGenericWindow>(Window->GetNativeWindow())->GetRestoredDimensions(X, Y, Width, Height) && Width > 0 && Height > 0)
	{
		Placement.Position = FVector2D(X, Y);
		Placement.Size = FVector2D(Width, Height);
	}
	return Placement;
}

void FStageWorkspaceShell::ApplyMainWindowPlacement(const FStageWindowPlacement& Placement)
{
	const TSharedPtr<SWindow> Window = MainWindow.Pin();
	if (!Window.IsValid() || Window->GetWindowMode() != EWindowMode::Windowed)
	{
		return;
	}
	// The engine's resize-to-resolution handling is removed while the viewport can dock (ADR F8), so this never becomes the saved resolution.
	if (Window->IsWindowMaximized())
	{
		Window->Restore();
	}
	Window->ReshapeWindow(Placement.Position, Placement.Size);
	if (Placement.bMaximized)
	{
		Window->Maximize();
	}
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
	Tab->SetContent(BuildWorkspaceContent(PendingPanelLayout.IsValid() ? PendingPanelLayout.ToSharedRef() : MakeDefaultLayout()));
	return Tab;
}

TSharedRef<SWidget> FStageWorkspaceShell::BuildWorkspaceContent(const TSharedRef<FTabManager::FLayout>& PanelLayout)
{
	const TSharedPtr<SWidget> PanelArea = PanelTabManager->RestoreFrom(PanelLayout, MainWindow.Pin(), false, EOutputCanBeNullptr::Never);
	UE_LOG(LogStageWorkspace, Log, TEXT("Panel layout '%s' restored."), *PanelLayout->GetLayoutName().ToString());

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MenuBarWidget.IsValid() ? MenuBarWidget.ToSharedRef() : SNullWidget::NullWidget
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			PanelArea.IsValid() ? PanelArea.ToSharedRef() : SNullWidget::NullWidget
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(StatusBarSlot, SBox)
			[
				MakeStatusBarContent()
			]
		];
}

TSharedRef<SWidget> FStageWorkspaceShell::MakeStatusBarContent() const
{
	return Callbacks.CreateStatusBarContent ? Callbacks.CreateStatusBarContent() : SNullWidget::NullWidget;
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
	for (const FGameplayTag& PanelTag : GetPanels())
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
	PanelTabManager = FGlobalTabmanager::Get()->NewTabManager(InWorkspaceTab);
	// Slate calls this a few seconds after any arrangement change; the subsystem decides what to save (ADR §3.4).
	PanelTabManager->SetOnPersistLayout(FTabManager::FOnPersistLayout::CreateSP(this, &FStageWorkspaceShell::HandlePersistLayout));
	RegisterPanelSpawners();
}

void FStageWorkspaceShell::RegisterPanelSpawners()
{
	if (!PanelTabManager.IsValid())
	{
		return;
	}

	for (const FGameplayTag& PanelTag : GetPanels())
	{
		const FName TabId = PanelTag.GetTagName();
		if (PanelTabManager->HasTabSpawner(TabId))
		{
			continue;
		}

		const FOnSpawnTab Spawner = PanelTag == StageCraftTags::Panel_Viewport
			? FOnSpawnTab::CreateSP(this, &FStageWorkspaceShell::SpawnViewportTab)
			: FOnSpawnTab::CreateSP(this, &FStageWorkspaceShell::SpawnWidgetPanelTab, PanelTag);
		PanelTabManager->RegisterTabSpawner(TabId, Spawner)
			.SetDisplayName(Callbacks.GetPanelLabel(PanelTag));
	}
}

void FStageWorkspaceShell::SyncReportedHosts()
{
	// Re-sync the derived host map from Slate without reporting: a restore is not a user action.
	ReportedHosts.Reset();
	TStringBuilder<256> Summary;
	for (const FGameplayTag& PanelTag : GetPanels())
	{
		const EStagePanelHost Host = GetPanelHost(PanelTag);
		ReportedHosts.Add(PanelTag, Host);
		Summary.Appendf(TEXT("%s%s %s"), Summary.Len() > 0 ? TEXT(", ") : TEXT(""), *PanelTag.ToString(), *UEnum::GetDisplayValueAsText(Host).ToString());
	}
	UE_LOG(LogStageWorkspace, Log, TEXT("Panels: %s."), Summary.ToString());
}

void FStageWorkspaceShell::RefreshPanelContent()
{
	for (const FGameplayTag& PanelTag : GetPanels())
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
	if (StatusBarSlot.IsValid())
	{
		StatusBarSlot->SetContent(MakeStatusBarContent());
	}
}

bool FStageWorkspaceShell::OpenPanel(const FGameplayTag& PanelTag)
{
	if (!bInstalled || !PanelTabManager.IsValid() || !PanelTabManager->HasTabSpawner(PanelTag.GetTagName()))
	{
		return false;
	}
	return PanelTabManager->TryInvokeTab(ToTabId(PanelTag)).IsValid();
}

bool FStageWorkspaceShell::ClosePanel(const FGameplayTag& PanelTag)
{
	const TSharedPtr<SDockTab> Tab = PanelTag != StageCraftTags::Panel_Viewport ? FindLiveTab(PanelTag) : nullptr;
	return Tab.IsValid() && Tab->RequestCloseTab();
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
	return bInstalled && PanelTabManager.IsValid() ? PanelTabManager->FindExistingLiveTab(ToTabId(PanelTag)) : nullptr;
}

TSharedRef<SDockTab> FStageWorkspaceShell::SpawnViewportTab(const FSpawnTabArgs& Args)
{
	const FGameplayTag PanelTag = StageCraftTags::Panel_Viewport;
	const TSharedPtr<SViewport> Viewport = ViewportWidget.Pin();

	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(Callbacks.GetPanelLabel(PanelTag))
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
	// Read live: a definition that is still loading at spawn gets its real name once it arrives.
	const TWeakPtr<FStageWorkspaceShell> WeakThis = AsWeak();
	const TAttribute<FText> Label = TAttribute<FText>::CreateLambda([WeakThis, PanelTag]()
	{
		const TSharedPtr<FStageWorkspaceShell> This = WeakThis.Pin();
		return This.IsValid() ? This->Callbacks.GetPanelLabel(PanelTag) : FText::GetEmpty();
	});

	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(Label)
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

void FStageWorkspaceShell::HandlePersistLayout(const TSharedRef<FTabManager::FLayout>& Layout)
{
	// Restores request a deferred save too. That save carries the restored arrangement, so it is harmless, but only the live manager's saves count.
	if (bInstalled && !bRestoringLayout)
	{
		Callbacks.LayoutChanged();
	}
}

void FStageWorkspaceShell::HandleMainWindowCloseRequested(const TSharedRef<SWindow>& Window)
{
	if (bInstalled)
	{
		Callbacks.MainWindowClosing();
	}
	// Continue exactly as without the override (SWindow::RequestDestroyWindow). UGameEngine::OnGameWindowClosed then quits.
	Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
	FSlateApplication::Get().RequestDestroyWindow(Window);
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

	// Re-parenting leaves each user's keyboard focus path running through the destroyed tabs, so key presses
	// (Esc, P, Space) would reach nothing until the viewport is clicked. Focusing the viewport alone is not enough:
	// Slate skips a focus change to the widget that is already focused (SlateApplication.cpp, SetUserFocus), so the
	// stale path is cleared first and then rebuilt through the viewport's new parents.
	const TSharedPtr<SWindow> Window = GetViewportWindow();
	if (Window.IsValid())
	{
		FSlateApplication::Get().ClearAllUserFocus(EFocusCause::SetDirectly);
		FSlateApplication::Get().SetAllUserFocusToGameViewport(EFocusCause::SetDirectly);
	}

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
