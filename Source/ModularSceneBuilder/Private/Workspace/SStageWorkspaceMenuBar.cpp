// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/SStageWorkspaceMenuBar.h"

#include "Actors/ModularTransformGizmo.h"
#include "Audio/StageAudioSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Components/SelectionComponent.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameplayTagContainer.h"
#include "History/StageEditHistoryComponent.h"
#include "History/StageEditHistorySubsystem.h"
#include "Placement/StagePlacementToolComponent.h"
#include "Placement/StageSnappingComponent.h"
#include "Player/ModularPlayerController.h"
#include "Render/StageRenderSubsystem.h"
#include "Scene/StageSceneComponent.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
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
	MenuBar.AddPullDownMenu(LOCTEXT("FileMenu", "File"), LOCTEXT("FileMenuTip", "Save the stage as a scene, open or delete saved scenes."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillFileMenu));
	MenuBar.AddPullDownMenu(LOCTEXT("EditMenu", "Edit"), LOCTEXT("EditMenuTip", "Undo and redo, select, delete or clear items, place items, choose the transform tool and snapping."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillEditMenu));
	MenuBar.AddPullDownMenu(LOCTEXT("RenderMenu", "Render"), LOCTEXT("RenderMenuTip", "Render high-resolution stills of the current view."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillRenderMenu));
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

void SStageWorkspaceMenuBar::OpenFolder(const FString& Directory)
{
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
	FPlatformProcess::ExploreFolder(*Directory);
}

void SStageWorkspaceMenuBar::FillFileMenu(FMenuBuilder& MenuBuilder)
{
	const TWeakObjectPtr<AModularPlayerController> WeakController = GetStageController();
	const UStageSceneComponent* Scenes = WeakController.IsValid() ? WeakController->GetSceneFiles() : nullptr;
	const UStageSessionSubsystem* Session = WeakController.IsValid() ? UWorld::GetSubsystem<UStageSessionSubsystem>(WeakController->GetWorld()) : nullptr;
	if (!Scenes || !Session)
	{
		return;
	}

	const FString CurrentScene = Session->GetSceneName();
	const bool bBusy = Scenes->IsBusy();
	const bool bHasScenes = !Scenes->GetSceneNames().IsEmpty();

	// The heading says what is open and whether it has unsaved changes, like a document title.
	const FText Heading = CurrentScene.IsEmpty()
		? (Session->IsDirty() ? LOCTEXT("UntitledDirty", "Untitled stage  (unsaved)") : LOCTEXT("Untitled", "Untitled stage"))
		: FText::Format(Session->IsDirty() ? LOCTEXT("SceneDirty", "{0}  (unsaved changes)") : LOCTEXT("SceneClean", "{0}"), FText::FromString(CurrentScene));
	MenuBuilder.BeginSection(TEXT("Scene"), Heading);
	MenuBuilder.AddMenuEntry(
		CurrentScene.IsEmpty() ? LOCTEXT("SaveScene", "Save Scene") : FText::Format(LOCTEXT("SaveSceneNamed", "Save \"{0}\""), FText::FromString(CurrentScene)),
		LOCTEXT("SaveSceneTip", "Write the stage to its scene file. A stage that was never saved needs Save Scene As first."), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakController, CurrentScene]() { if (WeakController.IsValid()) { WeakController->RequestSaveScene(CurrentScene); } }),
			FCanExecuteAction::CreateLambda([bCanSave = !CurrentScene.IsEmpty() && !bBusy]() { return bCanSave; })));
	MenuBuilder.AddSubMenu(LOCTEXT("SaveSceneAs", "Save Scene As..."),
		LOCTEXT("SaveSceneAsTip", "Save every placed item (position, rotation, scale, settings) under a name."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillSaveSceneAsMenu));
	MenuBuilder.AddSubMenu(LOCTEXT("OpenScene", "Open Scene"),
		LOCTEXT("OpenSceneTip", "Replace the stage with a saved scene."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillOpenSceneMenu),
		FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([bCanOpen = bHasScenes && !bBusy]() { return bCanOpen; })),
		NAME_None, EUserInterfaceActionType::Button);
	MenuBuilder.AddSubMenu(LOCTEXT("DeleteScene", "Delete Scene"),
		LOCTEXT("DeleteSceneTip", "Delete a saved scene file. The stage on screen is not changed."),
		FNewMenuDelegate::CreateSP(this, &SStageWorkspaceMenuBar::FillDeleteSceneMenu),
		FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([bCanDelete = bHasScenes && !bBusy]() { return bCanDelete; })),
		NAME_None, EUserInterfaceActionType::Button);
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection(TEXT("SceneFolder"));
	MenuBuilder.AddMenuEntry(LOCTEXT("OpenScenesFolder", "Open Scenes Folder"), FText::FromString(Scenes->GetScenesDirectory()), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Directory = Scenes->GetScenesDirectory()]() { OpenFolder(Directory); })));
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillSaveSceneAsMenu(FMenuBuilder& MenuBuilder)
{
	const AModularPlayerController* Controller = GetStageController();
	const UStageSessionSubsystem* Session = Controller ? UWorld::GetSubsystem<UStageSessionSubsystem>(Controller->GetWorld()) : nullptr;
	PendingSceneOverwrite.Reset();

	const TSharedRef<SEditableTextBox> TextBox = SNew(SEditableTextBox)
		.HintText(LOCTEXT("SaveSceneAsHint", "Scene name"))
		.Text(Session ? FText::FromString(Session->GetSceneName()) : FText::GetEmpty())
		.SelectAllTextWhenFocused(true)
		.ClearKeyboardFocusOnCommit(false)
		.OnTextCommitted(this, &SStageWorkspaceMenuBar::HandleSaveSceneAsCommitted);
	SaveSceneAsTextBox = TextBox;

	MenuBuilder.AddWidget(SNew(SBox).MinDesiredWidth(220.f)[TextBox], FText::GetEmpty(), /*bNoIndent*/ true);
}

void SStageWorkspaceMenuBar::HandleSaveSceneAsCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	AModularPlayerController* Controller = GetStageController();
	const UStageSceneComponent* Scenes = Controller ? Controller->GetSceneFiles() : nullptr;
	const TSharedPtr<SEditableTextBox> TextBox = SaveSceneAsTextBox.Pin();
	if (CommitType != ETextCommit::OnEnter || !Scenes || !TextBox.IsValid())
	{
		return;
	}

	const FString Name = Text.ToString().TrimStartAndEnd();
	if (!FStageSceneStore::IsValidSceneName(Name))
	{
		TextBox->SetError(UStageSceneComponent::DescribeFailure(EStageSceneOperation::Save, EStageSceneResult::InvalidName, Name));
		return;
	}

	// Replacing another saved scene loses it, so the first Enter on an existing name only warns.
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(Controller->GetWorld());
	const bool bIsCurrentScene = Session && Session->GetSceneName().Equals(Name, ESearchCase::IgnoreCase);
	if (Scenes->SceneExists(Name) && !bIsCurrentScene && !PendingSceneOverwrite.Equals(Name, ESearchCase::IgnoreCase))
	{
		PendingSceneOverwrite = Name;
		TextBox->SetError(FText::Format(LOCTEXT("SceneExists", "\"{0}\" already exists. Press Enter again to replace it."), FText::FromString(Name)));
		return;
	}

	PendingSceneOverwrite.Reset();
	const EStageSceneResult Result = Controller->RequestSaveScene(Name);
	if (Result == EStageSceneResult::Success)
	{
		FSlateApplication::Get().DismissAllMenus();
	}
	else
	{
		TextBox->SetError(UStageSceneComponent::DescribeFailure(EStageSceneOperation::Save, Result, Name));
	}
}

void SStageWorkspaceMenuBar::FillOpenSceneMenu(FMenuBuilder& MenuBuilder)
{
	const TWeakObjectPtr<AModularPlayerController> WeakController = GetStageController();
	const UStageSceneComponent* Scenes = WeakController.IsValid() ? WeakController->GetSceneFiles() : nullptr;
	const UStageSessionSubsystem* Session = WeakController.IsValid() ? UWorld::GetSubsystem<UStageSessionSubsystem>(WeakController->GetWorld()) : nullptr;
	if (!Scenes || !Session)
	{
		return;
	}

	const bool bDirty = Session->IsDirty();
	for (const FString& Name : Scenes->GetSceneNames())
	{
		const FExecuteAction Open = FExecuteAction::CreateLambda([WeakController, Name]() { if (WeakController.IsValid()) { WeakController->RequestLoadScene(Name); } });
		if (!bDirty)
		{
			MenuBuilder.AddMenuEntry(FText::FromString(Name), FText::Format(LOCTEXT("OpenSceneEntryTip", "Replace the stage with \"{0}\"."), FText::FromString(Name)),
				FSlateIcon(), FUIAction(Open));
			continue;
		}

		// Unsaved changes would be lost: opening asks for one more, explicit click.
		MenuBuilder.AddSubMenu(FText::FromString(Name), FText::Format(LOCTEXT("OpenSceneDirtyTip", "The stage has unsaved changes. Opening \"{0}\" discards them."), FText::FromString(Name)),
			FNewMenuDelegate::CreateLambda([Name, Open](FMenuBuilder& ConfirmMenu)
			{
				ConfirmMenu.AddMenuEntry(FText::Format(LOCTEXT("DiscardAndOpen", "Discard unsaved changes and open \"{0}\""), FText::FromString(Name)),
					FText::GetEmpty(), FSlateIcon(), FUIAction(Open));
			}));
	}
}

void SStageWorkspaceMenuBar::FillDeleteSceneMenu(FMenuBuilder& MenuBuilder)
{
	const TWeakObjectPtr<AModularPlayerController> WeakController = GetStageController();
	const UStageSceneComponent* Scenes = WeakController.IsValid() ? WeakController->GetSceneFiles() : nullptr;
	if (!Scenes)
	{
		return;
	}

	for (const FString& Name : Scenes->GetSceneNames())
	{
		// A file deletion cannot be undone, so it takes a second, explicit click.
		MenuBuilder.AddSubMenu(FText::FromString(Name), FText::Format(LOCTEXT("DeleteSceneEntryTip", "Delete the scene file \"{0}\"."), FText::FromString(Name)),
			FNewMenuDelegate::CreateLambda([WeakController, Name](FMenuBuilder& ConfirmMenu)
			{
				ConfirmMenu.AddMenuEntry(FText::Format(LOCTEXT("DeleteSceneConfirm", "Delete \"{0}\" permanently"), FText::FromString(Name)), FText::GetEmpty(), FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([WeakController, Name]() { if (WeakController.IsValid()) { WeakController->RequestDeleteScene(Name); } })));
			}));
	}
}

void SStageWorkspaceMenuBar::FillRenderMenu(FMenuBuilder& MenuBuilder)
{
	const TWeakObjectPtr<AModularPlayerController> WeakController = GetStageController();
	const UStageRenderSubsystem* Render = WeakController.IsValid() ? UWorld::GetSubsystem<UStageRenderSubsystem>(WeakController->GetWorld()) : nullptr;
	if (!Render)
	{
		return;
	}

	const FStageRenderSettings Settings = Render->GetSettings();
	const FIntPoint Size = StageRender::GetOutputSize(Settings.Resolution);
	MenuBuilder.BeginSection(TEXT("Render"), FText::Format(LOCTEXT("RenderSection", "Render  ({0} x {1}, {2})"),
		FText::AsNumber(Size.X), FText::AsNumber(Size.Y), StaticEnum<EStageRenderAntiAliasing>()->GetDisplayNameTextByValue(int64(Settings.AntiAliasing))));
	MenuBuilder.AddMenuEntry(LOCTEXT("RenderImage", "Render Image"),
		LOCTEXT("RenderImageTip", "Render the current view with the Render panel's settings. You can keep working while it renders."), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakController]() { if (WeakController.IsValid()) { WeakController->RequestRender(); } }),
			FCanExecuteAction::CreateLambda([bIdle = !Render->IsRendering()]() { return bIdle; })));
	MenuBuilder.AddMenuEntry(LOCTEXT("RenderPanel", "Render Settings..."), LOCTEXT("RenderPanelTip", "Show the Render panel: resolution, anti-aliasing, post-processing, watermark."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateSP(this, &SStageWorkspaceMenuBar::ShowPanel, FGameplayTag(StageCraftTags::Panel_Render))));
	MenuBuilder.AddMenuEntry(LOCTEXT("OpenRendersFolder", "Open Renders Folder"), FText::FromString(Render->GetOutputDirectory()), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([Directory = Render->GetOutputDirectory()]() { OpenFolder(Directory); })));
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillEditMenu(FMenuBuilder& MenuBuilder)
{
	// Weak: the controller is replaced on level travel while this menu bar lives on.
	const TWeakObjectPtr<AModularPlayerController> WeakController = GetStageController();
	if (!WeakController.IsValid())
	{
		return;
	}

	// Labels are built when the menu opens, so they name the step that would be undone right now.
	MenuBuilder.BeginSection(TEXT("History"), LOCTEXT("HistorySection", "History"));
	{
		const UStageEditHistorySubsystem* History = WeakController->GetEditHistory() ? WeakController->GetEditHistory()->GetHistory() : nullptr;
		const FText UndoName = History ? History->GetUndoDescription() : FText::GetEmpty();
		const FText RedoName = History ? History->GetRedoDescription() : FText::GetEmpty();

		MenuBuilder.AddMenuEntry(
			UndoName.IsEmpty() ? LOCTEXT("UndoNothing", "Undo  (Ctrl+Z)") : FText::Format(LOCTEXT("UndoStep", "Undo {0}  (Ctrl+Z)"), UndoName),
			LOCTEXT("UndoTip", "Revert the last placement, deletion or transform edit."), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakController]() { if (WeakController.IsValid()) { WeakController->RequestUndo(); } }),
				FCanExecuteAction::CreateLambda([bCanUndo = !UndoName.IsEmpty()]() { return bCanUndo; })));
		MenuBuilder.AddMenuEntry(
			RedoName.IsEmpty() ? LOCTEXT("RedoNothing", "Redo  (Ctrl+Y)") : FText::Format(LOCTEXT("RedoStep", "Redo {0}  (Ctrl+Y)"), RedoName),
			LOCTEXT("RedoTip", "Re-apply the last undone step (also Ctrl+Shift+Z)."), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakController]() { if (WeakController.IsValid()) { WeakController->RequestRedo(); } }),
				FCanExecuteAction::CreateLambda([bCanRedo = !RedoName.IsEmpty()]() { return bCanRedo; })));
	}
	MenuBuilder.EndSection();

	FillSelectionSection(MenuBuilder, WeakController);

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

	MenuBuilder.BeginSection(TEXT("Snapping"), LOCTEXT("SnappingSection", "Snapping"));
	MenuBuilder.AddMenuEntry(LOCTEXT("SnapToItems", "Snap to Items"),
		LOCTEXT("SnapToItemsTip", "While placing or moving, lock onto nearby items: side by side, stacked, or with edges and centres aligned. Guides show which faces line up."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakController]()
			{
				if (UStageSnappingComponent* SnappingComponent = WeakController.IsValid() ? WeakController->GetSnapping() : nullptr)
				{
					SnappingComponent->SetSnapToItemsEnabled(!SnappingComponent->IsSnapToItemsEnabled());
				}
			}),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([WeakController]()
			{
				const UStageSnappingComponent* SnappingComponent = WeakController.IsValid() ? WeakController->GetSnapping() : nullptr;
				return SnappingComponent && SnappingComponent->IsSnapToItemsEnabled();
			})),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	MenuBuilder.EndSection();
}

void SStageWorkspaceMenuBar::FillSelectionSection(FMenuBuilder& MenuBuilder, const TWeakObjectPtr<AModularPlayerController>& WeakController)
{
	const USelectionComponent* SelectionComponent = WeakController->GetSelection();
	const UStageSessionSubsystem* Session = UWorld::GetSubsystem<UStageSessionSubsystem>(WeakController->GetWorld());
	const int32 SelectedCount = SelectionComponent ? SelectionComponent->GetSelectionCount() : 0;
	const int32 PlacedCount = Session ? Session->GetStats().PlacedItems : 0;

	MenuBuilder.BeginSection(TEXT("Selection"), LOCTEXT("SelectionSection", "Selection"));
	MenuBuilder.AddMenuEntry(LOCTEXT("SelectAll", "Select All"),
		LOCTEXT("SelectAllTip", "Select every placed item. Ctrl+Click adds or removes single items."), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakController]() { if (WeakController.IsValid()) { WeakController->RequestSelectAll(); } }),
			FCanExecuteAction::CreateLambda([PlacedCount]() { return PlacedCount > 0; })));
	MenuBuilder.AddMenuEntry(LOCTEXT("DeselectAll", "Deselect All  (Esc)"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakController]()
			{
				if (USelectionComponent* Target = WeakController.IsValid() ? WeakController->GetSelection() : nullptr)
				{
					Target->ClearSelection();
				}
			}),
			FCanExecuteAction::CreateLambda([SelectedCount]() { return SelectedCount > 0; })));
	MenuBuilder.AddMenuEntry(
		SelectedCount > 1 ? FText::Format(LOCTEXT("DeleteSelectedN", "Delete {0} Selected Items  (Delete)"), FText::AsNumber(SelectedCount))
			: LOCTEXT("DeleteSelected", "Delete Selected  (Delete)"),
		LOCTEXT("DeleteSelectedTip", "Remove the selected items as one step; Ctrl+Z brings them back."), FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakController]() { if (WeakController.IsValid()) { WeakController->RequestDeleteSelection(); } }),
			FCanExecuteAction::CreateLambda([SelectedCount]() { return SelectedCount > 0; })));

	// Clearing asks first: the confirmation is a submenu, so one stray click can never wipe the stage.
	MenuBuilder.AddSubMenu(LOCTEXT("ClearStage", "Clear Stage..."),
		LOCTEXT("ClearStageTip", "Remove every placed item. You are asked to confirm, and Ctrl+Z restores the stage."),
		FNewMenuDelegate::CreateLambda([WeakController, PlacedCount](FMenuBuilder& ConfirmMenu)
		{
			ConfirmMenu.AddWidget(SNew(SBox).MaxDesiredWidth(280.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(FText::Format(LOCTEXT("ClearStageWarning", "This removes all {0} placed items from the stage. You can undo it with Ctrl+Z."), FText::AsNumber(PlacedCount)))
				], FText::GetEmpty(), /*bNoIndent*/ true);
			ConfirmMenu.AddMenuEntry(FText::Format(LOCTEXT("ClearStageConfirm", "Clear {0} Items"), FText::AsNumber(PlacedCount)), FText::GetEmpty(), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WeakController]() { if (WeakController.IsValid()) { WeakController->RequestClearStage(); } })));
		}),
		FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([PlacedCount]() { return PlacedCount > 0; })),
		NAME_None, EUserInterfaceActionType::Button);
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

void SStageWorkspaceMenuBar::ShowPanel(FGameplayTag PanelTag)
{
	// Opens the panel, or brings it to the front if it is already open somewhere.
	if (UStageWorkspaceSubsystem* WorkspacePtr = Workspace.Get())
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
