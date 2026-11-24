// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the dockable workspace (Docs/ADR/0001-dockable-workspace.md).
// They drive layouts and probe the viewport from the console so docking behaviour can be verified from
// logs, without a mouse. Run in Standalone Game. Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Algo/Find.h"
#include "CollisionQueryParams.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/HitResult.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "Slate/SceneViewport.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"
#include "Startup/SStageBrandedLoader.h"
#include "UI/StageMenuButton.h"
#include "UI/StageMenuModel.h"
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

	/** The whole main window as Slate draws it (panels, status bar, viewport), saved under Saved/Screenshots. "shot showui" does not reach the viewport client from GEngine->Exec. */
	void Screenshot(const TArray<FString>& Args, UWorld* World)
	{
		FScreenshotRequest::RequestScreenshot(FString(), /*bShowUI*/ true, /*bAddFilenameSuffix*/ true);
		UE_LOG(LogStageWorkspace, Display, TEXT("Screenshot requested (next frame, Saved/Screenshots)."));
	}

	/** Saves one Slate window as Slate draws it to Saved/Screenshots/<Name>_<time>.png. */
	void SaveWindowShot(const TSharedRef<SWindow>& Window, const FString& Name)
	{
		TArray<FColor> Pixels;
		FIntVector Size(0, 0, 0);
		if (!FSlateApplication::Get().TakeScreenshot(Window, Pixels, Size) || Pixels.IsEmpty())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("ScreenshotWindow: capture of %s failed."), *Name);
			return;
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, Png);
		const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("%s_%s.png"), *Name, *FDateTime::Now().ToString(TEXT("%H%M%S")));
		UE_LOG(LogStageWorkspace, Display, TEXT("ScreenshotWindow: %dx%d %s %s."), Size.X, Size.Y, FFileHelper::SaveArrayToFile(Png, *File) ? TEXT("saved") : TEXT("NOT saved"), *File);
	}

	/**
	 * The main window exactly as Slate draws it, overlays and dialogs included (the startup loader, the welcome screen), to
	 * Saved/Screenshots/Window_<time>.png. Screenshot captures through the game viewport, which a dialog over the workspace hides.
	 * "All" also saves every visible floating panel window (Window_<time>.png, Floating1_<time>.png, ...).
	 */
	void ScreenshotWindow(const TArray<FString>& Args, UWorld* World)
	{
		const UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
		const TSharedPtr<SWindow> Window = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr;
		if (!Window.IsValid())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("ScreenshotWindow: capture failed."));
			return;
		}
		SaveWindowShot(Window.ToSharedRef(), TEXT("Window"));
		if (Args.Contains(TEXT("All")))
		{
			// Their children too: an open dropdown of a floating panel is a window of that panel's window.
			int32 Floating = 0;
			TArray<TSharedRef<SWindow>> Pending = Window->GetChildWindows();
			while (!Pending.IsEmpty())
			{
				const TSharedRef<SWindow> Child = Pending[0];
				Pending.RemoveAt(0);
				if (Child->IsVisible())
				{
					SaveWindowShot(Child, FString::Printf(TEXT("Floating%d"), ++Floating));
					Pending.Append(Child->GetChildWindows());
				}
			}
		}
	}

	/**
	 * The live toolbar dropdown of this world whose widget name is Name (SketchSnapMenu, SourceMenu, SnapMenuButton...). Only one
	 * with a Slate widget counts: a panel rebuilt by a layout change leaves its old, unbound widgets until garbage collection.
	 */
	UStageMenuButton* FindMenuButton(UWorld* World, const FString& Name)
	{
		for (TObjectIterator<UStageMenuButton> It; It; ++It)
		{
			if (!It->IsTemplate() && It->GetWorld() == World && It->GetName() == Name && It->GetCachedWidget().IsValid())
			{
				return *It;
			}
		}
		return nullptr;
	}

	/**
	 * "<Dropdown> Describe | Open | Click | Pick <Label/Submenu label>": logs a toolbar dropdown's label and entries as it would open now,
	 * opens it (for a screenshot), or picks an entry through the same description a click runs. Labels may contain spaces.
	 */
	void Menu(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Usage: StageCraft.Workspace.Menu <Dropdown> Describe | Open | Click | Pick <Label/Submenu label>"));
			return;
		}
		UStageMenuButton* Button = FindMenuButton(World, Args[0]);
		if (!Button)
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Menu: no dropdown named %s."), *Args[0]);
			return;
		}
		if (Args[1].Equals(TEXT("Click"), ESearchCase::IgnoreCase))
		{
			// A left click at the button's centre through the same Slate entry points the mouse uses, so the real click path runs
			// (SComboButton::OnButtonClicked and the focus it hands to the menu), not OpenMenu.
			const TSharedPtr<SWidget> Widget = Button->GetCachedWidget();
			FSlateApplication& Slate = FSlateApplication::Get();
			const FGeometry& Geometry = Widget ? Widget->GetTickSpaceGeometry() : FGeometry();
			const FVector2D Centre = FVector2D(Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f));
			const uint32 User = Slate.GetUserIndexForMouse();
			const FPointerEvent Down(User, FSlateApplication::CursorPointerIndex, Centre, Centre, TSet<FKey>({ EKeys::LeftMouseButton }), EKeys::LeftMouseButton, 0.f, FModifierKeysState());
			const FPointerEvent Up(User, FSlateApplication::CursorPointerIndex, Centre, Centre, TSet<FKey>(), EKeys::LeftMouseButton, 0.f, FModifierKeysState());
			Slate.ProcessMouseMoveEvent(FPointerEvent(User, FSlateApplication::CursorPointerIndex, Centre, Centre, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState()));
			const bool bDown = Slate.ProcessMouseButtonDownEvent(nullptr, Down);
			const bool bUp = Slate.ProcessMouseButtonUpEvent(Up);
			const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(Slate.GetUserIndexForKeyboard());
			UE_LOG(LogStageWorkspace, Display, TEXT("Menu %s: clicked at (%.0f, %.0f) (down %s, up %s), focus %s."), *Args[0], Centre.X, Centre.Y,
				bDown ? TEXT("handled") : TEXT("unhandled"), bUp ? TEXT("handled") : TEXT("unhandled"), Focused ? *Focused->GetTypeAsString() : TEXT("nothing"));
		}
		else if (Args[1].Equals(TEXT("Open"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Menu %s: %s."), *Args[0], Button->OpenMenu() ? TEXT("opened") : TEXT("NOT opened (disabled)"));
		}
		else if (Args[1].Equals(TEXT("Pick"), ESearchCase::IgnoreCase))
		{
			const FString Path = FString::Join(TArrayView<const FString>(Args).RightChop(2), TEXT(" "));
			UE_LOG(LogStageWorkspace, Display, TEXT("Menu %s: pick \"%s\" %s."), *Args[0], *Path,
				StageMenu::Pick(Button->GetMenu(), Path) ? TEXT("ran") : TEXT("REFUSED (missing, greyed out or a submenu)"));
		}
		else
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Menu %s: label \"%s\"%s."), *Args[0], *Button->GetLabel().ToString(), Button->GetIsEnabled() ? TEXT("") : TEXT(", disabled"));
			TArray<FString> Lines;
			StageMenu::Describe(Button->GetMenu()).ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogStageWorkspace, Display, TEXT("Menu %s: %s"), *Args[0], *Line);
			}
		}
	}

	/**
	 * "<Text...> | {Escape} | {Enter} | {Down} | {Up}": sends the text, one character at a time, or the named key, to whatever has
	 * keyboard focus, through the same Slate entry points a keyboard uses. Scripted runs use it to type into an open menu's search
	 * ("Start typing to search"), close the menu, and see where focus goes.
	 */
	void Type(const TArray<FString>& Args, UWorld* World)
	{
		const FString Text = FString::Join(Args, TEXT(" "));
		if (Text.IsEmpty() || !FSlateApplication::IsInitialized())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Usage: StageCraft.Workspace.Type <Text...> | {Escape} | {Enter} | {Down} | {Up}"));
			return;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const uint32 User = Slate.GetUserIndexForKeyboard();
		const TSharedPtr<SWidget> Focused = Slate.GetUserFocusedWidget(User);
		const FString Before = Focused ? Focused->GetTypeAsString() : TEXT("nothing");
		if (Text.Equals(TEXT("{Focus}"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Type {Focus}: focus %s."), *Before);
			return;
		}
		int32 Handled = 0;
		int32 Sent = 0;
		const TPair<const TCHAR*, FKey> Keys[] = { { TEXT("{Escape}"), EKeys::Escape }, { TEXT("{Enter}"), EKeys::Enter }, { TEXT("{Down}"), EKeys::Down }, { TEXT("{Up}"), EKeys::Up } };
		const TPair<const TCHAR*, FKey>* Key = Algo::FindByPredicate(Keys, [&Text](const TPair<const TCHAR*, FKey>& Candidate) { return Text.Equals(Candidate.Key, ESearchCase::IgnoreCase); });
		if (Key)
		{
			const FKeyEvent Event(Key->Value, FModifierKeysState(), User, /*bIsRepeat*/ false, /*CharacterCode*/ 0, /*KeyCode*/ 0);
			Handled = Slate.ProcessKeyDownEvent(Event) ? 1 : 0;
			Slate.ProcessKeyUpEvent(Event);
			Sent = 1;
		}
		else
		{
			for (const TCHAR Character : Text)
			{
				Handled += Slate.ProcessKeyCharEvent(FCharacterEvent(Character, FModifierKeysState(), User, /*bIsRepeat*/ false)) ? 1 : 0;
				++Sent;
			}
		}
		const TSharedPtr<SWidget> After = Slate.GetUserFocusedWidget(User);
		UE_LOG(LogStageWorkspace, Display, TEXT("Type \"%s\": %d of %d handled, focus %s -> %s."), *Text, Handled, Sent, *Before,
			After ? *After->GetTypeAsString() : TEXT("nothing"));
	}

	/** Logs the widget tree of the first live user widget whose class name contains the argument: names, classes and slot types, for layout debugging. */
	void DumpWidget(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.IsEmpty())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("Usage: StageCraft.Workspace.DumpWidget <ClassNamePart>"));
			return;
		}
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			if (It->GetWorld() != World || It->IsTemplate() || !It->GetClass()->GetName().Contains(Args[0]) || !It->WidgetTree)
			{
				continue;
			}
			UE_LOG(LogStageWorkspace, Display, TEXT("DumpWidget %s (%s):"), *It->GetName(), *It->GetClass()->GetName());
			It->WidgetTree->ForEachWidget([](UWidget* Widget)
			{
				int32 Depth = 0;
				for (const UWidget* Parent = Widget->GetParent(); Parent; Parent = Parent->GetParent())
				{
					++Depth;
				}
				UE_LOG(LogStageWorkspace, Display, TEXT("  %s%s (%s) slot %s"), *FString::ChrN(Depth * 2, TEXT(' ')), *Widget->GetName(), *Widget->GetClass()->GetName(),
					Widget->Slot ? *Widget->Slot->GetClass()->GetName() : TEXT("-"));
			});
			return;
		}
		UE_LOG(LogStageWorkspace, Display, TEXT("DumpWidget: no live widget class contains '%s'."), *Args[0]);
	}

	/** Scrolls every vertical panel list (inspector, outliner, patch) to its end or start, so a screenshot can show rows below the fold. */
	void ScrollPanels(const TArray<FString>& Args, UWorld* World)
	{
		// "to=<WidgetName>" brings one named section to the top of the list that holds it (a screenshot of the middle of a long panel).
		if (!Args.IsEmpty() && Args[0].StartsWith(TEXT("to="), ESearchCase::IgnoreCase))
		{
			const FName Target(*Args[0].Mid(3));
			int32 Scrolled = 0;
			for (TObjectIterator<UWidget> It; It; ++It)
			{
				if (It->GetWorld() != World || It->IsTemplate() || It->GetFName() != Target)
				{
					continue;
				}
				for (UPanelWidget* Parent = It->GetParent(); Parent; Parent = Parent->GetParent())
				{
					if (UScrollBox* Box = Cast<UScrollBox>(Parent))
					{
						Box->ScrollWidgetIntoView(*It, /*AnimateScroll*/ false, EDescendantScrollDestination::TopOrLeft);
						++Scrolled;
						break;
					}
				}
			}
			UE_LOG(LogStageWorkspace, Display, TEXT("ScrollPanels to %s: %d lists."), *Target.ToString(), Scrolled);
			return;
		}
		const bool bToEnd = Args.IsEmpty() || !Args[0].Equals(TEXT("start"), ESearchCase::IgnoreCase);
		// "horizontal" scrolls the side-scrolling lists instead, such as the playback desk (its masters sit at the end).
		const EOrientation Orientation = Args.Num() > 1 && Args[1].Equals(TEXT("horizontal"), ESearchCase::IgnoreCase) ? Orient_Horizontal : Orient_Vertical;
		int32 Count = 0;
		for (TObjectIterator<UScrollBox> It; It; ++It)
		{
			if (It->GetWorld() == World && It->GetOrientation() == Orientation && !It->IsTemplate())
			{
				bToEnd ? It->ScrollToEnd() : It->ScrollToStart();
				++Count;
			}
		}
		UE_LOG(LogStageWorkspace, Display, TEXT("ScrollPanels %s: %d lists."), bToEnd ? TEXT("end") : TEXT("start"), Count);
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
	/**
	 * Shows the startup loader over the workspace for a few seconds, filling as if catalogs were loading, then fades it out.
	 * A real start is usually too quick to look at or screenshot (Docs/ADR/0024 §2).
	 */
	void PreviewLoader(const TArray<FString>& Args, UWorld* World)
	{
		const TSharedPtr<FStageWorkspaceShell> Shell = GetActiveShell(World);
		if (!Shell.IsValid())
		{
			return;
		}
		const float Seconds = Args.Num() > 0 ? FMath::Clamp(FCString::Atof(*Args[0]), 0.5f, 60.f) : 4.f;
		const double Start = FSlateApplication::Get().GetCurrentTime();
		auto Elapsed = [Start, Seconds]() { return float((FSlateApplication::Get().GetCurrentTime() - Start) / Seconds); };
		const TSharedRef<SStageBrandedLoader> Loader = SNew(SStageBrandedLoader)
			.Progress_Lambda([Elapsed]() { return TOptional<float>(FMath::Clamp(Elapsed(), 0.f, 1.f)); })
			.Status_Lambda([Elapsed]() { return FText::FromString(Elapsed() < 0.6f ? TEXT("Loading the library…") : Elapsed() < 0.9f ? TEXT("Loading the panels…") : TEXT("Loading the atmospheres…")); });
		Shell->ShowStartupOverlay(Loader);
		UE_LOG(LogStageWorkspace, Display, TEXT("PreviewLoader: shown for %.1f s."), Seconds);

		const TWeakPtr<FStageWorkspaceShell> WeakShell = Shell;
		const TWeakPtr<SStageBrandedLoader> WeakLoader = Loader;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakShell, WeakLoader](float)
		{
			if (const TSharedPtr<SStageBrandedLoader> Pinned = WeakLoader.Pin())
			{
				Pinned->FadeOut(0.35f, FSimpleDelegate::CreateLambda([WeakShell]()
				{
					if (const TSharedPtr<FStageWorkspaceShell> PinnedShell = WeakShell.Pin())
					{
						PinnedShell->CloseStartupOverlay();
					}
				}));
			}
			return false;
		}), Seconds + 0.25f);
	}

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

	/** Answers "Save changes before closing?" like its buttons: Save [Name] | Discard | Cancel. */
	void ExitPrompt(const TArray<FString>& Args, UWorld* World)
	{
		UStageWorkspaceSubsystem* Workspace = GetWorkspace(World);
		if (!Workspace || Args.IsEmpty())
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("ExitPrompt: usage Save [Name] | Discard | Cancel."));
			return;
		}
		UE_LOG(LogStageWorkspace, Display, TEXT("ExitPrompt: prompt %s."), Workspace->IsExitPromptOpen() ? TEXT("open") : TEXT("not open"));

		EStageExitChoice Choice = EStageExitChoice::Cancel;
		if (Args[0].Equals(TEXT("Save"), ESearchCase::IgnoreCase)) { Choice = EStageExitChoice::Save; }
		else if (Args[0].Equals(TEXT("Discard"), ESearchCase::IgnoreCase)) { Choice = EStageExitChoice::Discard; }
		else if (!Args[0].Equals(TEXT("Cancel"), ESearchCase::IgnoreCase))
		{
			UE_LOG(LogStageWorkspace, Display, TEXT("ExitPrompt: unknown choice %s."), *Args[0]);
			return;
		}
		Workspace->AnswerExitPrompt(Choice, FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" ")));
	}

	FAutoConsoleCommandWithWorldAndArgs ExitPromptCommand(TEXT("StageCraft.Workspace.ExitPrompt"), TEXT("Save [Name] | Discard | Cancel: answers the unsaved-changes prompt shown when the main window closes."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExitPrompt));
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
	FAutoConsoleCommandWithWorldAndArgs PreviewLoaderCommand(TEXT("StageCraft.Startup.PreviewLoader"), TEXT("[Seconds] Shows the startup loader over the workspace for a few seconds (default 4), for a look or a screenshot."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PreviewLoader));
	FAutoConsoleCommandWithWorldAndArgs DelayCommand(TEXT("StageCraft.Workspace.Delay"), TEXT("<Seconds> <Command...> Runs a console command later (test scripting)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Delay));
	FAutoConsoleCommandWithWorldAndArgs StatusCommand(TEXT("StageCraft.Workspace.Status"), TEXT("Logs workspace state, windows, viewport size and saved resolution."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Status));
	FAutoConsoleCommandWithWorldAndArgs FloatCommand(TEXT("StageCraft.Workspace.FloatViewport"), TEXT("[MonitorIndex] Floats the viewport into its own window (default: last monitor)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FloatViewport));
	FAutoConsoleCommandWithWorldAndArgs ResetCommand(TEXT("StageCraft.Workspace.Reset"), TEXT("Restores the default layout."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Reset));
	FAutoConsoleCommandWithWorldAndArgs ScreenshotCommand(TEXT("StageCraft.Workspace.Screenshot"), TEXT("Saves the main window, with all panels, to Saved/Screenshots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Screenshot));
	FAutoConsoleCommandWithWorldAndArgs ScreenshotWindowCommand(TEXT("StageCraft.Workspace.ScreenshotWindow"), TEXT("[All] Saves the main window as Slate draws it, dialogs and overlays included (All: and every floating panel window), to Saved/Screenshots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ScreenshotWindow));
	FAutoConsoleCommandWithWorldAndArgs MenuCommand(TEXT("StageCraft.Workspace.Menu"), TEXT("<Dropdown> Describe | Open | Click | Pick <Label/Submenu label>: a toolbar dropdown's entries, opened, or one picked as a click would (SketchEditMenu, SketchSnapMenu, SketchItemsMenu, SketchPhotoMenu, SourceMenu, ViewMenuButton, SnapMenuButton, TransformMenuButton, ToolsMenuButton)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Menu));
	FAutoConsoleCommandWithWorldAndArgs TypeCommand(TEXT("StageCraft.Workspace.Type"), TEXT("<Text...> | {Escape} | {Enter} | {Down} | {Up} | {Focus}: types the text, or presses the key (Focus only logs it), in the widget with keyboard focus as the keyboard would (an open menu's search, for example)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Type));
	FAutoConsoleCommandWithWorldAndArgs DumpWidgetCommand(TEXT("StageCraft.Workspace.DumpWidget"), TEXT("<ClassNamePart> Logs a live widget's tree (names, classes, slot types)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpWidget));
	FAutoConsoleCommandWithWorldAndArgs ScrollPanelsCommand(TEXT("StageCraft.Workspace.ScrollPanels"), TEXT("[end|start] [horizontal] | to=<WidgetName>: scrolls every vertical (or horizontal) panel list to its end (default) or start, or the list holding that widget until it is at the top, for screenshots."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ScrollPanels));
	FAutoConsoleCommandWithWorldAndArgs ViewportOnlyCommand(TEXT("StageCraft.Workspace.ViewportOnly"), TEXT("Shows only the viewport, filling the main window (profiling comparison)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ViewportOnly));
	FAutoConsoleCommandWithWorldAndArgs UnknownTabCommand(TEXT("StageCraft.Workspace.TestUnknownTab"), TEXT("Restores a layout that names a panel which does not exist."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TestUnknownTab));
	FAutoConsoleCommandWithWorldAndArgs ProbeCommand(TEXT("StageCraft.Workspace.ProbePick"), TEXT("Checks that cursor picking matches a centre ray in the viewport's current window."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ProbePick));
}

#endif // !UE_BUILD_SHIPPING
