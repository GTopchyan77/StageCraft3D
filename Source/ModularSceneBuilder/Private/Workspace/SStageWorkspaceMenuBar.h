// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Workspace/StageWorkspaceTypes.h"

/**
 * The workspace's Window and Layout menus, shown above the panels in the main window.
 *
 * A view in the project's binding pattern:
 * - It holds no state of its own. Menus are built from the subsystem's getters each time they open, so there is nothing to keep in sync and no polling.
 * - Every action commits through UStageWorkspaceSubsystem's validated operations. The results are shown to the user (Save As errors inline).
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
	void FillWindowMenu(class FMenuBuilder& MenuBuilder);
	void FillLayoutMenu(class FMenuBuilder& MenuBuilder);
	void FillSaveAsMenu(class FMenuBuilder& MenuBuilder);
	void FillDeleteMenu(class FMenuBuilder& MenuBuilder);

	void TogglePanel(FGameplayTag PanelTag);
	void ApplyLayout(FString LayoutName);
	void DeleteLayout(FString LayoutName);
	void HandleSaveAsCommitted(const FText& Text, ETextCommit::Type CommitType);

	static FText DescribeResult(EStageLayoutResult Result);

	TWeakObjectPtr<class UStageWorkspaceSubsystem> Workspace;

	/** The Save As name box of the open menu. Only valid while that submenu is open. */
	TWeakPtr<class SEditableTextBox> SaveAsTextBox;
};
