// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameplayTagContainer.h"
#include "Placement/StagePlacementTypes.h"
#include "StageItemLibraryPanel.generated.h"

DECLARE_DELEGATE_OneParam(FOnStageLibraryEntryClicked, class UBaseItemData* /*Item*/);
DECLARE_DELEGATE_TwoParams(FOnStageLibraryCategoryToggled, FGameplayTag /*Category*/, bool /*bExpanded*/);

namespace StageItemLibrary
{
	/**
	 * Search rule of the Library: every whitespace-separated word of Query must appear (case-insensitive) in the item
	 * name or its category label. An empty or blank query matches everything.
	 */
	MODULARSCENEBUILDER_API bool MatchesSearch(const FString& ItemName, const FString& CategoryLabel, const FString& Query);

	/** "StageCraft.Category.Lighting.MovingHead" reads "Lighting / MovingHead"; no tag reads "Other". */
	MODULARSCENEBUILDER_API FText MakeCategoryLabel(const FGameplayTag& Category);
}

/**
 * One catalog item in the Library: thumbnail tile, name and type, highlighted while it is the armed item.
 *
 * Without a Widget Blueprint the default tree is built in code and styled from the panel's theme. A WBP subclass
 * can provide widgets with the same names (BindWidgetOptional) and keeps its own designer styling.
 */
UCLASS(Blueprintable)
class MODULARSCENEBUILDER_API UStageItemLibraryEntry : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Binds the entry to its catalog item and the theme it is drawn with. Called once by the panel right after creation. */
	void SetItem(class UBaseItemData* InItem, const class UStageCraftUITheme& InTheme);

	/** Display state only; the panel sets it from the placement tool's armed item. */
	void SetArmed(bool bInArmed);

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

	/** Item type line under the name ("Moving Head", "Prop"...). */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> DetailText = nullptr;

private:
	void BuildDefaultTree();
	void ApplyCodeStyle();

	UFUNCTION()
	void HandleButtonClicked();

	UPROPERTY(Transient)
	TObjectPtr<class UBaseItemData> Item = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<const class UStageCraftUITheme> Theme = nullptr;

	/** Shown in the thumbnail tile when the item has no icon. Built with the default tree only. */
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> InitialText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UBorder> TileBorder = nullptr;

	bool bArmed = false;

	/** True when the tree came from BuildDefaultTree; only then does code apply styles (a WBP keeps its own). */
	bool bCodeStyled = false;
};

/**
 * A collapsible category group in the Library: a header row (chevron, name, visible count) above its entries.
 * Purely a view: the panel decides which entries it holds and which are visible.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageItemLibraryCategory : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Called once by the panel right after creation. */
	void InitializeCategory(const FGameplayTag& InCategory, const class UStageCraftUITheme& InTheme, bool bInExpanded);

	void AddEntry(class UStageItemLibraryEntry& Entry);

	/** While a search is active every matching group shows its entries, whatever its collapsed state. */
	void SetSearchActive(bool bInSearchActive);

	/** Recounts visible entries for the header and hides the whole group when none are visible. */
	void RefreshVisibleCount();

	const FGameplayTag& GetCategory() const { return Category; }

	/** The user expanded or collapsed the group (not raised by SetSearchActive). */
	FOnStageLibraryCategoryToggled OnToggled;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	//~ End UUserWidget Interface

private:
	void BuildTree();
	void RefreshBodyVisibility();

	UFUNCTION()
	void HandleHeaderClicked();

	UPROPERTY(Transient)
	TObjectPtr<class UButton> HeaderButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UImage> ChevronImage = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> LabelText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> CountText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> Body = nullptr;

	FGameplayTag Category;
	bool bExpanded = true;
	bool bSearchActive = false;
};

/**
 * Item Library dock panel (the asset browser of the stage): a toolbar with the Place-mode toggle, a search field,
 * and every catalog item in collapsible category groups.
 *
 * It changes nothing itself:
 * - Clicking an item asks the owning controller to arm it and enter Place mode (AModularPlayerController::RequestPlaceItem),
 *   so the rules decide and refusals are reported by the controller.
 * - The Place toggle switches the local player's placement tool, the same operation as P and the Edit menu.
 *
 * Push only, no tick: it reads the catalog once and rebuilds on UStageItemSubsystem::OnCatalogLoaded, and follows the
 * placement tool's OnArmedItemChanged / OnEditModeChanged. Searching only changes visibility, never rebuilds.
 *
 * Usable without a Widget Blueprint (the panel definition points straight at this class). Colors come from Theme, or
 * the palette defaults of UStageCraftUITheme when none is assigned.
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

	/** Receives the category groups. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UPanelWidget> ItemList = nullptr;

	/** "Loading…" until the catalog is ready, and "No items match" while a search hides everything. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> EmptyStateText = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UEditableTextBox> SearchBox = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UButton> PlaceModeButton = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> PlaceModeText = nullptr;

	/** Item count next to the title. */
	UPROPERTY(BlueprintReadOnly, Category = "StageCraft|Library", meta = (BindWidgetOptional))
	TObjectPtr<class UTextBlock> CountText = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Library")
	TSubclassOf<class UStageItemLibraryEntry> EntryClass;

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Library")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildDefaultTree();
	class UWidget* BuildToolbar();
	class UWidget* BuildSearchRow();
	class UWidget* BuildListArea();

	void RebuildEntries();
	void ClearEntries();
	void ApplySearch();
	void RefreshArmedHighlight();
	void RefreshPlaceModeButton();
	void SetEmptyState(const FText& Message);

	void HandleEntryClicked(class UBaseItemData* Item);
	void HandleCategoryToggled(FGameplayTag Category, bool bExpanded);

	UFUNCTION()
	void HandleCatalogLoaded();

	UFUNCTION()
	void HandleArmedItemChanged(class UBaseItemData* ArmedItem);

	UFUNCTION()
	void HandleEditModeChanged(EStageEditMode NewMode, EStageEditMode PreviousMode);

	UFUNCTION()
	void HandleSearchChanged(const FText& Text);

	UFUNCTION()
	void HandlePlaceModeClicked();

	class AModularPlayerController* GetStageController() const;
	const class UStageCraftUITheme& GetTheme() const;

	TWeakObjectPtr<class UStageItemSubsystem> ItemSubsystem;
	TWeakObjectPtr<class UStagePlacementToolComponent> PlacementTool;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageItemLibraryEntry>> Entries;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageItemLibraryCategory>> Categories;

	/** Groups the user collapsed. Kept across catalog rebuilds for the life of the panel. */
	TSet<FGameplayTag> CollapsedCategories;

	FString SearchQuery;

	/** True when the tree came from BuildDefaultTree; only then does code apply styles. */
	bool bCodeStyled = false;
};
