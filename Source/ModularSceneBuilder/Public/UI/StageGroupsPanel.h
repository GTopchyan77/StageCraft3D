// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "StageGroupsPanel.generated.h"

/**
 * The groups and programmer panel (StageCraft.Panel.Groups, Docs/ADR/0025-programmer-and-output-mix.md §8), laid out like a
 * console's group pool with its programmer keys:
 *  - programmer bar: how many fixtures the programmer holds, CLEAR, HIGHLIGHT and the FAN mode;
 *  - the group pool: one tile per group (name and fixture count); a click selects its fixtures, RECORD adds the selection as a group;
 *  - the current group (the last one clicked or recorded): rename, UPDATE (re-record from the selection) and DELETE.
 *
 * A view in the project's binding pattern: reads UStageShowLibrarySubsystem and UStageProgrammerSubsystem, owns no state but which
 * group is current; rebuilds on the next tick after OnGroupsChanged or OnValuesChanged; every write goes through
 * AModularPlayerController (RequestRecordGroup, RequestClearProgrammer, ...). Bindings are made in NativeConstruct and removed in
 * NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageGroupsPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Groups")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildProgrammerBar();
	class UWidget* BuildCurrentGroupRow();
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit);
	FButtonStyle MakeButtonStyle(bool bLit) const;
	void SetButtonLit(class UStageChoiceButton& Button, bool bLit);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void RequestRebuild();
	void Rebuild();

	UFUNCTION()
	void HandleShowDataChanged();

	UFUNCTION()
	void HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	void HandleProgrammerPicked(int32 ChoiceIndex);
	void HandleGroupPicked(int32 TileIndex);
	void HandleCurrentGroupPicked(int32 ChoiceIndex);

	UPROPERTY(Transient)
	TObjectPtr<class UWrapBox> Pool = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ProgrammerText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> ClearButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> HighlightButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> FanButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UWidget> CurrentGroupRow = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> NameField = nullptr;

	TWeakObjectPtr<class UStageShowLibrarySubsystem> BoundLibrary;
	TWeakObjectPtr<class UStageProgrammerSubsystem> BoundProgrammer;

	/** The group the current-group row edits; invalid when none (or it was deleted). */
	FGuid CurrentGroupId;

	/** Group ids in tile order, so a tile's index finds its group. */
	TArray<FGuid> TileGroupIds;

	FTimerHandle RebuildTimer;
};
