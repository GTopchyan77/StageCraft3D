// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/IUserListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/UserWidgetPool.h"
#include "Components/Widget.h"
#include "GameplayTagContainer.h"
#include "Outliner/StageOutlinerModel.h"
#include "StageOutlinerPanel.generated.h"

/** A row's button was clicked. The panel decides what it means; rows never touch the game. */
DECLARE_DELEGATE_OneParam(FOnStageOutlinerRowAction, class UStageOutlinerRow* /*Row*/);

/**
 * One line of the outliner's list as data: a discipline header or a placed item, copied from the outliner model at a rebuild.
 * The list hands it to whichever row widget is on screen for it (rows are recycled while scrolling). Owned by the panel.
 */
UCLASS(Transient)
class MODULARSCENEBUILDER_API UStageOutlinerEntry : public UObject
{
	GENERATED_BODY()

public:
	bool bGroupRow = false;
	/** Header rows: the discipline. Item rows: the discipline of their group. */
	FGameplayTag GroupCategory;
	/** Header rows only. */
	FText GroupLabel;
	int32 GroupItemCount = 0;
	EStageOutlinerFlagState GroupHidden = EStageOutlinerFlagState::None;
	EStageOutlinerFlagState GroupLocked = EStageOutlinerFlagState::None;
	bool bExpanded = true;
	/** Item rows only. */
	FStageOutlinerItem Item;
	/** Selection highlight (display only). Header rows show it when every listed item of the group is selected. */
	bool bSelected = false;

	UPROPERTY(Transient)
	TObjectPtr<const class UStageCraftUITheme> Theme = nullptr;
};

/**
 * One line of the outliner: either a discipline header (chevron, eye and lock for the whole group, name, item count) or a placed
 * item (eye, lock, label, model). A list entry: built once, then shows whichever UStageOutlinerEntry the list gives it, so it can be
 * recycled for another line while scrolling. A view only; it raises its buttons' clicks and shows the state it is given.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageOutlinerRow : public UUserWidget, public IUserListEntry
{
	GENERATED_BODY()

public:
	/** The line this row shows; null until the list assigns one. */
	const UStageOutlinerEntry* GetEntry() const { return Entry; }
	bool IsGroupRow() const { return Entry && Entry->bGroupRow; }
	FGuid GetInstanceId() const { return Entry && !Entry->bGroupRow ? Entry->Item.InstanceId : FGuid(); }
	FGameplayTag GetGroupCategory() const { return Entry ? Entry->GroupCategory : FGameplayTag(); }

	/** Shows InEntry (the list calls it whenever the row is given a line, including a reused row). */
	void SetEntry(UStageOutlinerEntry* InEntry);

	/** Re-reads the entry's selection flag (a selection change restyles the rows on screen without rebuilding the list). */
	void RefreshSelected();

	FOnStageOutlinerRowAction OnClicked;
	FOnStageOutlinerRowAction OnVisibilityClicked;
	FOnStageOutlinerRowAction OnLockClicked;
	FOnStageOutlinerRowAction OnExpandClicked;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	//~ End UUserWidget Interface

private:
	/** The shared layout: [indent][chevron][eye][lock][name + detail button]. Built once, before the row's Slate widget exists. */
	void BuildTree();
	class UButton* MakeIconButton(TObjectPtr<class UImage>& OutImage);
	/** Button styles of InTheme; applied when an entry brings a theme the row is not styled with yet. */
	void ApplyTheme(const class UStageCraftUITheme& InTheme);
	void SetIcon(class UImage* Image, FName BrushName, float Opacity) const;
	void ShowGroup(const UStageOutlinerEntry& Group);
	void ShowItem(const UStageOutlinerEntry& ItemEntry);
	void ApplyRowStyle();

	UFUNCTION()
	void HandleRowClicked();

	UFUNCTION()
	void HandleVisibilityClicked();

	UFUNCTION()
	void HandleLockClicked();

	UFUNCTION()
	void HandleExpandClicked();

	UPROPERTY(Transient)
	TObjectPtr<UStageOutlinerEntry> Entry = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<const class UStageCraftUITheme> Theme = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UBorder> RowBorder = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class USpacer> IndentSpacer = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> RowButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> ExpandButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> VisibilityButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UButton> LockButton = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UImage> ExpandImage = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UImage> VisibilityImage = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UImage> LockImage = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> NameText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> DetailText = nullptr;
};

/** The list made a row or took one back from its pool; the panel binds the row's clicks. Runs again for every reuse. */
DECLARE_DELEGATE_OneParam(FOnStageOutlinerRowGenerated, class UStageOutlinerRow& /*Row*/);

/**
 * The outliner's virtualized list (an SListView): only the rows on screen exist as widgets, and rows scrolled away are pooled for
 * the next lines. A stage of hundreds of items then costs the UI no more per frame than a short one; a box with one widget per
 * item cost about 2 ms of Slate a frame at 500 items (STATE.md #105).
 *
 * Built like UListView (pooled UUserWidget rows whose Slate widget is the table row) but with a native row class: UListView refuses
 * an entry class that is not a Blueprint in editor builds (UListViewBase::RebuildWidget, final), and the rows are built in code.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageOutlinerList : public UWidget
{
	GENERATED_BODY()

public:
	UStageOutlinerList(const FObjectInitializer& ObjectInitializer);

	/** The scroll bar. Takes effect when the Slate widget is built; call it before. */
	void SetListStyle(const FScrollBarStyle& InScrollBarStyle);

	/** Lists InEntries, in order. Rows on screen are given their entry again, since the panel reuses entry objects across rebuilds. */
	void SetEntries(TConstArrayView<TObjectPtr<UStageOutlinerEntry>> InEntries);

	/** The rows on screen (the only ones that exist as widgets). */
	void ForEachDisplayedRow(TFunctionRef<void(class UStageOutlinerRow&)> Visit) const;

	void ScrollToStart();
	void ScrollToEnd();

	FOnStageOutlinerRowGenerated OnRowGenerated;

protected:
	//~ Begin UWidget Interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	//~ End UWidget Interface

private:
	TSharedRef<class ITableRow> GenerateRow(TObjectPtr<UStageOutlinerEntry> Entry, const TSharedRef<class STableViewBase>& OwnerTable);
	void HandleRowReleased(const TSharedRef<class ITableRow>& TableRow);

	/** What SListView lists (it reads this array; SetEntries refreshes it). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStageOutlinerEntry>> Items;

	/** Rows not on screen wait here for the next line; the rows on screen are its active widgets. */
	UPROPERTY(Transient)
	FUserWidgetPool RowPool;

	FScrollBarStyle ScrollBarStyle;
	TSharedPtr<class STableViewBase> ListWidget;
};

/**
 * The Outliner dock panel (StageCraft.Panel.Outliner, Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §4): every placed item,
 * grouped by discipline (Stage, Truss, Rigging, Lighting, Audio...), with per-item and per-group visibility and lock toggles,
 * a filter, and group selection.
 *
 * A view in the project's binding pattern:
 *  - It changes nothing itself. Selecting goes through AModularPlayerController::RequestSelectItems; eye and lock go through
 *    RequestParameterChange (Visible / Locked parameters), so the GameMode validates and the session marks the scene dirty.
 *  - Push, never poll: the list is rebuilt on the next tick after UStageSessionSubsystem::OnItemListChanged (coalescing a burst,
 *    e.g. hiding a whole group, into one rebuild); selection changes only restyle rows (USelectionComponent::OnSelectionSetChanged).
 *  - Everything is bound in NativeConstruct and unbound in NativeDestruct. No tick.
 *  - The lines are data (UStageOutlinerEntry) in a virtualized list (UStageOutlinerList); only the rows on screen are widgets.
 *
 * Clicking an item selects it (Ctrl: add / remove); clicking a discipline selects every listed item in it (Ctrl: add / remove the group).
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageOutlinerPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Outliner")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildFilterRow();
	const class UStageCraftUITheme& GetTheme() const;
	class AModularPlayerController* GetStageController() const;

	void RequestRebuild();
	void Rebuild();
	void RefreshSelection();
	/** Fills Entry with Row of the listing. Entry objects are reused across rebuilds; the list regenerates its rows after. */
	void FillEntry(UStageOutlinerEntry& Entry, const FStageOutlinerRowRef& Row) const;
	/** Binds a row the list made or took back from its pool. Rows are recycled, so this runs again for each reuse. */
	void HandleRowGenerated(class UStageOutlinerRow& Row);

	/** The live items behind a group row: its items that pass the filter. */
	TArray<class AModularBaseActor*> ResolveGroupItems(const FGameplayTag& GroupCategory) const;
	class AModularBaseActor* ResolveItem(const FGuid& InstanceId) const;
	const FStageOutlinerGroup* FindGroup(const FGameplayTag& GroupCategory) const;

	/** Writes a Visible / Locked flag to each item that does not have it yet, through the controller's request bridge. */
	void SetFlag(const TArray<class AModularBaseActor*>& Items, const FGameplayTag& ParameterId, bool bValue);

	void HandleRowClicked(class UStageOutlinerRow* Row);
	void HandleVisibilityClicked(class UStageOutlinerRow* Row);
	void HandleLockClicked(class UStageOutlinerRow* Row);
	void HandleExpandClicked(class UStageOutlinerRow* Row);

	UFUNCTION()
	void HandleItemListChanged();

	UFUNCTION()
	void HandleSelectionSetChanged(int32 Count);

	UFUNCTION()
	void HandleFilterChanged(const FText& Text);

	UPROPERTY(Transient)
	TObjectPtr<UStageOutlinerList> RowList = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> CountText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> FooterText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> FilterBox = nullptr;

	/** One per listed line, in list order. Reused across rebuilds (only grown or trimmed), so a rebuild makes no new objects. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStageOutlinerEntry>> Entries;

	/** The model the entries were built from (values copied at the last rebuild). */
	TArray<FStageOutlinerGroup> Groups;

	/** Instance id -> live item, rebuilt with the rows. Weak: an item can be destroyed before the next rebuild. */
	TMap<FGuid, TWeakObjectPtr<class AModularBaseActor>> ItemsById;

	/** Groups the user collapsed; kept across rebuilds for the life of the panel. */
	TSet<FGameplayTag> CollapsedGroups;

	FString FilterQuery;

	TWeakObjectPtr<class UStageSessionSubsystem> Session;
	TWeakObjectPtr<class USelectionComponent> Selection;
	FTimerHandle RebuildTimer;
};
