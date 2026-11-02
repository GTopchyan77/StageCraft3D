// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Economy/StageEconomyTypes.h"
#include "Marketplace/StageDispatchTypes.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "StageMarketplaceWidget.generated.h"

/**
 * The Community Marketplace window (StageCraft.Panel.Marketplace, Docs/ADR/0021-community-marketplace.md §12): browse, preview in
 * 3D, get or buy, open in the stage, and publish a saved scene. Opens from Edit › Community Marketplace... and the Window menu, as
 * a floating window.
 *
 * A view in the project's binding pattern:
 *  - Reads come from UStageMarketplaceSubsystem (queries, the read-only preview), the profile (credits) and the economy.
 *  - Writes go through the controller: RequestMarketplacePublish and RequestLoadScene (confirmed with a second click when the
 *    stage has unsaved changes). GET / BUY opens the acquire dialog (UStageAcquireDialogWidget, ADR 0022 §11.3) over the panel,
 *    which confirms the price and the target and starts an order with RequestAcquireAndDispatch; nothing is bought in one click. Refusals reach the status bar through OnRequestRejected; outcomes arrive
 *    on the subsystem's delegates and are also shown here.
 *  - Push, never poll: bound in NativeConstruct, unbound in NativeDestruct; no tick (DisableNativeTick). The card list is rebuilt
 *    when a query answers (always a later frame than the click that asked), never inside a card's own click.
 *  - The read-only preview (FStageMarketplacePreview) lives as long as this widget and is destroyed with it.
 *
 * The widget tree is built in code from the theme; there is no Widget Blueprint.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageMarketplaceWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows the acquire dialog for ListingId (a listing of the current results) over the panel. Nothing is bought until it is confirmed. */
	void OpenAcquireDialog(const FGuid& ListingId);

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Marketplace")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	/** Listing details actions, in button order. */
	enum class EAction : int32 { Preview, Acquire, Open, Frame };

	void BuildTree();
	class UWidget* BuildHeader();
	class UWidget* BuildBrowsePage();
	class UWidget* BuildFilterColumn();
	class UWidget* BuildDetailColumn();
	class UWidget* BuildPublishPage();
	class UWidget* BuildRow(TConstArrayView<class UWidget*> Children, float Gap = 6.f);
	class UWidget* BuildLabeled(const FText& Label, class UWidget* Field);
	class UWidget* BuildSegment(TConstArrayView<FText> Labels, TArray<TObjectPtr<class UStageChoiceButton>>& OutButtons, void (UStageMarketplaceWidget::*Handler)(int32));
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageMarketplaceWidget::*Handler)(int32));
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UEditableTextBox* MakeTextField(const FText& Hint);
	class UStageThemedComboBox* MakeCombo(TConstArrayView<FText> Options);
	const class UStageCraftUITheme& GetTheme() const;
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;
	void StyleCard(class UStageChoiceButton& Card, bool bSelected) const;

	// --- Browsing ---
	void RunQuery();
	/** Takes the marketplace's last answered query (whoever sent it) into the filters, without querying. */
	void AdoptAnsweredQuery();
	void RebuildCards();
	void RefreshCardStyles();
	void RefreshFilters();
	void RefreshDetails();
	void RefreshHeader();
	const FStageMarketplaceListing* GetSelectedListing() const;
	FText DescribePrice(const FStageMarketplaceListing& Listing, EStageOwnershipState Ownership) const;

	void HandlePagePicked(int32 Index);
	void HandleKindPicked(int32 Index);
	void HandlePricePicked(int32 Index);
	void HandleCardPicked(int32 Index);
	void HandleActionPicked(int32 Index);

	// --- Acquire dialog ---
	void CloseAcquireDialog();
	/** Opens the scene imported from ListingId on the stage; a dirty stage needs a second request (the same duty as File > Open). */
	void OpenImportedScene(const FGuid& ListingId);

	// --- Preview ---
	void StartPreview();
	void HandlePreviewReady(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FStageMarketplacePreviewScene& Scene);
	void HandlePreviewOrbit(const FVector2D& DragPixels);
	void HandlePreviewZoom(float WheelSteps);

	// --- Publishing ---
	void RefreshSceneChoices();
	void HandlePagesPicked(int32 Index);
	void HandlePublishKindPicked(int32 Index);
	void HandlePublishExportPicked(int32 Index);
	void HandlePublishPicked(int32 Index);

	void ShowStatus(const FText& Message, bool bWarning);

	UFUNCTION()
	void HandleSearchCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void HandleCategoryChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
	UFUNCTION()
	void HandleSortChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
	UFUNCTION()
	void HandleListingsUpdated(FStageMarketplaceResultInfo Result);
	UFUNCTION()
	void HandleAcquireCompleted(FGuid ListingId, FStageMarketplaceResultInfo Result, const FString& SceneName);
	UFUNCTION()
	void HandlePublishCompleted(FStageMarketplaceResultInfo Result, const FStageMarketplaceListing& Listing);
	UFUNCTION()
	void HandleBalanceChanged(FGameplayTag Currency, int64 NewBalance);
	UFUNCTION()
	void HandleOwnershipChanged();
	UFUNCTION()
	void HandleOrderCompleted(const FStageOrderResultInfo& Result);

	class UStageMarketplaceSubsystem* GetMarketplace() const { return BoundMarketplace.Get(); }
	class AModularPlayerController* GetStageController() const;

	// Header.
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> CreditsText = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> PageButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UWidgetSwitcher> Pages = nullptr;

	// Browse: filters and results.
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> SearchField = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> KindButtons;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> PriceButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> CategoryCombo = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> SortCombo = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> ResultsText = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> PagingButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> CardList = nullptr;
	/** One per listing of the current page, in order. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> Cards;

	// Browse: preview and details.
	UPROPERTY(Transient)
	TObjectPtr<class UStageMarketplacePreviewWidget> PreviewSurface = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> PreviewText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> DetailTitle = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> DetailByline = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> DetailStats = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> DetailDescription = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> DetailPrice = nullptr;
	/** Indexed by EAction. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> ActionButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;

	// Publish.
	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> SceneCombo = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> TitleField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UMultiLineEditableTextBox> DescriptionField = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UEditableTextBox> KeywordsField = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> PublishKindButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UStageThemedComboBox> PublishCategoryCombo = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class USpinBox> PriceField = nullptr;
	/** Keep in StageCraft / allow 3D apps and files (the manifest's allowExport). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> PublishExportButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UStageChoiceButton> PublishButton = nullptr;

	// Acquire dialog: a dimmed layer over the whole panel, holding the dialog while it is open.
	UPROPERTY(Transient)
	TObjectPtr<class UBorder> DialogLayer = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UStageAcquireDialogWidget> AcquireDialog = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> PublishStatusText = nullptr;

	TWeakObjectPtr<class UStageMarketplaceSubsystem> BoundMarketplace;
	TWeakObjectPtr<class UStageProfileSubsystem> BoundProfile;
	TWeakObjectPtr<class UStageAcquireDispatchSubsystem> BoundDispatch;
	FDelegateHandle PreviewReadyHandle;

	/** Lives as long as this widget: the preview world is destroyed with it. Shared (not unique) because the type is private. */
	TSharedPtr<class FStageMarketplacePreview> Preview;

	/** What the browser asks for; Offset pages through it. */
	FStageMarketplaceQuery Query;
	/** The listings the cards were built from (a copy, so a card index stays valid until the next rebuild). */
	TArray<FStageMarketplaceListing> ShownListings;
	FGuid SelectedListing;
	/** The listing in the preview (or loading into it). */
	FGuid PreviewListing;
	/** Listing -> scene it was imported as, this session: enables OPEN IN STAGE. */
	TMap<FGuid, FString> ImportedScenes;
	/** Set by a first OPEN click on a dirty stage; the second click on the same listing discards and opens. */
	FGuid ConfirmOpenListing;
	EStagePackageKind PublishKind = EStagePackageKind::StageDesign;
	/** Off by default: a publisher opts in to letting buyers export (ADR 0022 §11.1). */
	bool bPublishAllowExport = false;
};
