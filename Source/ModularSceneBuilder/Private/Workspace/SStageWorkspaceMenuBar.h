// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Workspace/StageWorkspaceTypes.h"

/**
 * The workspace's File, Edit, Render, Window, Layout and Audio menus, shown above the panels in the main window.
 *
 * A view in the project's binding pattern:
 * - It holds no state of its own. Menus are built from the subsystem's getters each time they open, so there is nothing to keep in sync and no polling.
 * - Every action commits through the owning system's validated operations (UStageWorkspaceSubsystem, the local player's
 *   placement tool and gizmo, UStageAudioSubsystem). The results are shown to the user (Save As errors inline).
 *
 * Holds the subsystem weakly: menus that outlive it do nothing. Game thread only.
 */
class SStageWorkspaceMenuBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStageWorkspaceMenuBar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, class UStageWorkspaceSubsystem* InWorkspace);

private:
	void FillFileMenu(class FMenuBuilder& MenuBuilder);
	void FillSaveSceneAsMenu(class FMenuBuilder& MenuBuilder);
	void FillOpenSceneMenu(class FMenuBuilder& MenuBuilder);
	void FillDeleteSceneMenu(class FMenuBuilder& MenuBuilder);
	void FillRenderMenu(class FMenuBuilder& MenuBuilder);
	void FillEditMenu(class FMenuBuilder& MenuBuilder);
	void FillSelectionSection(class FMenuBuilder& MenuBuilder, const TWeakObjectPtr<class AModularPlayerController>& WeakController);
	void FillWindowMenu(class FMenuBuilder& MenuBuilder);
	void FillLayoutMenu(class FMenuBuilder& MenuBuilder);
	void FillSaveAsMenu(class FMenuBuilder& MenuBuilder);
	void FillDeleteMenu(class FMenuBuilder& MenuBuilder);
	void FillAudioMenu(class FMenuBuilder& MenuBuilder);

	/** The local player's editing controller, or null (no world yet, or a different controller class). */
	class AModularPlayerController* GetStageController() const;
	class UStageAudioSubsystem* GetAudio() const;

	void TogglePanel(FGameplayTag PanelTag);
	void ShowPanel(FGameplayTag PanelTag);
	void ApplyLayout(FString LayoutName);
	void DeleteLayout(FString LayoutName);
	void HandleSaveAsCommitted(const FText& Text, ETextCommit::Type CommitType);
	void HandleSaveSceneAsCommitted(const FText& Text, ETextCommit::Type CommitType);
	static void OpenFolder(const FString& Directory);

	static FText DescribeResult(EStageLayoutResult Result);

	TWeakObjectPtr<class UStageWorkspaceSubsystem> Workspace;

	/** The Save As name box of the open menu. Only valid while that submenu is open. */
	TWeakPtr<class SEditableTextBox> SaveAsTextBox;

	/** The scene Save As name box of the open menu. Only valid while that submenu is open. */
	TWeakPtr<class SEditableTextBox> SaveSceneAsTextBox;

	/** A scene name the user was warned exists; a second Enter on the same name replaces it. */
	FString PendingSceneOverwrite;
};
