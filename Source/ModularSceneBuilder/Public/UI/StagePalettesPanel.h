// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Show/ShowControlTypes.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "StagePalettesPanel.generated.h"

/**
 * The palette pools (StageCraft.Panel.Palettes, Docs/ADR/0028-palettes-and-references.md §7), laid out like a console's preset pools:
 *  - header: the kind tabs (ALL, INTENSITY, POSITION, COLOR, BEAM, FOCUS) and RECORD / RECORD PER FIXTURE for the selected kind;
 *  - the pool: one tile per palette of that kind (a colour swatch on colour palettes); a click applies it to the selection;
 *  - the current palette (the last one clicked or recorded): rename, UPDATE (re-record from the selection), DELETE, and the cues
 *    that refer to it.
 *
 * A view in the project's binding pattern: reads UStageShowLibrarySubsystem and UShowControlSubsystem, owns no state but the tab and
 * which palette is current; rebuilds on the next tick after OnPalettesChanged or OnCueListChanged; every write goes through
 * AModularPlayerController (RequestApplyPalette, RequestRecordPalette, ...). Bindings are made in NativeConstruct and removed in
 * NativeDestruct. No tick.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStagePalettesPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Palettes")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildCurrentPaletteRow();
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit);
	FButtonStyle MakeButtonStyle(bool bLit) const;
	void SetButtonLit(class UStageChoiceButton& Button, bool bLit);
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UWidget* MakeTile(const FStagePalette& Palette, bool bCurrent);
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void RequestRebuild();
	void Rebuild();

	UFUNCTION()
	void HandleShowDataChanged();

	UFUNCTION()
	void HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod);

	void HandleHeaderPicked(int32 ChoiceIndex);
	void HandlePalettePicked(int32 TileIndex);
	void HandleCurrentPalettePicked(int32 ChoiceIndex);

	UPROPERTY(Transient)
	TObjectPtr<class UWrapBox> Pool = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	/** One button per EStagePaletteKind, in enum order. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> KindTabs;

	UPROPERTY(Transient)
	TObjectPtr<class UWidget> CurrentPaletteRow = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> NameField = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> UsageText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> DeleteButton = nullptr;

	TWeakObjectPtr<class UStageShowLibrarySubsystem> BoundLibrary;
	TWeakObjectPtr<class UShowControlSubsystem> BoundShow;

	/** The pool shown and the kind RECORD records. */
	EStagePaletteKind ShownKind = EStagePaletteKind::Color;

	/** The palette the current-palette row edits; invalid when none (or it was deleted). */
	FGuid CurrentPaletteId;

	/** Palette ids in tile order, so a tile's index finds its palette. */
	TArray<FGuid> TilePaletteIds;

	FTimerHandle RebuildTimer;
};
