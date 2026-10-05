// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/StageWorkspaceSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Player/ModularPlayerController.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Workspace/StageCraftGameEngine.h"
#include "Workspace/StageWorkspaceShell.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageWorkspaceSubsystem)

#define LOCTEXT_NAMESPACE "StageWorkspace"

bool UStageWorkspaceSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// The editor owns the viewport in PIE (ADR §5), and a server has no UI.
	return !GIsEditor && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UStageWorkspaceSubsystem::Deinitialize()
{
	State = EStageWorkspaceState::ShuttingDown;
	if (Shell.IsValid())
	{
		Shell->Shutdown();
		Shell.Reset();
	}
	LocalController.Reset();
	Super::Deinitialize();
}

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

	FStageWorkspaceShellCallbacks Callbacks;
	TWeakObjectPtr<UStageWorkspaceSubsystem> WeakThis(this);
	TWeakObjectPtr<UStageCraftGameEngine> WeakEngine(GameEngine);
	Callbacks.CreatePanelContent = [WeakThis](const FGameplayTag& PanelTag) -> TSharedRef<SWidget>
	{
		UStageWorkspaceSubsystem* Self = WeakThis.Get();
		return Self ? Self->CreatePanelContent(PanelTag) : SNullWidget::NullWidget;
	};
	Callbacks.PanelHostChanged = [WeakThis](const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost)
	{
		if (UStageWorkspaceSubsystem* Self = WeakThis.Get())
		{
			Self->HandlePanelHostChanged(PanelTag, NewHost, PreviousHost);
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
	Shell->Install(FStageWorkspaceShell::MakeDefaultLayout());
	UE_LOG(LogStageWorkspace, Log, TEXT("Workspace installed in the main window."));
}

TSubclassOf<UUserWidget> UStageWorkspaceSubsystem::ResolvePanelWidgetClass(const FGameplayTag& PanelTag) const
{
	const TSoftClassPtr<UUserWidget>* SoftClass = nullptr;
	if (PanelTag == StageCraftTags::Panel_Inspector)
	{
		SoftClass = &InspectorPanelClass;
	}
	else if (PanelTag == StageCraftTags::Panel_FaderBank)
	{
		SoftClass = &FaderBankPanelClass;
	}

	if (!SoftClass || SoftClass->IsNull())
	{
		UE_LOG(LogStageWorkspace, Warning, TEXT("No widget class configured for panel %s ([/Script/ModularSceneBuilder.StageWorkspaceSubsystem] in DefaultGame.ini)."), *PanelTag.ToString());
		return nullptr;
	}
	return SoftClass->LoadSynchronous();
}

TSharedRef<SWidget> UStageWorkspaceSubsystem::CreatePanelContent(const FGameplayTag& PanelTag)
{
	AModularPlayerController* Controller = LocalController.Get();
	TSubclassOf<UUserWidget> WidgetClass = Controller ? ResolvePanelWidgetClass(PanelTag) : nullptr;

	// The widget is owned by the controller: panels reach the selection and the request bridge through GetOwningPlayer.
	// The tab keeps it alive through the SObjectWidget returned by TakeWidget (ADR §3.3).
	UUserWidget* Widget = WidgetClass ? CreateWidget<UUserWidget>(Controller, WidgetClass) : nullptr;
	if (Widget)
	{
		return Widget->TakeWidget();
	}

	const FText Message = Controller
		? LOCTEXT("PanelUnavailable", "This panel is not available.")
		: LOCTEXT("PanelWaiting", "Loading stage…");
	return SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Message)
		];
}

bool UStageWorkspaceSubsystem::OpenPanel(FGameplayTag PanelTag)
{
	return State == EStageWorkspaceState::Ready && Shell->OpenPanel(PanelTag);
}

EStagePanelHost UStageWorkspaceSubsystem::GetPanelHost(FGameplayTag PanelTag) const
{
	return State == EStageWorkspaceState::Ready ? Shell->GetPanelHost(PanelTag) : EStagePanelHost::Closed;
}

void UStageWorkspaceSubsystem::ResetToDefaultLayout()
{
	if (State == EStageWorkspaceState::Ready)
	{
		Shell->ApplyLayout(FStageWorkspaceShell::MakeDefaultLayout());
	}
}

void UStageWorkspaceSubsystem::HandlePanelHostChanged(const FGameplayTag& PanelTag, EStagePanelHost NewHost, EStagePanelHost PreviousHost)
{
	UE_LOG(LogStageWorkspace, Log, TEXT("Panel %s: %s -> %s."), *PanelTag.ToString(), *UEnum::GetValueAsString(PreviousHost), *UEnum::GetValueAsString(NewHost));
	OnPanelHostChanged.Broadcast(PanelTag, NewHost, PreviousHost);
}

#undef LOCTEXT_NAMESPACE
