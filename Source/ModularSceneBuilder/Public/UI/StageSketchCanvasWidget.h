// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Sketch/StageSketchTypes.h"
#include "UI/StageMenuModel.h"
#include "UI/StageSketchMenus.h"
#include "StageSketchCanvasWidget.generated.h"

/**
 * The Sketch to Stage window (StageCraft.Panel.Sketch, Docs/ADR/0015-sketch-to-stage.md §2.7): a 2D drawing canvas with tools
 * (freehand, stage shape, truss line, fixture, erase), the deck, truss and photo width sliders, a live summary of what the sketch
 * will build, and GENERATE 3D. The less frequent settings sit in View-menu style dropdowns next to the tools (STATE.md #100):
 * Edit (undo stroke, clear, frame), Snap (grid step, snap to grid, smoothing), Items (the catalog item per element) and Photo
 * (import a PNG or JPEG drawing, UStageSketchSubsystem::ImportImage, Docs/ADR/0017 §2.6, read on this computer or by Claude, and
 * show it under the canvas).
 * Opens from Edit › Sketch to Stage..., the Window menu and the viewport toolbar's Tools menu.
 *
 * A view in the project's binding pattern:
 *  - The sketch is read from UStageSketchSubsystem and edited through its validated setters (tool state, not stage state).
 *    The stage changes only through AModularPlayerController::RequestGenerateFromSketch: one request, all or nothing, one undo step.
 *  - After every write it reads back: OnSketchChanged, OnGenerationFinished and the catalog's load and change events refresh it.
 *  - Bound in NativeConstruct, unbound in NativeDestruct. No tick (DisableNativeTick).
 *
 * The widget tree is built in code from the theme; there is no Widget Blueprint.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageSketchCanvasWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Sketch")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** The panel's sliders, in order; each one's index in Sliders. */
	enum class ESlider : int32
	{
		DeckHeight,
		TrussHeight,
		PhotoWidth,
		Count
	};

	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildRow(TConstArrayView<class UWidget*> Children);
	class UWidget* BuildSegment(TConstArrayView<FText> Labels, TConstArrayView<FText> Tips, TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons,
		void (UStageSketchCanvasWidget::*Handler)(int32));
	class UWidget* BuildSlider(ESlider Slider, const FText& Label, const FText& Tip, float Min, float Max);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageSketchCanvasWidget::*Handler)(int32));
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	const class UStageCraftUITheme& GetTheme() const;
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected, bool bPrimary = false) const;

	void Refresh();

	/** A toolbar dropdown bound to Builder, themed and in a fixed-width box so the bar does not jump when a label changes. */
	class UWidget* MakeMenuButton(FName Name, const FText& Tip, float Width, FStageMenu (UStageSketchCanvasWidget::*Builder)(), TObjectPtr<class UStageMenuButton>& OutButton);

	/** The toolbar dropdowns (StageSketchMenus), described from the sketch's state when they open. */
	FStageMenu BuildEditMenu();
	FStageMenu BuildSnapMenu();
	FStageMenu BuildItemsMenu();
	FStageMenu BuildPhotoMenu();
	StageSketchMenus::FPhotoState GetPhotoState() const;

	void HandleToolPicked(int32 Index);
	void HandleEditCommand(StageSketchMenus::EEditCommand Command);
	void HandleSettingsPicked(const StageSketchMenus::FSettingsEdit& Edit);
	void HandleItemPicked(EStageSketchElement Element, class UBaseItemData* Item);
	void HandleGeneratePicked(int32 Unused);
	void HandleSliderMoved(int32 SliderIndex, float Value);

	/** Import opens the file dialog and starts the import; the others change the reference photo or who reads the next one. */
	void HandlePhotoCommand(StageSketchMenus::EPhotoCommand Command);
	/** TRACE ANYWAY (index 0) or USE COLOURS (index 1) on the image that was refused as a photograph. */
	void HandleRefusedPicked(int32 Index);

	UFUNCTION()
	void HandleImportChanged();

	UFUNCTION()
	void HandleSketchChanged();

	UFUNCTION()
	void HandleGenerationFinished(const FStageSketchGenerateResult& Result);

	UFUNCTION()
	void HandleCatalogChanged();

	class UStageSketchSubsystem* GetSketch() const;
	class AModularPlayerController* GetStageController() const;

	UPROPERTY(Transient)
	TObjectPtr<class UStageSketchSurfaceWidget> Surface = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ToolButtons;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> EditMenuButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> SnapMenuButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> ItemsMenuButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageMenuButton> PhotoMenuButton = nullptr;

	/** TRACE ANYWAY, USE COLOURS: shown next to the result only while an image is refused as a photograph (its message names them). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> RefusedButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageValueSlider>> Sliders;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTextBlock>> SliderValues;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> GenerateButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> HintText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> PlanText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> WarningText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ResultText = nullptr;

	TWeakObjectPtr<class UStageSketchSubsystem> BoundSketch;
	TWeakObjectPtr<class UStageItemSubsystem> BoundCatalog;

	/** The last generation's message; cleared by the next sketch edit, so it never describes a different drawing. */
	FText ResultMessage;
	bool bResultSucceeded = false;

	/** Who reads the next imported photo: a choice of this window, kept while it is open. */
	EStageSketchReader PhotoReader = EStageSketchReader::Local;

	/** The folder of the last photo opened, for the next file dialog. */
	FString LastPhotoDirectory;

	/** The sketch's import count when its message was last shown (UStageSketchSubsystem::GetImportsFinished). */
	uint32 ShownImport = 0;
};
