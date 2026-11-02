// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageAcquireDialogWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Bridge/StageBridgeProtocol.h"
#include "Bridge/StageBridgeSubsystem.h"
#include "Bridge/StageSceneExporter.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Import/StageFileDialog.h"
#include "Marketplace/StageAcquireDispatchSubsystem.h"
#include "Marketplace/StageDispatchRules.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "Services/StageAIToolSubsystem.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"
#include "Workspace/StageWorkspaceSubsystem.h"
#include "Workspace/StageWorkspaceTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageAcquireDialogWidget)

#define LOCTEXT_NAMESPACE "StageAcquireDialog"

namespace StageAcquireDialog
{
	constexpr float CardWidth = 560.f;
	constexpr float SectionGap = 12.f;

	void SetLabel(UStageChoiceButton* Button, const FText& Label)
	{
		if (UTextBlock* Text = Button ? Cast<UTextBlock>(Button->GetContent()) : nullptr)
		{
			Text->SetText(Label);
		}
	}
}

// --- Construction ---

const UStageCraftUITheme& UStageAcquireDialogWidget::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageAcquireDialogWidget::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

void UStageAcquireDialogWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

UTextBlock* UStageAcquireDialogWidget::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UStageChoiceButton* UStageAcquireDialogWidget::MakeButton(const FText& Label, const FText& Tip, int32 Index, void (UStageAcquireDialogWidget::*Handler)(int32))
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, Handler);
	UTextBlock* Text = MakeText(Label, 8, TEXT("Bold"), GetTheme().TextSecondary);
	Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Text)))
	{
		LabelSlot->SetPadding(FMargin(10.f, 4.f));
	}
	StyleChoice(*Button, false);
	return Button;
}

UWidget* UStageAcquireDialogWidget::MakeSectionLabel(const FText& Label)
{
	return MakeText(Label, 7, TEXT("Bold"), GetTheme().TextSecondary);
}

void UStageAcquireDialogWidget::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	FButtonStyle Style = bSelected
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bSelected)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bSelected ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageAcquireDialogWidget::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("DialogSize"));
	Size->SetWidthOverride(StageAcquireDialog::CardWidth);
	WidgetTree->RootWidget = Size;

	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("DialogCard"));
	Card->SetBrush(MakeBox(Palette.PanelBackground, CornerRadius, Palette.AccentColor, 1.f));
	Card->SetPadding(FMargin(16.f, 12.f));
	Size->SetContent(Card);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("DialogColumn"));
	Card->SetContent(Column);
	const auto Add = [Column](UWidget* Widget, float Top = 3.f) { Column->AddChildToVerticalBox(Widget)->SetPadding(FMargin(0.f, Top, 0.f, 0.f)); };

	// The listing.
	Add(MakeText(LOCTEXT("Heading", "GET FROM THE MARKETPLACE"), 9, TEXT("Bold"), Palette.AccentColor), 0.f);
	TitleText = MakeText(FText::GetEmpty(), 13, TEXT("Bold"), Palette.TextPrimary);
	TitleText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(TitleText, 6.f);
	BylineText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	Add(BylineText);
	RightsText = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.TextSecondary);
	RightsText->SetAutoWrapText(true);
	Add(RightsText);

	// The wallet.
	Add(MakeSectionLabel(LOCTEXT("Wallet", "YOUR CREDITS")), StageAcquireDialog::SectionGap);
	BalanceText = MakeText(FText::GetEmpty(), 9, TEXT("Regular"), Palette.TextPrimary);
	PriceText = MakeText(FText::GetEmpty(), 9, TEXT("Regular"), Palette.TextPrimary);
	AfterText = MakeText(FText::GetEmpty(), 9, TEXT("Bold"), Palette.TextPrimary);
	UHorizontalBox* Sums = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	for (UTextBlock* Sum : { BalanceText.Get(), PriceText.Get(), AfterText.Get() })
	{
		Sums->AddChildToHorizontalBox(Sum)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	Add(Sums);
	WalletNoteText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	WalletNoteText->SetAutoWrapText(true);
	Add(WalletNoteText);

	Buttons.SetNum(int32(EButton::Count));
	Buttons[int32(EButton::GetCredits)] = MakeButton(LOCTEXT("GetCredits", "GET CREDITS..."),
		LOCTEXT("GetCreditsTip", "Buy credits from the StageCraft store. Needs the online store; offline it explains why it is unavailable."), int32(EButton::GetCredits), &ThisClass::HandleButton);
	UHorizontalBox* TopUpRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	TopUpRow->AddChildToHorizontalBox(Buttons[int32(EButton::GetCredits)]);
#if !UE_BUILD_SHIPPING
	// Not built at all in Shipping (Buttons[DevGrant] stays null there), so neither the button nor its text ships.
	Buttons[int32(EButton::DevGrant)] = MakeButton(LOCTEXT("DevGrant", "DEV: ADD THE MISSING CREDITS"),
		LOCTEXT("DevGrantTip", "Development builds only: adds the credits this purchase is short of. Not in shipped builds."), int32(EButton::DevGrant), &ThisClass::HandleButton);
	TopUpRow->AddChildToHorizontalBox(Buttons[int32(EButton::DevGrant)])->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
#endif
	Add(TopUpRow);
	StoreText = MakeText(FText::GetEmpty(), 7, TEXT("Italic"), Palette.TextSecondary);
	StoreText->SetAutoWrapText(true);
	Add(StoreText);

	// The targets.
	Add(MakeSectionLabel(LOCTEXT("SendTo", "THEN SEND IT TO")), StageAcquireDialog::SectionGap);
	TargetList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("TargetList"));
	Add(TargetList);
	Buttons[int32(EButton::ConnectApps)] = MakeButton(LOCTEXT("ConnectApps", "3D APP CONNECTION..."),
		LOCTEXT("ConnectAppsTip", "Open Send to 3D App to switch on the bridge so Blender, 3ds Max, Cinema 4D or Maya can connect."), int32(EButton::ConnectApps), &ThisClass::HandleButton);
	Buttons[int32(EButton::ManageKeys)] = MakeButton(LOCTEXT("ManageKeys", "SERVICES & KEYS..."),
		LOCTEXT("ManageKeysTip", "Open Services & Keys to add or check the API keys of AI tools."), int32(EButton::ManageKeys), &ThisClass::HandleButton);
	UHorizontalBox* LinkRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	LinkRow->AddChildToHorizontalBox(Buttons[int32(EButton::ConnectApps)]);
	LinkRow->AddChildToHorizontalBox(Buttons[int32(EButton::ManageKeys)])->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	Add(LinkRow, 6.f);

	// The outcome and the decision.
	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	Add(StatusText, StageAcquireDialog::SectionGap);

	Buttons[int32(EButton::Confirm)] = MakeButton(FText::GetEmpty(), LOCTEXT("ConfirmTip", "Does exactly what it says; the price is the one shown above."), int32(EButton::Confirm), &ThisClass::HandleButton);
	Buttons[int32(EButton::Close)] = MakeButton(LOCTEXT("Cancel", "CANCEL"), LOCTEXT("CancelTip", "Close (Esc). An order already started carries on."), int32(EButton::Close), &ThisClass::HandleButton);
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UHorizontalBoxSlot* ConfirmSlot = Footer->AddChildToHorizontalBox(Buttons[int32(EButton::Confirm)]))
	{
		ConfirmSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	Footer->AddChildToHorizontalBox(Buttons[int32(EButton::Close)])->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	Add(Footer, 8.f);
}

// --- Lifetime ---

void UStageAcquireDialogWidget::NativeConstruct()
{
	Super::NativeConstruct();
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}
	if (UStageMarketplaceSubsystem* Marketplace = GameInstance->GetSubsystem<UStageMarketplaceSubsystem>())
	{
		BoundMarketplace = Marketplace;
		Marketplace->OnListingsUpdated.AddUniqueDynamic(this, &ThisClass::HandleListingsUpdated);
	}
	if (UStageProfileSubsystem* Profile = GameInstance->GetSubsystem<UStageProfileSubsystem>())
	{
		BoundProfile = Profile;
		Profile->OnBalanceChanged.AddUniqueDynamic(this, &ThisClass::HandleBalanceChanged);
		Profile->OnEntitlementsChanged.AddUniqueDynamic(this, &ThisClass::HandleRightsChanged);
		Profile->OnProfileLoaded.AddUniqueDynamic(this, &ThisClass::HandleRightsChanged);
	}
	if (UStageEconomySubsystem* Economy = GameInstance->GetSubsystem<UStageEconomySubsystem>())
	{
		BoundEconomy = Economy;
		Economy->OnCreditTopUpCompleted.AddUniqueDynamic(this, &ThisClass::HandleTopUpCompleted);
	}
	if (UStageBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UStageBridgeSubsystem>())
	{
		BoundBridge = Bridge;
		Bridge->OnStateChanged.AddUniqueDynamic(this, &ThisClass::HandleBridgeStateChanged);
		Bridge->OnClientsChanged.AddUniqueDynamic(this, &ThisClass::HandleRightsChanged);
	}
	if (UStageAIToolSubsystem* Tools = GameInstance->GetSubsystem<UStageAIToolSubsystem>())
	{
		BoundTools = Tools;
		Tools->OnToolsChanged.AddUniqueDynamic(this, &ThisClass::HandleRightsChanged);
	}
	if (UStageAcquireDispatchSubsystem* Dispatch = GameInstance->GetSubsystem<UStageAcquireDispatchSubsystem>())
	{
		BoundDispatch = Dispatch;
		Dispatch->OnOrderProgress.AddUniqueDynamic(this, &ThisClass::HandleOrderProgress);
		Dispatch->OnOrderCompleted.AddUniqueDynamic(this, &ThisClass::HandleOrderCompleted);
	}
	Refresh();
}

void UStageAcquireDialogWidget::NativeDestruct()
{
	if (UStageMarketplaceSubsystem* Marketplace = BoundMarketplace.Get())
	{
		Marketplace->OnListingsUpdated.RemoveDynamic(this, &ThisClass::HandleListingsUpdated);
	}
	if (UStageProfileSubsystem* Profile = BoundProfile.Get())
	{
		Profile->OnBalanceChanged.RemoveDynamic(this, &ThisClass::HandleBalanceChanged);
		Profile->OnEntitlementsChanged.RemoveDynamic(this, &ThisClass::HandleRightsChanged);
		Profile->OnProfileLoaded.RemoveDynamic(this, &ThisClass::HandleRightsChanged);
	}
	if (UStageEconomySubsystem* Economy = BoundEconomy.Get())
	{
		Economy->OnCreditTopUpCompleted.RemoveDynamic(this, &ThisClass::HandleTopUpCompleted);
	}
	if (UStageBridgeSubsystem* Bridge = BoundBridge.Get())
	{
		Bridge->OnStateChanged.RemoveDynamic(this, &ThisClass::HandleBridgeStateChanged);
		Bridge->OnClientsChanged.RemoveDynamic(this, &ThisClass::HandleRightsChanged);
	}
	if (UStageAIToolSubsystem* Tools = BoundTools.Get())
	{
		Tools->OnToolsChanged.RemoveDynamic(this, &ThisClass::HandleRightsChanged);
	}
	if (UStageAcquireDispatchSubsystem* Dispatch = BoundDispatch.Get())
	{
		Dispatch->OnOrderProgress.RemoveDynamic(this, &ThisClass::HandleOrderProgress);
		Dispatch->OnOrderCompleted.RemoveDynamic(this, &ThisClass::HandleOrderCompleted);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RefreshTimer);
	}
	BoundMarketplace.Reset();
	BoundProfile.Reset();
	BoundEconomy.Reset();
	BoundBridge.Reset();
	BoundTools.Reset();
	BoundDispatch.Reset();
	OnCloseRequested.Unbind();
	Super::NativeDestruct();
}

FReply UStageAcquireDialogWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		OnCloseRequested.ExecuteIfBound();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UStageAcquireDialogWidget::ShowListing(const FGuid& InListingId)
{
	if (ListingId != InListingId)
	{
		ListingId = InListingId;
		SelectedTarget = 0;
		ChosenFilePath.Reset();
		bOwnOrder = false;
		SetStatus(FText::GetEmpty(), false);
	}
	Refresh();
}

// --- Reading ---

const FStageMarketplaceListing* UStageAcquireDialogWidget::GetListing() const
{
	const UStageMarketplaceSubsystem* Marketplace = BoundMarketplace.Get();
	return Marketplace ? Marketplace->FindListing(ListingId) : nullptr;
}

EStageOwnershipState UStageAcquireDialogWidget::GetOwnership() const
{
	const UStageMarketplaceSubsystem* Marketplace = BoundMarketplace.Get();
	return Marketplace ? Marketplace->GetListingOwnership(ListingId) : EStageOwnershipState::Locked;
}

FStageEconomyResultInfo UStageAcquireDialogWidget::GetPurchaseCheck() const
{
	switch (GetOwnership())
	{
	case EStageOwnershipState::Free:
	case EStageOwnershipState::Owned:
		return FStageEconomyResultInfo::Ok();
	default:
		break;
	}
	const UStageEconomySubsystem* Economy = BoundEconomy.Get();
	return Economy ? Economy->CanPurchaseListing(ListingId)
		: FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoEconomy", "The shop is not available."));
}

int64 UStageAcquireDialogWidget::GetShortfall() const
{
	const FStageMarketplaceListing* Listing = GetListing();
	const UStageProfileSubsystem* Profile = BoundProfile.Get();
	if (!Listing || !Profile || GetOwnership() != EStageOwnershipState::Purchasable)
	{
		return 0;
	}
	return FMath::Max<int64>(0, Listing->Price.Credits - Profile->GetBalance(StageCraftTags::Currency_Credits));
}

FText UStageAcquireDialogWidget::DescribeTarget(const FStageDispatchTarget& Target) const
{
	switch (Target.Kind)
	{
	case EStageDispatchKind::SaveFile:
		return LOCTEXT("TargetFileShort", ".GLB FILE");
	case EStageDispatchKind::BridgeApp:
		if (const UStageBridgeSubsystem* Bridge = BoundBridge.Get())
		{
			for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
			{
				if (Client.ClientId == Target.BridgeClientId)
				{
					return FText::FromString(StageBridgeProtocol::GetAppDisplayName(Client.App).ToString().ToUpper());
				}
			}
		}
		return LOCTEXT("TargetAppShort", "THE 3D APP");
	case EStageDispatchKind::AITool:
		if (const UStageAIToolSubsystem* Tools = BoundTools.Get())
		{
			FStageAIToolInfo Info;
			if (Tools->FindTool(Target.ProviderId, Info))
			{
				return FText::FromString(Info.DisplayName.ToString().ToUpper());
			}
		}
		return LOCTEXT("TargetToolShort", "THE AI TOOL");
	case EStageDispatchKind::OpenInStageCraft:
	default:
		return LOCTEXT("TargetOpenShort", "OPEN IN STAGECRAFT");
	}
}

FText UStageAcquireDialogWidget::DescribeConfirm() const
{
	const FStageMarketplaceListing* Listing = GetListing();
	if (!Listing || !Targets.IsValidIndex(SelectedTarget))
	{
		return LOCTEXT("ConfirmNone", "NOT AVAILABLE");
	}
	const FStageDispatchTarget& Target = Targets[SelectedTarget];
	const FText Where = Target.Kind == EStageDispatchKind::OpenInStageCraft ? LOCTEXT("VerbOpen", "OPEN")
		: Target.Kind == EStageDispatchKind::SaveFile ? LOCTEXT("VerbSave", "SAVE AS .GLB")
		: FText::Format(LOCTEXT("VerbSend", "SEND TO {0}"), DescribeTarget(Target));
	return GetOwnership() == EStageOwnershipState::Purchasable
		? FText::Format(LOCTEXT("ConfirmBuy", "BUY FOR {0} CREDITS & {1}"), FText::AsNumber(Listing->Price.Credits), Where)
		: FText::Format(LOCTEXT("ConfirmGet", "GET & {0}"), Where);
}

void UStageAcquireDialogWidget::SetStatus(const FText& Message, bool bWarning)
{
	if (StatusText)
	{
		StatusText->SetText(Message);
		StatusText->SetColorAndOpacity(FSlateColor(bWarning ? GetTheme().WarningColor : GetTheme().TextSecondary));
	}
}

// --- Refreshing ---

void UStageAcquireDialogWidget::RequestRefresh()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (!RefreshTimer.IsValid())
	{
		RefreshTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			RefreshTimer.Invalidate();
			Refresh();
		}));
	}
}

void UStageAcquireDialogWidget::Refresh()
{
	RefreshWallet();
	RebuildTargets();
	RefreshFooter();
}

void UStageAcquireDialogWidget::RefreshWallet()
{
	const UStageCraftUITheme& Palette = GetTheme();
	const FStageMarketplaceListing* Listing = GetListing();
	const UStageProfileSubsystem* Profile = BoundProfile.Get();
	const UStageEconomySubsystem* Economy = BoundEconomy.Get();
	const UStageMarketplaceSubsystem* Marketplace = BoundMarketplace.Get();
	if (!Listing)
	{
		TitleText->SetText(LOCTEXT("Gone", "This listing is no longer in the results"));
		BylineText->SetText(LOCTEXT("GoneHint", "Close this and search again."));
		RightsText->SetText(FText::GetEmpty());
		WalletNoteText->SetText(FText::GetEmpty());
		Buttons[int32(EButton::GetCredits)]->SetVisibility(ESlateVisibility::Collapsed);
#if !UE_BUILD_SHIPPING
		Buttons[int32(EButton::DevGrant)]->SetVisibility(ESlateVisibility::Collapsed);
#endif
		return;
	}

	TitleText->SetText(FText::FromString(Listing->Title));
	BylineText->SetText(FText::Format(LOCTEXT("Byline", "by {0}  ·  {1} items  ·  {2} fixtures  ·  {3} cues"),
		FText::FromString(Listing->AuthorName), Listing->ItemCount, Listing->FixtureCount, Listing->CueCount));
	RightsText->SetText(Listing->bAllowsExport
		? LOCTEXT("RightsOpen", "The publisher allows this stage to go to 3D apps, AI tools and .glb files.")
		: LOCTEXT("RightsClosed", "The publisher keeps this stage inside StageCraft: you can open and use it here, but not export it."));
	RightsText->SetColorAndOpacity(FSlateColor(Listing->bAllowsExport ? Palette.AccentColor : Palette.TextSecondary));

	const int64 Balance = Profile ? Profile->GetBalance(StageCraftTags::Currency_Credits) : 0;
	const EStageOwnershipState Ownership = GetOwnership();
	const bool bCharged = Ownership == EStageOwnershipState::Purchasable;
	const int64 Price = bCharged ? Listing->Price.Credits : 0;
	BalanceText->SetText(FText::Format(LOCTEXT("Balance", "Balance  {0}"), FText::AsNumber(Balance)));
	PriceText->SetText(Ownership == EStageOwnershipState::Free ? LOCTEXT("PriceFree", "Price  FREE")
		: Ownership == EStageOwnershipState::Owned ? LOCTEXT("PriceOwned", "Price  OWNED")
		: FText::Format(LOCTEXT("Price", "Price  {0}"), FText::AsNumber(Listing->Price.Credits)));
	const int64 After = Balance - Price;
	AfterText->SetText(FText::Format(LOCTEXT("After", "After  {0}"), FText::AsNumber(After)));
	AfterText->SetColorAndOpacity(FSlateColor(After < 0 ? Palette.WarningColor : Palette.TextPrimary));

	const FStageEconomyResultInfo Check = GetPurchaseCheck();
	const int64 Shortfall = GetShortfall();
	FText Note;
	switch (Ownership)
	{
	case EStageOwnershipState::Free:
		Note = LOCTEXT("NoteFree", "Free: nothing is charged.");
		break;
	case EStageOwnershipState::Owned:
		Note = LOCTEXT("NoteOwned", "You own this listing; getting it again adds another copy to your scenes and costs nothing.");
		break;
	default:
		Note = Check.IsSuccess() ? LOCTEXT("NoteReady", "Enough credits. Nothing is charged until you confirm.")
			: Check.Code == EStageEconomyResult::InsufficientFunds ? FText::Format(LOCTEXT("NoteShort", "{0} more credits are needed."), FText::AsNumber(Shortfall))
			: Check.Message;
		break;
	}
	WalletNoteText->SetText(Note);
	WalletNoteText->SetColorAndOpacity(FSlateColor(Check.IsSuccess() ? Palette.TextSecondary : Palette.WarningColor));

	const bool bShort = Check.Code == EStageEconomyResult::InsufficientFunds && Shortfall > 0;
	Buttons[int32(EButton::GetCredits)]->SetVisibility(bShort ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
#if !UE_BUILD_SHIPPING
	Buttons[int32(EButton::DevGrant)]->SetVisibility(bShort ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
#endif

	const bool bStore = Economy && Economy->CanSellCredits();
	StoreText->SetText(FText::Format(LOCTEXT("Store", "{0}  ·  {1}  ·  {2} entitlements on this profile. Subscriptions arrive as entitlements from the online store."),
		Marketplace ? Marketplace->GetBackendName() : FText::GetEmpty(),
		bStore ? LOCTEXT("StoreOnline", "credits can be bought") : LOCTEXT("StoreOffline", "offline: credits cannot be bought"),
		FText::AsNumber(Profile ? Profile->GetEntitlements().Num() : 0)));
}

void UStageAcquireDialogWidget::RebuildTargets()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageAcquireDispatchSubsystem* Dispatch = BoundDispatch.Get();
	const UStageBridgeSubsystem* Bridge = BoundBridge.Get();
	const UStageAIToolSubsystem* Tools = BoundTools.Get();

	// Identity of the selected row, so a rebuild (an app connecting) keeps it.
	const FStageDispatchTarget Previous = Targets.IsValidIndex(SelectedTarget) ? Targets[SelectedTarget] : FStageDispatchTarget();

	Targets.Reset();
	Targets.Add(FStageDispatchTarget());
	FStageDispatchTarget File;
	File.Kind = EStageDispatchKind::SaveFile;
	File.FilePath = ChosenFilePath;
	Targets.Add(File);
	if (Bridge)
	{
		for (const FStageBridgeClientInfo& Client : Bridge->GetClients())
		{
			FStageDispatchTarget App;
			App.Kind = EStageDispatchKind::BridgeApp;
			App.BridgeClientId = Client.ClientId;
			Targets.Add(App);
		}
	}
	if (Tools)
	{
		for (const FStageAIToolInfo& Tool : Tools->GetTools())
		{
			if (Tool.Can(EStageAIToolCapability::AcceptModel))
			{
				FStageDispatchTarget ToolTarget;
				ToolTarget.Kind = EStageDispatchKind::AITool;
				ToolTarget.ProviderId = Tool.ProviderId;
				Targets.Add(ToolTarget);
			}
		}
	}
	SelectedTarget = Targets.IndexOfByPredicate([&Previous](const FStageDispatchTarget& Target)
	{
		return Target.Kind == Previous.Kind && Target.BridgeClientId == Previous.BridgeClientId && Target.ProviderId == Previous.ProviderId;
	});
	SelectedTarget = FMath::Max(SelectedTarget, 0);

	TargetList->ClearChildren();
	TargetButtons.Reset();
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		const FStageDispatchTarget& Target = Targets[Index];
		const EStageDispatchAvailability Availability = Dispatch ? Dispatch->EvaluateTarget(ListingId, Target) : EStageDispatchAvailability::Unavailable;
		const bool bAvailable = Availability == EStageDispatchAvailability::Available;
		const bool bSelected = Index == SelectedTarget;

		FText Name;
		FText Detail;
		switch (Target.Kind)
		{
		case EStageDispatchKind::OpenInStageCraft:
			Name = LOCTEXT("RowOpen", "Open in StageCraft");
			Detail = LOCTEXT("RowOpenDetail", "Added to your scenes and opened on the stage (you are asked first if the stage has unsaved changes).");
			break;
		case EStageDispatchKind::SaveFile:
			Name = LOCTEXT("RowFile", "Save as a .glb file");
			Detail = ChosenFilePath.IsEmpty() ? LOCTEXT("RowFileDetail", "Pick this to choose where; you choose before anything is bought.")
				: FText::Format(LOCTEXT("RowFileChosen", "To {0}"), FText::FromString(ChosenFilePath));
			break;
		case EStageDispatchKind::BridgeApp:
		{
			const FStageBridgeClientInfo* Client = Bridge ? Bridge->GetClients().FindByPredicate([&Target](const FStageBridgeClientInfo& Info) { return Info.ClientId == Target.BridgeClientId; }) : nullptr;
			Name = Client ? FText::Format(LOCTEXT("RowApp", "{0} {1}"), StageBridgeProtocol::GetAppDisplayName(Client->App), FText::FromString(Client->AppVersion))
				: LOCTEXT("RowAppGone", "3D app");
			Detail = LOCTEXT("RowAppDetail", "Connected through its StageCraft add-on; imported as glTF.");
			break;
		}
		case EStageDispatchKind::AITool:
		{
			FStageAIToolInfo Info;
			Name = Tools && Tools->FindTool(Target.ProviderId, Info) ? Info.DisplayName : FText::FromName(Target.ProviderId);
			Detail = LOCTEXT("RowToolDetail", "AI tool: receives the finished model.");
			break;
		}
		}
		if (!bAvailable)
		{
			Detail = StageDispatchRules::Describe(Availability);
		}

		UStageChoiceButton* Row = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Row->SetChoiceIndex(Index);
		Row->OnPicked.BindUObject(this, &ThisClass::HandleTargetPicked);
		UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Content->AddChildToVerticalBox(MakeText(Name, 9, TEXT("Bold"), bAvailable ? Palette.TextPrimary : Palette.TextDisabled));
		UTextBlock* DetailText = MakeText(Detail, 7, TEXT("Regular"), bAvailable ? Palette.TextSecondary : Palette.WarningColor);
		DetailText->SetAutoWrapText(true);
		Content->AddChildToVerticalBox(DetailText);
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Row->AddChild(Content)))
		{
			ContentSlot->SetPadding(FMargin(8.f, 4.f));
			ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		}
		FButtonStyle Style = MakeFlatButton(Palette.RowBackground, Palette.RowHover, Palette.RowHover);
		Style.SetNormal(MakeBox(bSelected ? Palette.RowHover : Palette.RowBackground, CornerRadius, bSelected ? Palette.AccentColor : Palette.Divider, 1.f));
		Style.SetHovered(MakeBox(Palette.RowHover, CornerRadius, bSelected ? Palette.AccentColor : Palette.Divider, 1.f));
		Row->SetStyle(Style);
		// A row that cannot be used stays readable (it says why) but cannot be picked.
		Row->SetIsEnabled(bAvailable);
		TargetList->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
		TargetButtons.Add(Row);
	}

	Buttons[int32(EButton::ConnectApps)]->SetVisibility(Bridge && Bridge->GetClients().IsEmpty() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	const bool bAnyToolNeedsKey = Tools && Tools->GetTools().ContainsByPredicate([](const FStageAIToolInfo& Info) { return !Info.HasUsableKey(); });
	Buttons[int32(EButton::ManageKeys)]->SetVisibility(bAnyToolNeedsKey ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void UStageAcquireDialogWidget::RefreshFooter()
{
	const UStageAcquireDispatchSubsystem* Dispatch = BoundDispatch.Get();
	const UStageMarketplaceSubsystem* Marketplace = BoundMarketplace.Get();
	const bool bOrderRunning = Dispatch && Dispatch->IsOrderPending();
	const bool bAcquiring = Marketplace && Marketplace->IsAcquiring(ListingId);
	const bool bTargetReady = Targets.IsValidIndex(SelectedTarget) && Dispatch
		&& Dispatch->EvaluateTarget(ListingId, Targets[SelectedTarget]) == EStageDispatchAvailability::Available
		&& StageDispatchRules::HasValidPath(Targets[SelectedTarget]);

	UStageChoiceButton* Confirm = Buttons[int32(EButton::Confirm)];
	StageAcquireDialog::SetLabel(Confirm, bOrderRunning ? LOCTEXT("ConfirmWorking", "WORKING...") : DescribeConfirm());
	const bool bEnabled = GetListing() && GetPurchaseCheck().IsSuccess() && bTargetReady && !bOrderRunning && !bAcquiring;
	Confirm->SetIsEnabled(bEnabled);
	StyleChoice(*Confirm, bEnabled);
	StageAcquireDialog::SetLabel(Buttons[int32(EButton::Close)], bOwnOrder || bOrderRunning ? LOCTEXT("Close", "CLOSE") : LOCTEXT("Cancel", "CANCEL"));
}

// --- Actions ---

bool UStageAcquireDialogWidget::ChooseFilePath()
{
	const FStageMarketplaceListing* Listing = GetListing();
	const FString Exports = FPaths::Combine(FPlatformProcess::UserDir(), TEXT("StageCraft Exports"));
	FString Path;
	if (!StageFileDialog::SaveFile(LOCTEXT("SaveTitle", "Save the stage as glTF"), Exports,
		(Listing ? FPaths::MakeValidFileName(Listing->Title) : FString(TEXT("Stage"))) + FStageSceneExporter::FileExtension, TEXT("glTF binary (*.glb)|*.glb"), Path))
	{
		return false;
	}
	if (!Path.EndsWith(FStageSceneExporter::FileExtension, ESearchCase::IgnoreCase))
	{
		Path += FStageSceneExporter::FileExtension;
	}
	ChosenFilePath = Path;
	return true;
}

void UStageAcquireDialogWidget::HandleTargetPicked(int32 Index)
{
	if (!Targets.IsValidIndex(Index))
	{
		return;
	}
	// The path is chosen now, before anything is bought; cancelling keeps the previous choice.
	if (Targets[Index].Kind == EStageDispatchKind::SaveFile && !ChooseFilePath())
	{
		return;
	}
	SelectedTarget = Index;
	SetStatus(FText::GetEmpty(), false);
	RequestRefresh();
}

void UStageAcquireDialogWidget::HandleButton(int32 Index)
{
	AModularPlayerController* Controller = GetStageController();
	switch (static_cast<EButton>(Index))
	{
	case EButton::Confirm:
	{
		const FStageMarketplaceListing* Listing = GetListing();
		if (!Controller || !Listing || !Targets.IsValidIndex(SelectedTarget))
		{
			return;
		}
		FStageAcquireOrder Order;
		Order.ListingId = ListingId;
		// The price on screen, so a price that changed meanwhile is refused instead of charged.
		Order.ExpectedCredits = Listing->Price.Credits;
		Order.Target = Targets[SelectedTarget];
		const TArray<FStageEconomyResultInfo> Refusals = Controller->RunCapturingRejections([&Controller, &Order]() { Controller->RequestAcquireAndDispatch(Order); });
		if (Refusals.IsEmpty())
		{
			bOwnOrder = true;
		}
		else
		{
			SetStatus(Refusals[0].Message, true);
		}
		RefreshFooter();
		return;
	}

	case EButton::Close:
		OnCloseRequested.ExecuteIfBound();
		return;

	case EButton::GetCredits:
	{
		const int64 Shortfall = GetShortfall();
		if (Controller && Shortfall > 0)
		{
			const TArray<FStageEconomyResultInfo> Refusals = Controller->RunCapturingRejections([&Controller, Shortfall]() { Controller->RequestCreditTopUp(Shortfall); });
			SetStatus(Refusals.IsEmpty() ? LOCTEXT("TopUpStarted", "Asking the store for credits...") : Refusals[0].Message, !Refusals.IsEmpty());
		}
		return;
	}

	case EButton::DevGrant:
#if !UE_BUILD_SHIPPING
		if (Controller && GetShortfall() > 0)
		{
			const bool bGranted = Controller->RequestDevGrantCredits(GetShortfall());
			SetStatus(bGranted ? LOCTEXT("DevGranted", "Development: the missing credits were added.") : LOCTEXT("DevRefused", "Development: credits could not be added (frozen profile?)."), !bGranted);
		}
#endif
		return;

	case EButton::ManageKeys:
	case EButton::ConnectApps:
		if (UStageWorkspaceSubsystem* Workspace = GetGameInstance() ? GetGameInstance()->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr)
		{
			Workspace->OpenPanel(static_cast<EButton>(Index) == EButton::ManageKeys ? FGameplayTag(StageCraftTags::Panel_Services) : FGameplayTag(StageCraftTags::Panel_SendTo));
		}
		return;

	default:
		return;
	}
}

// --- Events ---

void UStageAcquireDialogWidget::HandleBalanceChanged(FGameplayTag Currency, int64 NewBalance)
{
	RequestRefresh();
}

void UStageAcquireDialogWidget::HandleRightsChanged()
{
	RequestRefresh();
}

void UStageAcquireDialogWidget::HandleBridgeStateChanged(bool bRunning)
{
	RequestRefresh();
}

void UStageAcquireDialogWidget::HandleListingsUpdated(FStageMarketplaceResultInfo Result)
{
	RequestRefresh();
}

void UStageAcquireDialogWidget::HandleTopUpCompleted(FGameplayTag Currency, int64 GrantedAmount, FStageEconomyResultInfo Result)
{
	SetStatus(Result.IsSuccess() ? FText::Format(LOCTEXT("TopUpDone", "{0} credits added."), FText::AsNumber(GrantedAmount)) : Result.Message, !Result.IsSuccess());
	RequestRefresh();
}

void UStageAcquireDialogWidget::HandleOrderProgress(FGuid InListingId, EStageOrderStage Stage)
{
	if (InListingId != ListingId || Stage == EStageOrderStage::Idle)
	{
		return;
	}
	const FStageMarketplaceListing* Listing = GetListing();
	const FText Title = Listing ? FText::FromString(Listing->Title) : FText::GetEmpty();
	SetStatus(Stage == EStageOrderStage::Acquiring
		? FText::Format(LOCTEXT("Acquiring", "Getting \"{0}\": buying if needed, downloading and checking the package..."), Title)
		: FText::Format(LOCTEXT("Delivering", "Sending \"{0}\" to {1}..."), Title, Targets.IsValidIndex(SelectedTarget) ? DescribeTarget(Targets[SelectedTarget]) : FText::GetEmpty()), false);
	RefreshFooter();
}

void UStageAcquireDialogWidget::HandleOrderCompleted(const FStageOrderResultInfo& Result)
{
	if (Result.ListingId != ListingId)
	{
		return;
	}
	SetStatus(Result.Message, !Result.IsSuccess());
	if (StatusText && Result.IsSuccess())
	{
		StatusText->SetColorAndOpacity(FSlateColor(GetTheme().AccentColor));
	}
	RequestRefresh();
}

#undef LOCTEXT_NAMESPACE
