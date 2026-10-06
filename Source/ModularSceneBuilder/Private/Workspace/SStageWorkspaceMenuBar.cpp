// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/SStageWorkspaceMenuBar.h"

#include "Actors/ModularTransformGizmo.h"
#include "Audio/StageAudioSubsystem.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameplayTagContainer.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Player/ModularPlayerController.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Workspace/StageLayoutStore.h"
#include "Workspace/StageWorkspaceSubsystem.h"

#define LOCTEXT_NAMESPACE "StageWorkspaceMenuBar"

namespace StageWorkspaceMenuBar
{
	constexpr float SliderWidth = 160.f;
}

void SStageWorkspaceMenuBar::Construct(const FArguments& InArgs, UStageWorkspaceSubsystem* InWorkspace)
{
	Workspace = InWorkspace;

	FMenuBarBuilder MenuBar(nullptr);
	MenuBar.AddPullDownMenu(LOCTEXT("EditMenu", "Edit"), LOCTEXT("EditMenuTip", "Select or place items, and choose the transform tool."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillEditMenu));
	MenuBar.AddPullDownMenu(LOCTEXT("WindowMenu", "Window"), LOCTEXT("WindowMenuTip", "Open, focus or close panels."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillWindowMenu));
	MenuBar.AddPullDownMenu(LOCTEXT("LayoutMenu", "Layout"), LOCTEXT("LayoutMenuTip", "Load, save and delete workspace layouts."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillLayoutMenu));
	MenuBar.AddPullDownMenu(LOCTEXT("AudioMenu", "Audio"), LOCTEXT("AudioMenuTip", "Master and effects volume, mute."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillAudioMenu));

	ChildSlot
	[
		MenuBar.MakeWidget()
	];
}

AModularPlayerController* SStageWorkspaceMenuBar::GetStageController() const
{
	const UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	const UGameInstance* GameInstance = WorkspacePtr ? WorkspacePtr->GetGameInstance() : nullptr;
	return GameInstance ? Cast<AModularPlayerController>(GameInstance->GetFirstLocalPlayerController()) : nullptr;
}

UStageAudioSubsystem* SStageWorkspaceMenuBar::GetAudio() const
{
	const UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	const UGameInstance* GameInstance = WorkspacePtr ? WorkspacePtr->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UStageAudioSubsystem>() : nullptr;
}

void SStageWorkspaceMenuBar::FillEditMenu(FMenuBuilder& MenuBuilder)
{
	// Weak: the controller is replaced on level travel while this menu bar lives on.
	const TWeakObjectPtr<AModularPlayerController> WeakController = GetStageController();
	if (!WeakController.IsValid())
	{
		return;
	}

	MenuBuilder.BeginSection(TEXT("EditMode"), LOCTEXT("ModeSection", "Mode"));
	const auto AddModeEntry = [&MenuBuilder, WeakController](EStageEditMode Mode, const FText& Label, const FText& Tip)
	{
		MenuBuilder.AddMenuEntry(Label, Tip, FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakController, Mode]()
				{
					if (UStagePlacementToolComponent* Tool = WeakController.IsValid() ? WeakController->GetPlacementTool() : nullptr)
					{
						Mode == EStageEditMode::Place ? Tool->EnterPlaceMode() : Tool->EnterSelectMode();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakController, Mode]()
				{
					const UStagePlacementToolComponent* Tool = WeakController.IsValid() ? WeakController->GetPlacementTool() : nullptr;
					return Tool && Tool->GetEditMode() == Mode;
				})),
			NAME_None, EUserInterfaceActionType::RadioButton);
	};
	AddModeEntry(EStageEditMode::Select, LOCTEXT("SelectMode", "Select"), LOCTEXT("SelectModeTip", "Click items to select them; edit with the gizmo or the Inspector."));
	AddModeEntry(EStageEditMode::Place, LOCTEXT("PlaceMode", "Place  (P)"), LOCTEXT("PlaceModeTip", "Show the armed Library item under the cursor; every click places a copy until you leave Place mode (Esc or P)."));
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection(TEXT("TransformTool"), LOCTEXT("TransformSection", "Transform Tool  (Space cycles)"));
	const auto AddGizmoEntry = [&MenuBuilder, WeakController](EGizmoMode GizmoMode, const FText& Label)
	{
		MenuBuilder.AddMenuEntry(Label, FText::GetEmpty(), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakController, GizmoMode]()
				{
					if (AModularTransformGizmo* Gizmo = WeakController.IsValid() ? WeakController->GetGizmo() : nullptr)
					{
						Gizmo->SetMode(GizmoMode);
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakController, GizmoMode]()
				{
					const AModularTransformGizmo* Gizmo = WeakController.IsValid() ? WeakController->GetGizmo() : nullptr;
					return Gizmo && Gizmo->GetMode() == GizmoMode;
				})),
			NAME_None, EUserInterfaceActionType::RadioButton);
	};
	AddGizmoEntry(EGizmoMode::Translate, LOCTEXT("MoveTool", "Move"));
	AddGizmoEntry(EGizmoMode::Rotate, LOCTEXT("RotateTool", "Rotate"));
	AddGizmoEntry(EGizmoMode::Scale, LOCTEXT("ScaleTool", "Scale"));
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillAudioMenu(FMenuBuilder& MenuBuilder)
{
	const TWeakObjectPtr<UStageAudioSubsystem> WeakAudio = GetAudio();
	if (!WeakAudio.IsValid())
	{
		return;
	}

	MenuBuilder.AddMenuEntry(
		LOCTEXT("Mute", "Mute All"),
		LOCTEXT("MuteTip", "Silence every sound. Volumes are kept for when you unmute."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakAudio]() { if (WeakAudio.IsValid()) { WeakAudio->ToggleMute(); } }),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([WeakAudio]() { return WeakAudio.IsValid() && WeakAudio->IsMuted(); })),
		NAME_None, EUserInterfaceActionType::ToggleButton);

	// Sliders apply live through the subsystem's clamping setters and always show the stored value back.
	const auto MakeVolumeSlider = [WeakAudio](float (UStageAudioSubsystem::*Getter)() const, void (UStageAudioSubsystem::*Setter)(float))
	{
		return SNew(SBox)
			.WidthOverride(StageWorkspaceMenuBar::SliderWidth)
			[
				SNew(SSlider)
				.Value_Lambda([WeakAudio, Getter]() { return WeakAudio.IsValid() ? (WeakAudio.Get()->*Getter)() : 0.f; })
				.OnValueChanged_Lambda([WeakAudio, Setter](float NewValue) { if (WeakAudio.IsValid()) { (WeakAudio.Get()->*Setter)(NewValue); } })
			];
	};

	MenuBuilder.BeginSection(TEXT("Volume"), LOCTEXT("VolumeSection", "Volume"));
	MenuBuilder.AddWidget(MakeVolumeSlider(&UStageAudioSubsystem::GetMasterVolume, &UStageAudioSubsystem::SetMasterVolume), LOCTEXT("Master", "Master"));
	MenuBuilder.AddWidget(MakeVolumeSlider(&UStageAudioSubsystem::GetEffectsVolume, &UStageAudioSubsystem::SetEffectsVolume), LOCTEXT("Effects", "Effects"));
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillWindowMenu(FMenuBuilder& MenuBuilder)
{
	UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	if (!WorkspacePtr)
	{
		return;
	}

	MenuBuilder.BeginSection(TEXT("Panels"), LOCTEXT("PanelsSection", "Panels"));
	for (const FGameplayTag& PanelTag : WorkspacePtr->GetAvailablePanels())
	{
		const bool bIsViewport = PanelTag == StageCraftTags::Panel_Viewport;
		const TWeakObjectPtr<UStageWorkspaceSubsystem> WeakWorkspace = Workspace;
		MenuBuilder.AddMenuEntry(
			WorkspacePtr->GetPanelDisplayName(PanelTag),
			bIsViewport ? LOCTEXT("ViewportTip", "Bring the 3D viewport to the front. It can float but not close.") : LOCTEXT("PanelTip", "Show or hide this panel."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SStageWorkspaceMenuBar::TogglePanel, PanelTag),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakWorkspace, PanelTag]()
				{
					const UStageWorkspaceSubsystem* Subsystem = WeakWorkspace.Get();
					return Subsystem && Subsystem->GetPanelHost(PanelTag) != EStagePanelHost::Closed;
				})),
			NAME_None,
			EUserInterfaceActionType::ToggleButton);
	}
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection(TEXT("WindowLayout"));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ResetLayout", "Reset Layout"),
		LOCTEXT("ResetLayoutTip", "Restore the Default layout. The main window stays where it is."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SStageWorkspaceMenuBar::ApplyLayout, UStageWorkspaceSubsystem::DefaultLayoutName)));
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillLayoutMenu(FMenuBuilder& MenuBuilder)
{
	UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	if (!WorkspacePtr)
	{
		return;
	}

	const FString& ActiveLayout = WorkspacePtr->GetActiveLayoutName();
	const auto AddLayoutEntry = [this, &MenuBuilder, &ActiveLayout](const FString& LayoutName)
	{
		MenuBuilder.AddMenuEntry(
			FText::FromString(LayoutName),
			FText::Format(LOCTEXT("ApplyLayoutTip", "Replace the current arrangement with '{0}'."), FText::FromString(LayoutName)),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SStageWorkspaceMenuBar::ApplyLayout, LayoutName),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([bActive = LayoutName.Equals(ActiveLayout, ESearchCase::IgnoreCase)]() { return bActive; })),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	};

	MenuBuilder.BeginSection(TEXT("BuiltIn"), LOCTEXT("BuiltInSection", "Built-in Layouts"));
	for (const FString& LayoutName : WorkspacePtr->GetBuiltInLayouts())
	{
		AddLayoutEntry(LayoutName);
	}
	MenuBuilder.EndSection();

	const TArray<FString> UserLayouts = WorkspacePtr->GetUserLayouts();
	MenuBuilder.BeginSection(TEXT("User"), LOCTEXT("UserSection", "My Layouts"));
	for (const FString& LayoutName : UserLayouts)
	{
		AddLayoutEntry(LayoutName);
	}
	if (UserLayouts.IsEmpty())
	{
		MenuBuilder.AddMenuEntry(LOCTEXT("NoUserLayouts", "No saved layouts"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
	}
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection(TEXT("Manage"));
	MenuBuilder.AddSubMenu(LOCTEXT("SaveAs", "Save Layout As..."), LOCTEXT("SaveAsTip", "Save the current panels, floating windows and main window position under a name."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillSaveAsMenu));
	MenuBuilder.AddSubMenu(LOCTEXT("Delete", "Delete Layout"), LOCTEXT("DeleteTip", "Delete one of your saved layouts."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillDeleteMenu),
		FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([bAny = !UserLayouts.IsEmpty()]() { return bAny; })),
		NAME_None, EUserInterfaceActionType::Button);
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillSaveAsMenu(FMenuBuilder& MenuBuilder)
{
	const UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	const FString ActiveLayout = WorkspacePtr ? WorkspacePtr->GetActiveLayoutName() : FString();

	// Suggest the active user layout, so "save my changes" is Save As + Enter. Built-ins are reserved names, so they are not suggested.
	const TSharedRef<SEditableTextBox> TextBox = SNew(SEditableTextBox)
		.HintText(LOCTEXT("SaveAsHint", "Layout name"))
		.Text(WorkspacePtr && !WorkspacePtr->IsBuiltInLayout(ActiveLayout) ? FText::FromString(ActiveLayout) : FText::GetEmpty())
		.SelectAllTextWhenFocused(true)
		.ClearKeyboardFocusOnCommit(false)
		.OnTextCommitted(this, &SStageWorkspaceMenuBar::HandleSaveAsCommitted);
	SaveAsTextBox = TextBox;

	MenuBuilder.AddWidget(SNew(SBox).MinDesiredWidth(220.f)[TextBox], FText::GetEmpty(), /*bNoIndent*/ true);
}

void SStageWorkspaceMenuBar::FillDeleteMenu(FMenuBuilder& MenuBuilder)
{
	const UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	if (!WorkspacePtr)
	{
		return;
	}
	for (const FString& LayoutName : WorkspacePtr->GetUserLayouts())
	{
		MenuBuilder.AddMenuEntry(FText::FromString(LayoutName),
			FText::Format(LOCTEXT("DeleteLayoutTip", "Delete the saved layout '{0}'. The current arrangement is not changed."), FText::FromString(LayoutName)),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(this, &SStageWorkspaceMenuBar::DeleteLayout, LayoutName)));
	}
}

void SStageWorkspaceMenuBar::TogglePanel(FGameplayTag PanelTag)
{
	UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	if (!WorkspacePtr)
	{
		return;
	}
	// The viewport never closes; its entry only brings it to the front.
	const bool bOpen = WorkspacePtr->GetPanelHost(PanelTag) != EStagePanelHost::Closed;
	if (bOpen && PanelTag != StageCraftTags::Panel_Viewport)
	{
		WorkspacePtr->ClosePanel(PanelTag);
	}
	else
	{
		WorkspacePtr->OpenPanel(PanelTag);
	}
}

void SStageWorkspaceMenuBar::ApplyLayout(FString LayoutName)
{
	if (UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get())
	{
		WorkspacePtr->ApplyLayout(LayoutName); // failures are logged and keep the current arrangement
	}
}

void SStageWorkspaceMenuBar::DeleteLayout(FString LayoutName)
{
	if (UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get())
	{
		WorkspacePtr->DeleteLayout(LayoutName);
	}
}

void SStageWorkspaceMenuBar::HandleSaveAsCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get();
	if (CommitType != ETextCommit::OnEnter || !WorkspacePtr)
	{
		return;
	}

	const EStageLayoutResult Result = WorkspacePtr->SaveCurrentLayoutAs(Text.ToString().TrimStartAndEnd());
	if (Result == EStageLayoutResult::Success)
	{
		FSlateApplication::Get().DismissAllMenus();
	}
	else if (const TSharedPtr<SEditableTextBox> TextBox = SaveAsTextBox.Pin())
	{
		TextBox->SetError(DescribeResult(Result));
	}
}

FText SStageWorkspaceMenuBar::DescribeResult(EStageLayoutResult Result)
{
	switch (Result)
	{
	case EStageLayoutResult::InvalidName:
		return FText::Format(LOCTEXT("InvalidName", "Use 1-{0} letters, digits, spaces, '-', '_' or brackets."), FText::AsNumber(FStageLayoutStore::MaxNameLength));
	case EStageLayoutResult::ReservedName:
		return LOCTEXT("ReservedName", "Built-in layouts cannot be overwritten. Choose another name.");
	case EStageLayoutResult::WriteFailed:
		return LOCTEXT("WriteFailed", "The layout could not be written to disk.");
	case EStageLayoutResult::WorkspaceInactive:
		return LOCTEXT("Inactive", "The workspace is not active.");
	default:
		return FText::FromString(UEnum::GetValueAsString(Result));
	}
}

#undef LOCTEXT_NAMESPACE
