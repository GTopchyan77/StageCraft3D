// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the dockable workspace (Docs/ADR/0001-dockable-workspace.md).
// They drive layouts and probe the viewport from the console so docking behaviour can be verified from
// logs, without a mouse. Run in Standalone Game. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "CollisionQueryParams.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/HitResult.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Player/ModularPlayerController.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SWindow.h"
#include "Workspace/StageWorkspaceShell.h"
#include "Workspace/StageWorkspaceSubsystem.h"

namespace StageWorkspaceCommands
{
	UStageWorkspaceSubsystem* GetWorkspace(UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;
	}

	TSharedPtr<FStageWorkspaceShell> GetActiveShell(UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		if (!Workspace || !Workspace->IsWorkspaceActive())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("The workspace is not active (state %s). Run as Standalone Game without -StageDirectViewport."),
				Workspace ? *UEnum::GetValueAsString(Workspace->GetState()) : TEXT("<no subsystem: editor/PIE>"));
			return nullptr;
		}
		return Workspace->GetShellForDiagnostics();
	}

	FString DescribeWindow(const TSharedPtr<SWindow>& Window)
	{
		if (!Window.IsValid())
		{
			return TEXT("<none>");
		}
		const FVector2D Position = Window->GetPositionInScreen();
		const FVector2D Size = Window->GetSizeInScreen();
		return FString::Printf(TEXT("'%s' at (%.0f, %.0f) size %.0fx%.0f"), *Window->GetTitle().ToString(), Position.X, Position.Y, Size.X, Size.Y);
	}

	void Status(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		UE_LOG(LogStageWorkspace, Display, TEXT("Workspace state: %s"), Workspace ? *UEnum::GetValueAsString(Workspace->GetState()) : TEXT("<no subsystem>"));

		if (const UGameEngine* GameEngine = Cast<UGameEngine>(GEngine))
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("  Main window: %s"), *DescribeWindow(GameEngine->GameViewportWindow.Pin()));
			if (GameEngine->SceneViewport.IsValid())
			{
				const FIntPoint Size = GameEngine->SceneViewport->GetSizeXY();
				const ISlateViewport& SlateViewport = *GameEngine->SceneViewport;
				UE_LOG(LogStageWorkspace, Display, TEXT("  Scene viewport: %dx%d, separate render target: %s"),
					Size.X, Size.Y, SlateViewport.UseSeparateRenderTarget() ? TEXT("yes") : TEXT("no"));
			}
		}
		UE_LOG(LogStageWorkspace, Display, TEXT("  GSystemResolution: %dx%d mode %d"), GSystemResolution.ResX, GSystemResolution.ResY, static_cast<int32>(GSystemResolution.WindowMode));
		if (const UGameUserSettings* Settings = GEngine ? GEngine->GetGameUserSettings() : nullptr)
		{
			const FIntPoint Saved = Settings->GetScreenResolution();
			UE_LOG(LogStageWorkspace, Display, TEXT("  GameUserSettings resolution: %dx%d"), Saved.X, Saved.Y);
		}

		if (TSharedPtr<FStageWorkspaceShell> Shell = (Workspace && Workspace->IsWorkspaceActive()) ? Workspace->GetShellForDiagnostics() : nullptr)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("  Viewport window: %s"), *DescribeWindow(Shell->GetViewportWindow()));
			for (const FGameplayTag& PanelTag : Workspace->GetAvailablePanels())
			{
				UE_LOG(LogStageWorkspace, Display, TEXT("  %s: %s"), *PanelTag.ToString(), *UEnum::GetValueAsString(Shell->GetPanelHost(PanelTag)));
			}
		}

		TArray<TSharedRef<SWindow>> Windows;
		FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
		UE_LOG(LogStageWorkspace, Display, TEXT("  %d visible Slate windows:"), Windows.Num());
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("    %s"), *DescribeWindow(Window));
		}
	}

	/** Floats the viewport into its own window on the given monitor (default: the last one), panels stay in the main window. */
	void FloatViewport(const TArray<FString>& Args, UWorld* World)
	{
		TSharedPtr<FStageWorkspaceShell> Shell = GetActiveShell(World);
		if (!Shell.IsValid())
		{
			return;
		}

		FDisplayMetrics Metrics;
		FSlateApplication::Get().GetCachedDisplayMetrics(Metrics);
		if (Metrics.MonitorInfo.IsEmpty())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("FloatViewport: no monitor information."));
			return;
		}
		const int32 MonitorIndex = Args.IsEmpty() ? Metrics.MonitorInfo.Num() - 1 : FMath::Clamp(FCString::Atoi(*Args[0]), 0, Metrics.MonitorInfo.Num() - 1);
		const FPlatformRect& WorkArea = Metrics.MonitorInfo[MonitorIndex].WorkArea;
		const FVector2D WindowSize(FMath::Min(1280, WorkArea.Right - WorkArea.Left - 100), FMath::Min(720, WorkArea.Bottom - WorkArea.Top - 100));
		const FVector2D WindowPosition(WorkArea.Left + 50, WorkArea.Top + 50);

		UE_LOG(LogStageWorkspace, Display, TEXT("FloatViewport: monitor index %d of %d ('%s'), work area (%d, %d)-(%d, %d)."), MonitorIndex, Metrics.MonitorInfo.Num(),
			*Metrics.MonitorInfo[MonitorIndex].Name, WorkArea.Left, WorkArea.Top, WorkArea.Right, WorkArea.Bottom);

		const TSharedRef<FTabManager::FLayout> Layout = FStageWorkspaceShell::NewPanelLayout()
			->AddArea
			(
				FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.6f)->AddTab(FTabId(StageCraftTags::Panel_FaderBank.GetTag().GetTagName()), ETabState::OpenedTab))
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.4f)->AddTab(FTabId(StageCraftTags::Panel_Inspector.GetTag().GetTagName()), ETabState::OpenedTab))
			)
			->AddArea
			(
				FTabManager::NewArea(WindowSize)->SetWindow(WindowPosition, false)
				->Split(FTabManager::NewStack()->AddTab(FTabId(StageCraftTags::Panel_Viewport.GetTag().GetTagName()), ETabState::OpenedTab))
			);
		Shell->ApplyLayout(Layout);
		Status(Args, World);
	}

	void Reset(const TArray<FString>& Args, UWorld* World)
	{
		if (UStageWorkspaceSubsystem* Workspace = GetWorkspace(World))
		{
			Workspace->ResetToDefaultLayout();
		}
		Status(Args, World);
	}

	/** Only the viewport, filling the main window: the like-for-like comparison against -StageDirectViewport for frame-cost measurements. */
	void ViewportOnly(const TArray<FString>& Args, UWorld* World)
	{
		TSharedPtr<FStageWorkspaceShell> Shell = GetActiveShell(World);
		if (!Shell.IsValid())
		{
			return;
		}
		Shell->ApplyLayout(FStageWorkspaceShell::NewPanelLayout()
			->AddArea
			(
				FTabManager::NewPrimaryArea()
				->Split(FTabManager::NewStack()->SetHideTabWell(true)->AddTab(FTabId(StageCraftTags::Panel_Viewport.GetTag().GetTagName()), ETabState::OpenedTab))
			));
		Status(Args, World);
	}

	/** Restores the default layout plus a tab no spawner knows, to see how Slate treats a removed panel in a saved layout. */
	void TestUnknownTab(const TArray<FString>& Args, UWorld* World)
	{
		TSharedPtr<FStageWorkspaceShell> Shell = GetActiveShell(World);
		if (!Shell.IsValid())
		{
			return;
		}
		const TSharedRef<FTabManager::FLayout> Layout = FStageWorkspaceShell::NewPanelLayout()
			->AddArea
			(
				FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.7f)->AddTab(FTabId(StageCraftTags::Panel_Viewport.GetTag().GetTagName()), ETabState::OpenedTab))
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.3f)
					->AddTab(FTabId(TEXT("StageCraft.Panel.DoesNotExist")), ETabState::OpenedTab)
					->AddTab(FTabId(StageCraftTags::Panel_Inspector.GetTag().GetTagName()), ETabState::OpenedTab))
			);
		Shell->ApplyLayout(Layout);
		Status(Args, World);
	}

	/**
	 * Moves the OS cursor to the viewport's centre through the controller (the same path as a click), then compares
	 * GetHitResultUnderCursor with a centre deprojection. Equal hits prove mouse picking uses the viewport's own window and geometry.
	 */
	void ProbePick(const TArray<FString>& Args, UWorld* World)
	{
		AModularPlayerController* Controller = World ? Cast<AModularPlayerController>(World->GetFirstPlayerController()) : nullptr;
		if (!Controller)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("ProbePick: no AModularPlayerController."));
			return;
		}

		int32 SizeX = 0;
		int32 SizeY = 0;
		Controller->GetViewportSize(SizeX, SizeY);
		const FVector2D Centre(SizeX * 0.5f, SizeY * 0.5f);
		Controller->SetMouseLocation(FMath::RoundToInt(Centre.X), FMath::RoundToInt(Centre.Y));

		float MouseX = 0.f;
		float MouseY = 0.f;
		const bool bHasMouse = Controller->GetMousePosition(MouseX, MouseY);

		FHitResult CursorHit;
		Controller->GetHitResultUnderCursor(ECC_Visibility, false, CursorHit);

		FVector Origin;
		FVector Direction;
		FHitResult CentreHit;
		if (Controller->DeprojectScreenPositionToWorld(Centre.X, Centre.Y, Origin, Direction))
		{
			World->LineTraceSingleByChannel(CentreHit, Origin, Origin + Direction * 100000.f, ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(StageWorkspaceProbe), false));
		}

		const FVector2D Cursor = FSlateApplication::Get().GetCursorPos();
		UE_LOG(LogStageWorkspace, Display, TEXT("ProbePick: viewport %dx%d, OS cursor (%.0f, %.0f), viewport mouse %s(%.0f, %.0f)."),
			SizeX, SizeY, Cursor.X, Cursor.Y, bHasMouse ? TEXT("") : TEXT("<none> "), MouseX, MouseY);
		UE_LOG(LogStageWorkspace, Display, TEXT("  Under cursor: %s at %s"), *GetNameSafe(CursorHit.GetActor()), *CursorHit.ImpactPoint.ToCompactString());
		UE_LOG(LogStageWorkspace, Display, TEXT("  Centre ray:   %s at %s -> %s"), *GetNameSafe(CentreHit.GetActor()), *CentreHit.ImpactPoint.ToCompactString(),
			CursorHit.GetActor() == CentreHit.GetActor() && CursorHit.ImpactPoint.Equals(CentreHit.ImpactPoint, 5.f) ? TEXT("MATCH") : TEXT("MISMATCH"));
	}

	/** Runs a console command after a delay, so one -ExecCmds line can script a timed test sequence. */
	void Delay(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Usage: StageCraft.Workspace.Delay <Seconds> <Command...>"));
			return;
		}
		const float Seconds = FMath::Max(0.f, FCString::Atof(*Args[0]));
		const FString Command = FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Command](float)
		{
			// Resolved when it fires: level travel may have replaced the world in the meantime.
			UWorld* CurrentWorld = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
			UE_LOG(LogStageWorkspace, Display, TEXT("Delay: running '%s'."), *Command);
			if (GEngine)
			{
				GEngine->Exec(CurrentWorld, *Command);
			}
			return false; // one-shot
		}), Seconds);
	}

	/** The layout name is the whole argument list, so names with spaces need no quoting. */
	FString JoinArgs(const TArray<FString>& Args)
	{
		return FString::Join(Args, TEXT(" "));
	}

	FGameplayTag ParsePanelTag(const TArray<FString>& Args)
	{
		return Args.IsEmpty() ? FGameplayTag() : FGameplayTag::RequestGameplayTag(FName(*Args[0]), /*ErrorIfNotFound*/ false);
	}

	void OpenPanel(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		const FGameplayTag PanelTag = ParsePanelTag(Args);
		UE_LOG(LogStageWorkspace, Display, TEXT("OpenPanel %s: %s"), *PanelTag.ToString(), Workspace && Workspace->OpenPanel(PanelTag) ? TEXT("ok") : TEXT("refused"));
	}

	void ClosePanel(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		const FGameplayTag PanelTag = ParsePanelTag(Args);
		UE_LOG(LogStageWorkspace, Display, TEXT("ClosePanel %s: %s"), *PanelTag.ToString(), Workspace && Workspace->ClosePanel(PanelTag) ? TEXT("ok") : TEXT("refused"));
	}

	void SaveLayout(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		const EStageLayoutResult Result = Workspace ? Workspace->SaveCurrentLayoutAs(JoinArgs(Args)) : EStageLayoutResult::WorkspaceInactive;
		UE_LOG(LogStageWorkspace, Display, TEXT("SaveLayout '%s': %s"), *JoinArgs(Args), *UEnum::GetValueAsString(Result));
	}

	void LoadLayout(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		const EStageLayoutResult Result = Workspace ? Workspace->ApplyLayout(JoinArgs(Args)) : EStageLayoutResult::WorkspaceInactive;
		UE_LOG(LogStageWorkspace, Display, TEXT("LoadLayout '%s': %s"), *JoinArgs(Args), *UEnum::GetValueAsString(Result));
		Status(Args, World);
	}

	void DeleteLayout(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		const EStageLayoutResult Result = Workspace ? Workspace->DeleteLayout(JoinArgs(Args)) : EStageLayoutResult::WorkspaceInactive;
		UE_LOG(LogStageWorkspace, Display, TEXT("DeleteLayout '%s': %s"), *JoinArgs(Args), *UEnum::GetValueAsString(Result));
	}

	void ListLayouts(const TArray<FString>& Args, UWorld* World)
	{
		const UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		if (!Workspace)
		{
			return;
		}
		UE_LOG(LogStageWorkspace, Display, TEXT("Active layout: '%s'. Built-in: %s. User: %s."), *Workspace->GetActiveLayoutName(),
			*FString::Join(Workspace->GetBuiltInLayouts(), TEXT(", ")), *FString::Join(Workspace->GetUserLayouts(), TEXT(", ")));
	}

	/** Closes the main window exactly as its OS close button does (SWindow::RequestDestroyWindow), which quits the app. */
	void CloseMainWindow(const TArray<FString>& Args, UWorld* World)
	{
		const UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
		if (const TSharedPtr<SWindow> Window = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("CloseMainWindow: requesting destroy of %s."), *DescribeWindow(Window));
			Window->RequestDestroyWindow();
		}
	}

	FAutoConsoleCommandWithWorldAndArgs CloseMainWindowCommand(TEXT("StageCraft.Workspace.CloseMainWindow"), TEXT("Closes the main window like its OS close button (quits)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&CloseMainWindow));
	FAutoConsoleCommandWithWorldAndArgs OpenPanelCommand(TEXT("StageCraft.Workspace.OpenPanel"), TEXT("<PanelTag> Opens or focuses a panel."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OpenPanel));
	FAutoConsoleCommandWithWorldAndArgs ClosePanelCommand(TEXT("StageCraft.Workspace.ClosePanel"), TEXT("<PanelTag> Closes a panel (not the viewport)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ClosePanel));
	FAutoConsoleCommandWithWorldAndArgs SaveLayoutCommand(TEXT("StageCraft.Workspace.SaveLayout"), TEXT("<Name> Saves the current arrangement as a user layout."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SaveLayout));
	FAutoConsoleCommandWithWorldAndArgs LoadLayoutCommand(TEXT("StageCraft.Workspace.LoadLayout"), TEXT("<Name> Applies a built-in or user layout."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LoadLayout));
	FAutoConsoleCommandWithWorldAndArgs DeleteLayoutCommand(TEXT("StageCraft.Workspace.DeleteLayout"), TEXT("<Name> Deletes a user layout."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DeleteLayout));
	FAutoConsoleCommandWithWorldAndArgs ListLayoutsCommand(TEXT("StageCraft.Workspace.ListLayouts"), TEXT("Logs the active, built-in and user layouts."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ListLayouts));
	FAutoConsoleCommandWithWorldAndArgs DelayCommand(TEXT("StageCraft.Workspace.Delay"), TEXT("<Seconds> <Command...> Runs a console command later (test scripting)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Delay));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Workspace.Status"), TEXT("Logs workspace state, windows, viewport size and saved resolution."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs FloatCommand(TEXT("StageCraft.Workspace.FloatViewport"), TEXT("[MonitorIndex] Floats the viewport into its own window (default: last monitor)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FloatViewport));
	FAutoConsoleCommandWithWorldAndArgs ResetCommand(TEXT("StageCraft.Workspace.Reset"), TEXT("Restores the default layout."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Reset));
	FAutoConsoleCommandWithWorldAndArgs ViewportOnlyCommand(TEXT("StageCraft.Workspace.ViewportOnly"), TEXT("Shows only the viewport, filling the main window (profiling comparison)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ViewportOnly));
	FAutoConsoleCommandWithWorldAndArgs UnknownTabCommand(TEXT("StageCraft.Workspace.TestUnknownTab"), TEXT("Restores a layout that names a panel which does not exist."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TestUnknownTab));
	FAutoConsoleCommandWithWorldAndArgs ProbeCommand(TEXT("StageCraft.Workspace.ProbePick"), TEXT("Checks that cursor picking matches a centre ray in the viewport's current window."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ProbePick));
}

#endif // !UE_BUILD_SHIPPING
