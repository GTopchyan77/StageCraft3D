// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameplayTagContainer.h"
#include "Outliner/StageOutlinerModel.h"
#include "StageOutlinerPanel.generated.h"

/** A row's button was clicked. The panel decides what it means; rows never touch the game. */
DECLARE_DELEGATE_OneParam(FOnStageOutlinerRowAction, class UStageOutlinerRow* /*Row*/);

/**
 * One line of the outliner: either a discipline header (chevron, eye and lock for the whole group, name, item count) or a placed
 * item (eye, lock, label, model). A view only, built in code from the theme; it raises its buttons' clicks and shows the state it is given.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageOutlinerRow : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Builds the row as a discipline header. Called once, right after creation and before the row is added to a panel. */
	void InitializeGroup(const FStageOutlinerGroup& Group, bool bExpanded, const class UStageCraftUITheme& InTheme);

	/** Builds the row as a placed item. Called once, right after creation. */
	void InitializeItem(const FStageOutlinerItem& Item, const class UStageCraftUITheme& InTheme);

	/** Selection highlight (display only). Group rows show it when every item of the group is selected. */
	void SetSelected(bool bInSelected);

	bool IsGroupRow() const { return bGroupRow; }
	const FGuid& GetInstanceId() const { return InstanceId; }
	const FGameplayTag& GetGroupCategory() const { return GroupCategory; }

	FOnStageOutlinerRowAction OnClicked;
	FOnStageOutlinerRowAction OnVisibilityClicked;
	FOnStageOutlinerRowAction OnLockClicked;
	FOnStageOutlinerRowAction OnExpandClicked;

private:
	/** The shared layout: [indent][chevron][eye][lock][name + detail button]. */
	void BuildTree(bool bWithChevron, float Indent);
	class UButton* MakeIconButton(FName BrushName, const FText& ToolTip, float Opacity, TObjectPtr<class UImage>& OutImage);
	void SetIcon(class UImage* Image, FName BrushName, float Opacity) const;
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
	TObjectPtr<const class UStageCraftUITheme> Theme = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UBorder> RowBorder = nullptr;

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

	FGuid InstanceId;
	FGameplayTag GroupCategory;
	bool bGroupRow = false;
	bool bSelected = false;
};

/**
 * The Outliner dock panel (StageCraft.Panel.Outliner, Docs/ADR/0005-spans-outliner-and-bill-of-materials.md §4): every placed item,
 * grouped by discipline (Stage, Truss, Rigging, Lighting, Audio...), with per-item and per-group visibility and lock toggles,
 * a filter, and group selection.
 *
 * A view in the project's binding pattern:
 *  - It changes nothing itself. Selecting goes through AModularPlayerController::RequestSelectItems; eye and lock go through
 *    RequestParameterChange (Visible / Locked parameters), so the GameMode validates and the session marks the scene dirty.
 *  - Push, never poll: the tree is rebuilt on the next tick after UStageSessionSubsystem::OnItemListChanged (coalescing a burst,
 *    e.g. hiding a whole group, into one rebuild); selection changes only restyle rows (USelectionComponent::OnSelectionSetChanged).
 *  - Everything is bound in NativeConstruct and unbound in NativeDestruct. No tick.
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
	void ClearRows();
	/** Creates a row, binds it, lets Initialize build it as a group or item row, then adds it to the list. */
	void AddRow(TFunctionRef<void(class UStageOutlinerRow&)> Initialize);

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
	TObjectPtr<class UVerticalBox> RowList = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> CountText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> EmptyText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> FooterText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> FilterBox = nullptr;

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageOutlinerRow>> Rows;

	/** The model the rows were built from (values copied at the last rebuild). */
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
