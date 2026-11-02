// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Bridge/StageBridgeTypes.h"
#include "Economy/StageEconomyTypes.h"
#include "Engine/TimerHandle.h"
#include "Marketplace/StageDispatchTypes.h"
#include "Marketplace/StageMarketplaceTypes.h"
#include "StageAcquireDialogWidget.generated.h"

DECLARE_DELEGATE(FOnStageAcquireDialogClose);

/**
 * The acquire dialog (Docs/ADR/0022-dcc-bridge.md §11.3): shown over the Marketplace panel when the user picks GET or BUY. It shows
 * the listing, the wallet (balance, price, balance afterwards, and why buying is blocked when it is), the targets the stage can go
 * to (StageCraft, a .glb file, each connected 3D app, each AI tool that takes models) with the reason any of them cannot be used,
 * and one confirm button that names exactly what will happen.
 *
 * A view in the project's binding pattern:
 *  - Reads: the marketplace (listing, ownership), the economy (CanPurchaseListing, CanSellCredits), the profile (balance), the
 *    bridge (clients), the AI tools (key states) and the dispatcher (EvaluateTarget, the order in flight). Display only: the
 *    dispatcher, the economy and the exporter check everything again.
 *  - Writes go through the controller: RequestAcquireAndDispatch with the price shown, RequestCreditTopUp, RequestDevGrantCredits
 *    (development builds). The .glb path is chosen before buying, so cancelling the file dialog never leaves a bought stage unsent.
 *  - Push, never poll: bound in NativeConstruct, unbound in NativeDestruct, no tick; changes rebuild the targets on the next tick.
 *  - Closing (CLOSE, Esc) never cancels an order: the dispatcher owns it and the Marketplace panel reports its end.
 *
 * Built in code from the theme; created by UStageMarketplaceWidget, which owns its lifetime.
 */
UCLASS(meta = (DisableNativeTick))
class MODULARSCENEBUILDER_API UStageAcquireDialogWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Shows ListingId, a listing of the marketplace's current results. */
	void ShowListing(const FGuid& InListingId);

	/** The user asked to close (CLOSE or Esc). The owner removes the dialog. */
	FOnStageAcquireDialogClose OnCloseRequested;

protected:
	//~ Begin UUserWidget Interface
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	//~ End UUserWidget Interface

	UPROPERTY(EditDefaultsOnly, Category = "StageCraft|Marketplace")
	TObjectPtr<class UStageCraftUITheme> Theme = nullptr;

private:
	enum class EButton : int32 { Confirm, Close, GetCredits, DevGrant, ManageKeys, ConnectApps, Count };

	void BuildTree();
	class UTextBlock* MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color);
	class UStageChoiceButton* MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageAcquireDialogWidget::*Handler)(int32));
	class UWidget* MakeSectionLabel(const FText& Label);
	const class UStageCraftUITheme& GetTheme() const;
	void StyleChoice(class UStageChoiceButton& Button, bool bSelected) const;

	/** Rebuilds everything on the next tick (never inside the callback that caused it). */
	void RequestRefresh();
	void Refresh();
	void RefreshWallet();
	void RebuildTargets();
	void RefreshFooter();

	const FStageMarketplaceListing* GetListing() const;
	EStageOwnershipState GetOwnership() const;
	/** The purchase check for display: Ok for free and owned listings. */
	FStageEconomyResultInfo GetPurchaseCheck() const;
	int64 GetShortfall() const;
	FText DescribeTarget(const FStageDispatchTarget& Target) const;
	FText DescribeConfirm() const;
	void SetStatus(const FText& Message, bool bWarning);

	void HandleButton(int32 Index);
	void HandleTargetPicked(int32 Index);
	bool ChooseFilePath();

	UFUNCTION()
	void HandleBalanceChanged(FGameplayTag Currency, int64 NewBalance);
	UFUNCTION()
	void HandleRightsChanged();
	UFUNCTION()
	void HandleTopUpCompleted(FGameplayTag Currency, int64 GrantedAmount, FStageEconomyResultInfo Result);
	UFUNCTION()
	void HandleBridgeStateChanged(bool bRunning);
	UFUNCTION()
	void HandleListingsUpdated(FStageMarketplaceResultInfo Result);
	UFUNCTION()
	void HandleOrderProgress(FGuid InListingId, EStageOrderStage Stage);
	UFUNCTION()
	void HandleOrderCompleted(const FStageOrderResultInfo& Result);

	class AModularPlayerController* GetStageController() const;

	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> TitleText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> BylineText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> RightsText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> BalanceText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> PriceText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> AfterText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> WalletNoteText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StoreText = nullptr;
	UPROPERTY(Transient)
	TObjectPtr<class UVerticalBox> TargetList = nullptr;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> TargetButtons;
	UPROPERTY(Transient)
	TObjectPtr<class UTextBlock> StatusText = nullptr;
	/** Indexed by EButton. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStageChoiceButton>> Buttons;

	TWeakObjectPtr<class UStageMarketplaceSubsystem> BoundMarketplace;
	TWeakObjectPtr<class UStageProfileSubsystem> BoundProfile;
	TWeakObjectPtr<class UStageEconomySubsystem> BoundEconomy;
	TWeakObjectPtr<class UStageBridgeSubsystem> BoundBridge;
	TWeakObjectPtr<class UStageAIToolSubsystem> BoundTools;
	TWeakObjectPtr<class UStageAcquireDispatchSubsystem> BoundDispatch;

	FGuid ListingId;
	/** What the stage can be sent to, in row order; rebuilt with the rows. */
	TArray<FStageDispatchTarget> Targets;
	int32 SelectedTarget = 0;
	/** The .glb path picked for SaveFile; kept across rebuilds. */
	FString ChosenFilePath;
	/** This dialog started the order in flight (another view may start one too). */
	bool bOwnOrder = false;
	FTimerHandle RefreshTimer;
};
