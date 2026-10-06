// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "History/StageCommandHistory.h"
#include "History/StageEditCommand.h"
#include "StageEditHistorySubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnStageEditHistoryChanged, EStageHistoryChange, Change, const FText&, Description);

/**
 * The undo/redo history of the stage being built in this world (Docs/ADR/0003-undo-redo-and-object-snapping.md).
 *
 * A world subsystem because the history describes one stage session: it must not survive level
 * travel, where the items it names no longer exist. Game and PIE worlds only, like UStageSessionSubsystem.
 *
 * Owns the stacks only. It never touches the world itself: commands run against the IStageItemEditor
 * passed in by the caller, which is the local player's UStageEditHistoryComponent (reached through the
 * controller's RequestUndo / RequestRedo bridge). Views read the descriptions and bind OnHistoryChanged.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageEditHistorySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	/** Adds an action that has already been applied. Clears the redo stack. */
	void Record(TSharedRef<IStageEditCommand> Command);

	EStageCommandResult Undo(IStageItemEditor& Editor);
	EStageCommandResult Redo(IStageItemEditor& Editor);

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	bool CanUndo() const { return History.CanUndo(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	bool CanRedo() const { return History.CanRedo(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	int32 GetUndoCount() const { return History.GetUndoCount(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	int32 GetRedoCount() const { return History.GetRedoCount(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	FText GetUndoDescription() const { return History.GetUndoDescription(); }

	UFUNCTION(BlueprintPure, Category = "StageCraft|History")
	FText GetRedoDescription() const { return History.GetRedoDescription(); }

	/** Description is the step concerned (empty for Cleared). */
	UPROPERTY(BlueprintAssignable, Category = "StageCraft|History")
	FOnStageEditHistoryChanged OnHistoryChanged;

protected:
	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

private:
	EStageCommandResult ApplyStep(bool bUndo, IStageItemEditor& Editor);

	FStageCommandHistory History;
};
