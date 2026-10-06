// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "StageItemLibraryPanel.generated.h"

DECLARE_DELEGATE_OneParam(FOnStageLibraryEntryClicked, class UBaseItemData* /*Item*/);

/**
 * One catalog item in the Library panel: icon + name, highlighted while it is the armed item.
 * Works without a Widget Blueprint (a default tree is built in code); a WBP subclass can restyle it by
 * providing widgets with the same names (BindWidgetOptional).
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageItemLibraryEntry : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Binds the entry to its catalog item. Called once by the panel right after creation. */
	void SetItem(class UBaseItemData* InItem);

	/** Display state only; the panel sets it from the placement tool's armed item. */
	void SetArmed(bool bArmed);

	class UBaseItemData* GetItem() const { return Item; }

	FOnStageLibraryEntryClicked OnClicked;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	//~ End UUserWidget Interface

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UButton> EntryButton = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UImage> IconImage = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> NameText = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Library")
	FLinearColor ArmedTint = FLinearColor(0.2f, 0.55f, 1.f);

private:
	void BuildDefaultTree();

	UFUNCTION()
	void HandleButtonClicked();

	UPROPERTY(Transient)
	TObjectPtr<class UBaseItemData> Item = nullptr;
};

/**
 * Item Library dock panel: every catalog item grouped by category. Clicking an item asks the owning
 * player's controller to arm it and enter Place mode (AModularPlayerController::RequestPlaceItem), so
 * the rules decide; the panel itself changes nothing.
 *
 * Push only: it reads the catalog once and rebuilds on UStageItemSubsystem::OnCatalogLoaded, and follows
 * the armed item through the placement tool's OnArmedItemChanged. No tick.
 *
 * Usable without a Widget Blueprint (the panel definition can point straight at this class); a WBP
 * subclass can lay it out differently by providing ItemList / StatusText.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageItemLibraryPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	/** Receives category headers and entries. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UPanelWidget> ItemList = nullptr;

	/** Shows "Loading…" until the catalog is ready, then the hint line. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Library")
	TSubclassOf<class UStageItemLibraryEntry> EntryClass;

private:
	void BuildDefaultTree();
	void RebuildEntries();
	void RefreshArmedHighlight();
	void HandleEntryClicked(class UBaseItemData* Item);

	UFUNCTION()
	void HandleCatalogLoaded();

	UFUNCTION()
	void HandleArmedItemChanged(class UBaseItemData* ArmedItem);

	class AModularPlayerController* GetStageController() const;

	TWeakObjectPtr<class UStageItemSubsystem> ItemSubsystem;
	TWeakObjectPtr<class UStagePlacementToolComponent> PlacementTool;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageItemLibraryEntry>> Entries;
};
