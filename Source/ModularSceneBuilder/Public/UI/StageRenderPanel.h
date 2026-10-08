// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Render/StageRenderTypes.h"
#include "StageRenderPanel.generated.h"

/**
 * The Render panel (StageCraft.Panel.Render): choose a resolution preset, anti-aliasing, post-processing and the
 * watermark, then render a still of the current view to Saved/Renders (Docs/ADR/0004-selection-scenes-and-rendering.md §5.4).
 *
 * A view in the project's binding pattern:
 *  - Settings are read from UStageRenderSubsystem and written back through SetSettings, its single write path; the panel
 *    shows them again from OnSettingsChanged (read-back), never from its own copy.
 *  - Rendering goes through the controller's RequestRender, like every other request; the panel follows the job through
 *    OnRenderStateChanged / OnRenderFinished (push, no polling) and stays responsive while the job runs.
 *  - Everything is bound in NativeConstruct and unbound in NativeDestruct. No tick.
 *
 * The widget tree is built in code from the theme, like the Library; there is no Widget Blueprint.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageRenderPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Render")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildChoiceRow(const FText& Title, TConstArrayView<FText> Labels, TConstArrayView<FText> Tips,
		TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons, void (UStageRenderPanel::*Handler)(int32));
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;

	void RefreshSettings();
	void RefreshJob();
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;

	void HandleResolutionPicked(int32 Index);
	void HandleAntiAliasingPicked(int32 Index);
	void HandlePostProcessPicked(int32 Index);
	void HandleWatermarkPicked(int32 Index);
	void ApplySettings(const FStageRenderSettings& Settings);

	UFUNCTION()
	void HandleRenderClicked();

	UFUNCTION()
	void HandleOpenFolderClicked();

	UFUNCTION()
	void HandleSettingsChanged(const FStageRenderSettings& Settings);

	UFUNCTION()
	void HandleRenderStateChanged(EStageRenderState NewState);

	UFUNCTION()
	void HandleRenderFinished(const FStageRenderResult& Result);

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ResolutionButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> AntiAliasingButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> PostProcessButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> WatermarkButtons;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> RenderButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> RenderButtonText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> OpenFolderButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> SummaryText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> FolderText = nullptr;

	TWeakObjectPtr<class UStageRenderSubsystem> RenderSubsystem;
};
