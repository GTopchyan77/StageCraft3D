// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageWorkspaceSubsystem.h"

#include "Algo/StableSort.h"
#include "Blueprint/UserWidget.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/StreamableManager.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "UI/StageStatusBarWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Workspace/SStageWorkspaceMenuBar.h"
#include "Workspace/StageCraftGameEngine.h"
#include "Workspace/StageLayoutStore.h"
#include "Workspace/StagePanelDefinition.h"
#include "Workspace/StageWorkspaceShell.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageWorkspaceSubsystem)

#define LOCTEXT_NAMESPACE "StageWorkspace"

const FString UStageWorkspaceSubsystem::DefaultLayoutName(TEXT("Default"));
const FString UStageWorkspaceSubsystem::ViewportOnlyLayoutName(TEXT("Viewport Only"));

namespace StageWorkspace
{
	/** Each monitor's work area in physical pixels and divided by its DPI scale (FStageLayoutStore::ClampToWorkAreas), primary first. */
	void GetMonitorWorkAreas(TArray<FBox2D>& OutScreenAreas, TArray<FBox2D>& OutSlateAreas)
	{
		FDisplayMetrics Metrics;
		FSlateApplication::Get().GetCachedDisplayMetrics(Metrics);

		TArray<FMonitorInfo> Monitors = Metrics.MonitorInfo;
		Algo::StableSortBy(Monitors, [](const FMonitorInfo& Monitor) { return Monitor.bIsPrimary ? 0 : 1; });
		for (const FMonitorInfo& Monitor : Monitors)
		{
			const FBox2D Screen(FVector2D(Monitor.WorkArea.Left, Monitor.WorkArea.Top), FVector2D(Monitor.WorkArea.Right, Monitor.WorkArea.Bottom));
			const float DPIScale = FMath::Max(0.1f, FPlatformApplicationMisc::GetDPIScaleFactorAtPoint(Screen.GetCenter().X, Screen.GetCenter().Y));
			OutScreenAreas.Add(Screen);
			OutSlateAreas.Add(FBox2D(Screen.Min / DPIScale, Screen.Max / DPIScale));
		}
	}

	FString LexResult(EStageLayoutResult Result)
	{
		return UEnum::GetValueAsString(Result);
	}
}

bool UStageWorkspaceSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// The editor owns the viewport in PIE (ADR §5), and a server has no UI.
	return !GIsEditor && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UStageWorkspaceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LayoutStore = MakeShared<FStageLayoutStore>(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("Workspace")), FStageWorkspaceShell::PanelLayoutVersion);
	UAssetManager::CallOrRegister_OnCompletedInitialScan(FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::LoadPanelCatalog));
}

void UStageWorkspaceSubsystem::Deinitialize()
{
	// Quitting through RequestExit (the 'exit' command, a fatal map error) leaves every window alive, so the session
	// can still be captured here. Closing the main window already saved and sealed it (HandleMainWindowClosing).
	if (State == EStageWorkspaceState::Ready && Shell.IsValid() && Shell->GetViewportWindow().IsValid())
	{
		SaveSessionNow();
	}
	PendingSessionWrite.Wait();

	State = EStageWorkspaceState::ShuttingDown;
	if (Shell.IsValid())
	{
		Shell->Shutdown();
		Shell.Reset();
	}
	if (PanelCatalogHandle.IsValid())
	{
		PanelCatalogHandle->CancelHandle();
		PanelCatalogHandle.Reset();
	}
	PanelDefinitions.Reset();
	LocalController.Reset();
	Super::Deinitialize();
}

// --- Controller registration and install ---

void UStageWorkspaceSubsystem::RegisterLocalController(AModularPlayerController* Controller)
{
	if (!Controller || !Controller->IsLocalController())
	{
		return;
	}
	LocalController = Controller;

	if (State == EStageWorkspaceState::Uninitialized)
	{
		InstallIfSupported(); // builds the panels against LocalController
	}
	else if (State == EStageWorkspaceState::Ready)
	{
		Shell->RefreshPanelContent();
	}
}

void UStageWorkspaceSubsystem::UnregisterLocalController(AModularPlayerController* Controller)
{
	if (!Controller || LocalController.Get() != Controller)
	{
		return;
	}
	LocalController.Reset();

	// Release the panel widgets now: they are owned by this controller and bound to its components.
	if (State == EStageWorkspaceState::Ready)
	{
		Shell->RefreshPanelContent();
	}
}

void UStageWorkspaceSubsystem::InstallIfSupported()
{
	if (State != EStageWorkspaceState::Uninitialized)
	{
		return;
	}

	// The one engine type that can host a docked viewport (ADR F4, F12). A plain UGameEngine renders directly to the window.
	UStageCraftGameEngine* GameEngine = Cast<UStageCraftGameEngine>(GEngine);
	const TSharedPtr<SWindow> MainWindow = GameEngine ? GameEngine->GameViewportWindow.Pin() : nullptr;
	const TSharedPtr<SViewport> ViewportWidget = GameEngine ? GameEngine->GetGameViewportWidget() : nullptr;
	if (!GameEngine || !GameEngine->CanDockGameViewport() || !MainWindow.IsValid() || !ViewportWidget.IsValid())
	{
		State = EStageWorkspaceState::Unsupported;
		UE_LOG(LogStageWorkspace, Log, TEXT("Workspace unsupported in this run: %s. Using the plain HUD."),
			!GameEngine ? TEXT("GameEngine is not StageCraftGameEngine") : TEXT("the game viewport renders directly to the window (-StageDirectViewport?) or has no window"));
		return;
	}

	if (!UAssetManager::Get().HasInitialScanCompleted())
	{
		// Panels discovered later still get spawners (LoadPanelCatalog) and can be opened from the Window menu,
		// but this startup layout cannot place them. Game builds scan synchronously at startup, so this is unexpected.
		UE_LOG(LogStageWorkspace, Warning, TEXT("Workspace installing before the Asset Manager scan finished; saved panels other than the viewport may not be restored this launch."));
	}

	FStageWorkspaceShellCallbacks Callbacks;
	TWeakObjectPtr<UStageWorkspaceSubsystem> WeakThis(this);
	TWeakObjectPtr<UStageCraftGameEngine> WeakEngine(GameEngine);
	Callbacks.GetWidgetPanels = [WeakThis]()
	{
		const UStageWorkspaceSubsystem* Self = WeakThis.Get();
		return Self ? Self->ScannedPanels : TArray<FGameplayTag>();
	};
	Callbacks.GetPanelLabel = [WeakThis](const FGameplayTag& PanelTag)
	{
		const UStageWorkspaceSubsystem* Self = WeakThis.Get();
		return Self ? Self->GetPanelDisplayName(PanelTag) : FText::GetEmpty();
	};
	Callbacks.CreatePanelContent = [WeakThis](const FGameplayTag& PanelTag) -> TSharedRef<SWidget>
	{
		UStageWorkspaceSubsystem* Self = WeakThis.Get();
		return Self ? Self->CreatePanelContent(PanelTag) : SNullWidget::NullWidget;
	};
	Callbacks.CreateStatusBarContent = [WeakThis]() -> TSharedRef<SWidget>
	{
		UStageWorkspaceSubsystem* Self = WeakThis.Get();
		return Self ? Self->CreateStatusBarContent() : SNullWidget::NullWidget;
	};
	Callbacks.PanelHostChanged = [WeakThis](const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost)
	{
		if (UStageWorkspaceSubsystem* Self = WeakThis.Get())
		{
			Self->HandlePanelHostChanged(PanelTag, NewHost, PreviousHost);
		}
	};
	Callbacks.LayoutChanged = [WeakThis]()
	{
		if (UStageWorkspaceSubsystem* Self = WeakThis.Get())
		{
			Self->HandleLayoutChanged();
		}
	};
	Callbacks.MainWindowClosing = [WeakThis]()
	{
		if (UStageWorkspaceSubsystem* Self = WeakThis.Get())
		{
			Self->HandleMainWindowClosing();
		}
	};
	Callbacks.SetMainWindowContent = [WeakEngine](const TSharedRef<SWidget>& Content)
	{
		if (UStageCraftGameEngine* Engine = WeakEngine.Get())
		{
			Engine->SetMainWindowContent(Content);
		}
	};
	Callbacks.RestoreMainWindowViewport = [WeakEngine]()
	{
		if (UStageCraftGameEngine* Engine = WeakEngine.Get())
		{
			Engine->RestoreMainWindowViewport();
		}
	};

	Shell = MakeShared<FStageWorkspaceShell>(MainWindow.ToSharedRef(), ViewportWidget.ToSharedRef(), MoveTemp(Callbacks));
	State = EStageWorkspaceState::Ready;

	// Restore the last session. Any failure falls back to Default; a bad file was already moved aside by the store.
	FStageSavedLayout Session;
	const EStageLayoutResult SessionResult = LayoutStore->LoadSession(Session);
	TSharedPtr<FTabManager::FLayout> StartupLayout;
	if (SessionResult == EStageLayoutResult::Success)
	{
		TArray<FBox2D> ScreenAreas;
		TArray<FBox2D> SlateAreas;
		StageWorkspace::GetMonitorWorkAreas(ScreenAreas, SlateAreas);
		FStageLayoutStore::ClampToWorkAreas(Session, ScreenAreas, SlateAreas);
		StartupLayout = FTabManager::FLayout::NewFromString(Session.SlateLayout);
	}
	if (!StartupLayout.IsValid())
	{
		if (SessionResult != EStageLayoutResult::NotFound)
		{
			UE_LOG(LogStageWorkspace, Warning, TEXT("Last session layout not restored (%s, %s); using '%s'."),
				*StageWorkspace::LexResult(SessionResult == EStageLayoutResult::Success ? EStageLayoutResult::Corrupt : SessionResult), *LayoutStore->GetSessionPath(), *DefaultLayoutName);
		}
		Session = FStageSavedLayout();
		Session.Name = DefaultLayoutName;
		StartupLayout = FStageWorkspaceShell::MakeDefaultLayout();
	}

	Shell->Install(StartupLayout.ToSharedRef(), SNew(SStageWorkspaceMenuBar, this));
	if (Session.MainWindow.IsSet())
	{
		Shell->ApplyMainWindowPlacement(Session.MainWindow.GetValue());
	}
	ActiveLayoutName = Session.Name;
	UE_LOG(LogStageWorkspace, Log, TEXT("Workspace installed in the main window with layout '%s'%s."), *ActiveLayoutName,
		SessionResult == EStageLayoutResult::Success ? TEXT(" (restored from the last session)") : TEXT(""));
}

// --- Panels ---

void UStageWorkspaceSubsystem::LoadPanelCatalog()
{
	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FPrimaryAssetId> PanelIds;
	AssetManager.GetPrimaryAssetIdList(UStagePanelDefinition::PanelAssetType, PanelIds);

	ScannedPanels.Reset();
	for (int32 Index = PanelIds.Num() - 1; Index >= 0; --Index)
	{
		// The primary asset name is the panel tag (UStagePanelDefinition::GetPrimaryAssetId).
		const FGameplayTag PanelTag = FGameplayTag::RequestGameplayTag(PanelIds[Index].PrimaryAssetName, /*ErrorIfNotFound*/ false);
		if (!PanelTag.MatchesTag(StageCraftTags::Panel) || PanelTag == StageCraftTags::Panel || PanelTag == StageCraftTags::Panel_Viewport)
		{
			UE_LOG(LogStageWorkspace, Warning, TEXT("Panel definition %s ignored: its PanelTag is not a registered StageCraft.Panel.* tag (or is the built-in viewport)."), *PanelIds[Index].ToString());
			PanelIds.RemoveAt(Index);
			continue;
		}
		ScannedPanels.AddUnique(PanelTag);
	}
	ScannedPanels.Sort([](const FGameplayTag& A, const FGameplayTag& B) { return A.GetTagName().LexicalLess(B.GetTagName()); });

	if (State == EStageWorkspaceState::Ready)
	{
		Shell->RegisterPanelSpawners();
	}

	if (PanelIds.IsEmpty())
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("No panel definitions found. Check the StagePanel entry under AssetManagerSettings in DefaultGame.ini."));
		return;
	}
	PanelCatalogHandle = AssetManager.LoadPrimaryAssets(PanelIds, { UStagePanelDefinition::UIBundle },
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandlePanelCatalogLoaded));
}

void UStageWorkspaceSubsystem::HandlePanelCatalogLoaded()
{
	TArray<UObject*> Loaded;
	UAssetManager::Get().GetPrimaryAssetObjectList(UStagePanelDefinition::PanelAssetType, Loaded);

	PanelDefinitions.Reset();
	for (UObject* Object : Loaded)
	{
		UStagePanelDefinition* Definition = Cast<UStagePanelDefinition>(Object);
		if (Definition && ScannedPanels.Contains(Definition->PanelTag))
		{
			PanelDefinitions.Add(Definition->PanelTag, Definition);
		}
	}
	UE_LOG(LogStageWorkspace, Log, TEXT("Panel catalog loaded: %d definitions."), PanelDefinitions.Num());

	// Panels spawned before this showed a loading state.
	if (State == EStageWorkspaceState::Ready)
	{
		Shell->RefreshPanelContent();
	}
}

TArray<FGameplayTag> UStageWorkspaceSubsystem::GetAvailablePanels() const
{
	TArray<FGameplayTag> Panels;
	Panels.Reserve(ScannedPanels.Num() + 1);
	Panels.Add(StageCraftTags::Panel_Viewport);
	Panels.Append(ScannedPanels);
	return Panels;
}

FText UStageWorkspaceSubsystem::GetPanelDisplayName(FGameplayTag PanelTag) const
{
	if (PanelTag == StageCraftTags::Panel_Viewport)
	{
		return LOCTEXT("ViewportLabel", "Viewport");
	}
	const UStagePanelDefinition* Definition = PanelDefinitions.FindRef(PanelTag);
	if (Definition && !Definition->DisplayName.IsEmpty())
	{
		return Definition->DisplayName;
	}
	FString TagName = PanelTag.ToString();
	int32 LastDot = INDEX_NONE;
	if (TagName.FindLastChar(TEXT('.'), LastDot))
	{
		TagName.RightChopInline(LastDot + 1);
	}
	return FText::FromString(TagName);
}

TSharedRef<SWidget> UStageWorkspaceSubsystem::CreatePanelContent(const FGameplayTag& PanelTag)
{
	AModularPlayerController* Controller = LocalController.Get();
	const UStagePanelDefinition* Definition = PanelDefinitions.FindRef(PanelTag);
	// The UI bundle loaded with the definition, so the class is resident when the definition is.
	const TSubclassOf<UUserWidget> WidgetClass = Definition ? Definition->WidgetClass.Get() : nullptr;

	// The widget is owned by the controller: panels reach the selection and the request bridge through GetOwningPlayer.
	// The tab keeps it alive through the SObjectWidget returned by TakeWidget (ADR §3.3).
	if (Controller && WidgetClass)
	{
		if (UUserWidget* Widget = CreateWidget<UUserWidget>(Controller, WidgetClass))
		{
			return Widget->TakeWidget();
		}
	}

	FText Message;
	if (!Controller || !Definition)
	{
		Message = LOCTEXT("PanelWaiting", "Loading…");
	}
	else
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("Panel %s: widget class '%s' could not be loaded or created."), *PanelTag.ToString(), *Definition->WidgetClass.ToString());
		Message = LOCTEXT("PanelUnavailable", "This panel is not available.");
	}
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Message)
		];
}

TSharedRef<SWidget> UStageWorkspaceSubsystem::CreateStatusBarContent()
{
	// Owned by the controller like the panels, so it reaches the placement tool and the request bridge through
	// GetOwningPlayer; the slot keeps it alive through TakeWidget and drops it when the controller changes.
	AModularPlayerController* Controller = LocalController.Get();
	UStageStatusBarWidget* StatusBar = Controller ? CreateWidget<UStageStatusBarWidget>(Controller, UStageStatusBarWidget::StaticClass()) : nullptr;
	return StatusBar ? StatusBar->TakeWidget() : SNullWidget::NullWidget;
}

bool UStageWorkspaceSubsystem::OpenPanel(FGameplayTag PanelTag)
{
	return State == EStageWorkspaceState::Ready && Shell->OpenPanel(PanelTag);
}

bool UStageWorkspaceSubsystem::ClosePanel(FGameplayTag PanelTag)
{
	return State == EStageWorkspaceState::Ready && Shell->ClosePanel(PanelTag);
}

EStagePanelHost UStageWorkspaceSubsystem::GetPanelHost(FGameplayTag PanelTag) const
{
	return State == EStageWorkspaceState::Ready ? Shell->GetPanelHost(PanelTag) : EStagePanelHost::Closed;
}

void UStageWorkspaceSubsystem::HandlePanelHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost)
{
	UE_LOG(LogStageWorkspace, Log, TEXT("Panel %s: %s -> %s."), *PanelTag.ToString(), *UEnum::GetValueAsString(PreviousHost), *UEnum::GetValueAsString(NewHost));
	OnPanelHostChanged.Broadcast(PanelTag, NewHost, PreviousHost);
}

// --- Layouts ---

TArray<FString> UStageWorkspaceSubsystem::GetBuiltInLayouts() const
{
	return { DefaultLayoutName, ViewportOnlyLayoutName };
}

TArray<FString> UStageWorkspaceSubsystem::GetUserLayouts() const
{
	return LayoutStore.IsValid() ? LayoutStore->ListUserLayouts() : TArray<FString>();
}

bool UStageWorkspaceSubsystem::IsBuiltInLayout(const FString& LayoutName) const
{
	return LayoutName.Equals(DefaultLayoutName, ESearchCase::IgnoreCase) || LayoutName.Equals(ViewportOnlyLayoutName, ESearchCase::IgnoreCase);
}

EStageLayoutResult UStageWorkspaceSubsystem::ApplyLayout(const FString& LayoutName)
{
	if (State != EStageWorkspaceState::Ready)
	{
		return EStageLayoutResult::WorkspaceInactive;
	}

	if (IsBuiltInLayout(LayoutName))
	{
		const bool bDefault = LayoutName.Equals(DefaultLayoutName, ESearchCase::IgnoreCase);
		Shell->ApplyLayout(bDefault ? FStageWorkspaceShell::MakeDefaultLayout() : FStageWorkspaceShell::MakeViewportOnlyLayout());
		SetActiveLayout(bDefault ? DefaultLayoutName : ViewportOnlyLayoutName);
		return EStageLayoutResult::Success;
	}

	FStageSavedLayout Layout;
	EStageLayoutResult Result = LayoutStore->LoadUserLayout(LayoutName, Layout);
	if (Result == EStageLayoutResult::Success)
	{
		Result = ApplySavedLayout(Layout);
	}

	if (Result != EStageLayoutResult::Success)
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("ApplyLayout '%s' failed: %s. The current arrangement is kept."), *LayoutName, *StageWorkspace::LexResult(Result));
		if (Result == EStageLayoutResult::Corrupt || Result == EStageLayoutResult::VersionMismatch)
		{
			OnLayoutsChanged.Broadcast(); // the store moved the file aside
		}
		return Result;
	}
	SetActiveLayout(Layout.Name);
	return EStageLayoutResult::Success;
}

EStageLayoutResult UStageWorkspaceSubsystem::ApplySavedLayout(FStageSavedLayout& Layout)
{
	TArray<FBox2D> ScreenAreas;
	TArray<FBox2D> SlateAreas;
	StageWorkspace::GetMonitorWorkAreas(ScreenAreas, SlateAreas);
	FStageLayoutStore::ClampToWorkAreas(Layout, ScreenAreas, SlateAreas);

	const TSharedPtr<FTabManager::FLayout> PanelLayout = FTabManager::FLayout::NewFromString(Layout.SlateLayout);
	if (!PanelLayout.IsValid())
	{
		return EStageLayoutResult::Corrupt;
	}
	Shell->ApplyLayout(PanelLayout.ToSharedRef());
	if (Layout.MainWindow.IsSet())
	{
		Shell->ApplyMainWindowPlacement(Layout.MainWindow.GetValue());
	}
	return EStageLayoutResult::Success;
}

EStageLayoutResult UStageWorkspaceSubsystem::SaveCurrentLayoutAs(const FString& LayoutName)
{
	if (State != EStageWorkspaceState::Ready)
	{
		return EStageLayoutResult::WorkspaceInactive;
	}

	EStageLayoutResult Result = EStageLayoutResult::Success;
	FStageSavedLayout Layout;
	if (!FStageLayoutStore::IsValidLayoutName(LayoutName))
	{
		Result = EStageLayoutResult::InvalidName;
	}
	else if (IsBuiltInLayout(LayoutName))
	{
		Result = EStageLayoutResult::ReservedName;
	}
	else if (!CaptureCurrentLayout(LayoutName, Layout))
	{
		Result = EStageLayoutResult::WorkspaceInactive;
	}
	else
	{
		Result = LayoutStore->SaveUserLayout(Layout);
	}

	if (Result != EStageLayoutResult::Success)
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("SaveCurrentLayoutAs '%s' failed: %s."), *LayoutName, *StageWorkspace::LexResult(Result));
		return Result;
	}

	UE_LOG(LogStageWorkspace, Log, TEXT("Layout '%s' saved to %s."), *LayoutName, *LayoutStore->GetUserLayoutPath(LayoutName));
	ActiveLayoutName = LayoutName;
	OnLayoutsChanged.Broadcast();
	SaveSessionAsync();
	return EStageLayoutResult::Success;
}

EStageLayoutResult UStageWorkspaceSubsystem::DeleteLayout(const FString& LayoutName)
{
	if (State != EStageWorkspaceState::Ready)
	{
		return EStageLayoutResult::WorkspaceInactive;
	}

	const EStageLayoutResult Result = IsBuiltInLayout(LayoutName) ? EStageLayoutResult::ReservedName : LayoutStore->DeleteUserLayout(LayoutName);
	if (Result != EStageLayoutResult::Success)
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("DeleteLayout '%s' failed: %s."), *LayoutName, *StageWorkspace::LexResult(Result));
		return Result;
	}

	UE_LOG(LogStageWorkspace, Log, TEXT("Layout '%s' deleted."), *LayoutName);
	if (ActiveLayoutName.Equals(LayoutName, ESearchCase::IgnoreCase))
	{
		ActiveLayoutName.Reset();
		SaveSessionAsync();
	}
	OnLayoutsChanged.Broadcast();
	return EStageLayoutResult::Success;
}

void UStageWorkspaceSubsystem::ResetToDefaultLayout()
{
	ApplyLayout(DefaultLayoutName);
}

void UStageWorkspaceSubsystem::SetActiveLayout(const FString& LayoutName)
{
	ActiveLayoutName = LayoutName;
	UE_LOG(LogStageWorkspace, Log, TEXT("Layout '%s' applied."), *LayoutName);
	SaveSessionAsync();
	OnLayoutApplied.Broadcast(ActiveLayoutName);
}

bool UStageWorkspaceSubsystem::CaptureCurrentLayout(const FString& Name, FStageSavedLayout& OutLayout) const
{
	const TSharedPtr<FTabManager::FLayout> PanelLayout = Shell.IsValid() ? Shell->CaptureLayout() : nullptr;
	if (!PanelLayout.IsValid())
	{
		return false;
	}
	OutLayout.Name = Name;
	OutLayout.SlateLayout = PanelLayout->ToString();
	OutLayout.MainWindow = Shell->GetMainWindowPlacement();
	return true;
}

// --- Session persistence ---

void UStageWorkspaceSubsystem::HandleLayoutChanged()
{
	SaveSessionAsync();
}

void UStageWorkspaceSubsystem::HandleMainWindowClosing()
{
	SaveSessionNow();
}

void UStageWorkspaceSubsystem::SaveSessionAsync()
{
	FStageSavedLayout Session;
	if (bSessionSealed || State != EStageWorkspaceState::Ready || !CaptureCurrentLayout(ActiveLayoutName, Session))
	{
		return;
	}

	// Only strings cross to the worker; it never touches this object (Rules §29).
	PendingSessionWrite = UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Path = LayoutStore->GetSessionPath(), Contents = LayoutStore->Serialize(Session)]()
		{
			if (!FStageLayoutStore::WriteFileAtomic(Path, Contents))
			{
				UE_LOG(LogStageWorkspace, Warning, TEXT("Could not write the session layout to %s."), *Path);
			}
		},
		UE::Tasks::Prerequisites(PendingSessionWrite));
}

void UStageWorkspaceSubsystem::SaveSessionNow()
{
	FStageSavedLayout Session;
	if (bSessionSealed || State != EStageWorkspaceState::Ready || !CaptureCurrentLayout(ActiveLayoutName, Session))
	{
		return;
	}
	bSessionSealed = true;

	PendingSessionWrite.Wait();
	const FString Path = LayoutStore->GetSessionPath();
	if (FStageLayoutStore::WriteFileAtomic(Path, LayoutStore->Serialize(Session)))
	{
		UE_LOG(LogStageWorkspace, Log, TEXT("Session layout saved to %s."), *Path);
	}
	else
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("Could not write the session layout to %s."), *Path);
	}
}

#undef LOCTEXT_NAMESPACE
