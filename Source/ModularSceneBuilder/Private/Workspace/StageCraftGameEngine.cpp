// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageCraftGameEngine.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Workspace/StageWorkspaceTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageCraftGameEngine)

TSharedRef<SViewport> UStageCraftGameEngine::CreateGameViewportWidget()
{
	TSharedRef<SViewport> ViewportWidget = Super::CreateGameViewportWidget();
	SceneViewportWidget = ViewportWidget;

#if !UE_BUILD_SHIPPING
	// Profiling switch: keeps the engine's direct rendering so its frame cost can be compared (ADR §6, Phase 0).
	// The workspace then reports Unsupported and the plain HUD is used.
	if (FParse::Param(FCommandLine::Get(), TEXT("StageDirectViewport")))
	{
		UE_LOG(LogStageWorkspace, Log, TEXT("StageCraftGameEngine: -StageDirectViewport set; the game viewport renders directly to the window and cannot be docked."));
		return ViewportWidget;
	}
#endif

	// Must happen here: UGameEngine::CreateGameViewport builds the FSceneViewport right after this call,
	// and FSceneViewport reads this flag once, in its constructor (SceneViewport.cpp:76).
	ViewportWidget->SetRenderDirectlyToWindow(false);
	return ViewportWidget;
}

void UStageCraftGameEngine::Start()
{
	Super::Start();

	// UGameEngine::OnViewportResized copies the viewport's size into GSystemResolution and the saved
	// UGameUserSettings resolution. A viewport in a dock tab is not the window, so that would save the
	// tab's size as the screen resolution (ADR F8). The workspace owns window sizing instead.
	// UGameEngine binds nothing else of its own to this event (GameEngine.cpp:273).
	if (CanDockGameViewport())
	{
		FViewport::ViewportResizedEvent.RemoveAll(this);
		UE_LOG(LogStageWorkspace, Log, TEXT("StageCraftGameEngine: viewport uses a separate render target; engine resize-to-resolution handling removed."));
	}
}

void UStageCraftGameEngine::Tick(float DeltaSeconds, bool bIdleMode)
{
	if (bFirstTickDone || !MainWindowHost.IsValid())
	{
		bFirstTickDone = true;
		Super::Tick(DeltaSeconds, bIdleMode);
		return;
	}

	// UGameEngine's first tick registers GameViewportWidget as Slate's game viewport (GameEngine.cpp:2022). Slate routes
	// game focus and mouse capture through that registration, and our host has no viewport interface (RegisterGameViewport
	// ensures on it). So for this one tick the member is the real viewport. The per-frame window switch runs after the
	// tick (LaunchEngineLoop.cpp:5902), by which time the member points at the host again.
	bFirstTickDone = true;
	GameViewportWidget = SceneViewportWidget;
	Super::Tick(DeltaSeconds, bIdleMode);
	GameViewportWidget = MainWindowHost;
}

TSharedPtr<SViewport> UStageCraftGameEngine::GetGameViewportWidget() const
{
	return SceneViewportWidget.IsValid() ? SceneViewportWidget : Super::GetGameViewportWidget();
}

bool UStageCraftGameEngine::CanDockGameViewport() const
{
	return SceneViewportWidget.IsValid() && !SceneViewportWidget->ShouldRenderDirectly() && GameViewportWindow.IsValid();
}

void UStageCraftGameEngine::SetMainWindowContent(const TSharedRef<SWidget>& Content)
{
	const TSharedPtr<SWindow> Window = GameViewportWindow.Pin();
	if (!ensureMsgf(Window.IsValid() && CanDockGameViewport(), TEXT("SetMainWindowContent needs a main window and a dockable game viewport.")))
	{
		return;
	}

	if (!MainWindowHost.IsValid())
	{
		// A plain container. It has no viewport interface, so it renders and handles input only through its content.
		MainWindowHost = SNew(SViewport);
	}
	MainWindowHost->SetContent(Content);

	// What SwitchGameWindowToUseGameViewport keeps in the window every frame (ADR F12).
	GameViewportWidget = MainWindowHost;
	if (Window->GetContent() != MainWindowHost)
	{
		Window->SetContent(MainWindowHost.ToSharedRef());
	}
}

void UStageCraftGameEngine::RestoreMainWindowViewport()
{
	if (!MainWindowHost.IsValid())
	{
		return;
	}

	// Release the content first: it may still parent the real viewport.
	MainWindowHost->SetContent(SNullWidget::NullWidget);
	MainWindowHost.Reset();
	GameViewportWidget = SceneViewportWidget;

	// After the main window closed, UGameEngine::OnGameWindowClosed has already released SceneViewport (GameEngine.cpp:768-780).
	// The widget then has no viewport interface, and the app is quitting, so there is nothing to hand back or register.
	const TSharedPtr<SWindow> Window = GameViewportWindow.Pin();
	if (Window.IsValid() && SceneViewportWidget.IsValid() && SceneViewport.IsValid())
	{
		Window->SetContent(SceneViewportWidget.ToSharedRef());
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().RegisterGameViewport(SceneViewportWidget.ToSharedRef());
		}
	}
}
