// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Bridge/StageBridgeTypes.h"
#include "Scene/StageSceneTypes.h"
#include "StageSendToWidget.generated.h"

/**
 * The Send to 3D App window (StageCraft.Panel.SendTo, Docs/ADR/0022-dcc-bridge.md §7): switches the DCC bridge on or off, picks
 * a saved scene and sends it to a connected Blender, 3ds Max or Cinema 4D, or saves it as a .glb file. Opens from
 * File › Send to 3D App... as a floating window.
 *
 * A view in the project's binding pattern:
 *  - Reads come from UStageBridgeSubsystem (state, connected apps) and the scene component (saved scenes, the open one).
 *  - Writes go through the controller: RequestSetBridgeEnabled, RequestSendScene, RequestExportScene. Refusals reach the status bar
 *    through OnRequestRejected and are also shown here.
 *  - Push, never poll: bound in NativeConstruct, unbound in NativeDestruct; no tick. The target list is rebuilt when the bridge
 *    reports a change (a socket event drained on a later frame), never inside a target button's own click.
 *
 * The widget tree is built in code from the theme; there is no Widget Blueprint.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageSendToWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Bridge")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageSendToWidget::*Handler)(int32));
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;
	class UStageBridgeSubsystem* GetBridge() const;

	void RefreshScenes();
	void RebuildTargets();
	void RefreshState();
	void SetStatus(const FText& Text, bool bWarning);
	FString GetSelectedScene() const;

	void HandleTogglePicked(int32 Index);
	void HandleTargetPicked(int32 Index);

	UFUNCTION()
	void HandleBridgeStateChanged(bool bRunning);

	UFUNCTION()
	void HandleClientsChanged();

	UFUNCTION()
	void HandleSendCompleted(const FStageBridgeResultInfo& Result);

	/** An export this panel asked for (through the controller) has ended. */
	UFUNCTION()
	void HandleExportCompleted(const FStageBridgeResultInfo& Result);

	UFUNCTION()
	void HandleSceneSelected(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleSceneOperationFinished(const FStageSceneOperationResult& Result);

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> ToggleButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StateText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> SceneCombo = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SceneNote = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> TargetList = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	/** Client ids of the app buttons, by button index; the last button (index == Num) is "Save as .glb file". */
	TArray<int32> TargetClients;

	TWeakObjectPtr<class UStageBridgeSubsystem> BoundBridge;
	TWeakObjectPtr<class AModularPlayerController> BoundController;
	TWeakObjectPtr<class UStageSceneComponent> BoundScenes;
};
