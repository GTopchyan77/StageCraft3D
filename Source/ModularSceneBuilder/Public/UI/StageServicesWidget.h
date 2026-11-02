// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Services/StageAIToolTypes.h"
#include "StageServicesWidget.generated.h"

/**
 * The Services & Keys window (StageCraft.Panel.Services, Docs/ADR/0023-ai-tool-providers-and-credential-vault.md §5): one row per
 * configured AI tool with its key state and a password field (SAVE, CHECK, REMOVE), and the 3D apps connected over the bridge.
 *
 * Keys go in once, through AModularPlayerController::RequestStoreProviderKey, and are never shown again; the field is cleared as
 * soon as the request is made. StageCraft cannot check a 3D app's own licence: an app is listed when its add-on connects.
 * Push, never poll: bound in NativeConstruct, unbound in NativeDestruct; rows are rebuilt on the next tick after a change.
 * Built in code from the theme.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageServicesWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Services")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** Per-tool buttons; a button's index is ToolIndex * 3 + action (StageServicesPanel::ActionCount). */
	enum class EToolAction : int32 { Save, Check, Remove };

	void BuildTree();
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageServicesWidget::*Handler)(int32));
	const class UStageCraftUITheme& GetTheme() const;
	void StyleChoice(class UStageChoiceButton& Button) const;

	void RequestRebuild();
	/** Rebuilds the tool cards when the set of tools changed; otherwise updates their state labels and buttons in place, so a key being typed survives. */
	void RefreshTools();
	void RebuildToolCards(const TArray<FStageAIToolInfo>& Infos);
	void UpdateToolCard(int32 Row, const FStageAIToolInfo& Info);
	void RebuildApps();
	void SetStatus(const FText& Message, bool bWarning);
	FText DescribeKeyState(EStageAIToolKeyState State) const;

	void HandleToolAction(int32 Index);
	void HandleOpenSendTo(int32 Unused);

	UFUNCTION()
	void HandleToolsChanged();
	UFUNCTION()
	void HandleKeyVerified(const FStageAIToolResultInfo& Result);
	/** A hand-off ended (the dispatcher's): CHECK is usable again. */
	UFUNCTION()
	void HandleJobCompleted(const FStageAIToolResultInfo& Result);
	UFUNCTION()
	void HandleBridgeStateChanged(bool bRunning);

	class AModularPlayerController* GetStageController() const;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> ToolList = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> AppList = nullptr;
	/** Per tool row, in GetTools() order: the password field, the key state label and the SAVE / CHECK / REMOVE buttons. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UEditableTextBox>> KeyFields;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> StateLabels;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> SaveButtons;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> CheckButtons;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> RemoveButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	TWeakObjectPtr<class UStageAIToolSubsystem> BoundTools;
	TWeakObjectPtr<class UStageBridgeSubsystem> BoundBridge;

	/** The ids of the rows, in order, so a button index maps back to its tool. */
	TArray<FName> RowIds;
	FTimerHandle RebuildTimer;
};
