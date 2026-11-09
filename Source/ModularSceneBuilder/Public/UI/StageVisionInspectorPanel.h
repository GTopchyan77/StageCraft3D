// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "StageVisionInspectorPanel.generated.h"

/**
 * The Vision Inspector panel (StageCraft.Panel.VisionInspector, Docs/ADR/0032 §8): pick a photo of a real stage, read it with the online
 * model, check the bill of materials (kind, count, position, real-world spec, catalog match, confidence), correct counts, save it as CSV
 * and build it in the stage.
 *
 * A view in the project's binding pattern: owns no state (the bill is UStageVisionBomSubsystem's); rebuilds on the next tick after
 * OnChanged; quantity edits and removals go through the subsystem's validated setters; building goes through
 * AModularPlayerController::RequestBuildFromVisionBom (one undo step). Bound in NativeConstruct, unbound in NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageVisionInspectorPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Vision")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	enum class EAction : int32 { OpenPhoto, Cancel, Clear, CopyCsv, SaveCsv, Build, Count };

	void BuildTree();
	const class UStageCraftUITheme& GetTheme() const;
	class UStageVisionBomSubsystem* GetVision() const;
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageVisionInspectorPanel::*Handler)(int32), bool bAccent = false);

	void RequestRebuild();
	void Rebuild();
	void HandleChanged();
	void HandleAction(int32 Index);
	/** Row buttons: Index = line * 3 + (0 minus, 1 plus, 2 remove). */
	void HandleRowAction(int32 Index);

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> TotalsText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> List = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ActionButtons;

	TWeakObjectPtr<class UStageVisionBomSubsystem> BoundVision;
	FDelegateHandle ChangedHandle;
	FTimerHandle RebuildTimer;
	FString LastPhotoDirectory;
};
