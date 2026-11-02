// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageEconomySubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Economy/StageCommerceBackend.h"
#include "Economy/StageProductData.h"
#include "Economy/StageTopUpRules.h"
#include "Economy/StageUnlockCondition.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "ModularSceneBuilder.h"
#include "Subsystems/StageProfileSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageEconomySubsystem)

#define LOCTEXT_NAMESPACE "StageCraftEconomy"

namespace StageEconomy
{
	/**
	 * Price per currency with duplicate entries summed. Checking entries one by one would let a
	 * price of "100 + 100 credits" pass with a balance of 150. False on a malformed price.
	 */
	bool SumPrice(const TArray<FStageCurrencyAmount>& Price, TMap<FGameplayTag, int64>& OutTotals)
	{
		OutTotals.Reset();
		for (const FStageCurrencyAmount& Cost : Price)
		{
			if (!Cost.Currency.MatchesTag(StageCraftTags::Currency) || Cost.Amount < 0)
			{
				return false;
			}
			int64& Total = OutTotals.FindOrAdd(Cost.Currency);
			if (Total > MAX_int64 - Cost.Amount)
			{
				return false;
			}
			Total += Cost.Amount;
		}
		return true;
	}

	FText ItemName(const UBaseItemData* Item)
	{
		return Item && !Item->DisplayName.IsEmpty() ? Item->DisplayName : FText::FromString(GetNameSafe(Item));
	}
}

void UStageEconomySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// The profile must exist before anything here reads or settles against it.
	Collection.InitializeDependency<UStageProfileSubsystem>();
	Super::Initialize(Collection);

	UClass* BackendClass = CommerceBackendClass.IsNull() ? nullptr : CommerceBackendClass.LoadSynchronous();
	if (!BackendClass || BackendClass->HasAnyClassFlags(CLASS_Abstract) || !BackendClass->IsChildOf<UStageCommerceBackend>())
	{
		if (!CommerceBackendClass.IsNull())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("CommerceBackendClass '%s' is not a concrete UStageCommerceBackend; using the local backend."), *CommerceBackendClass.ToString());
		}
		BackendClass = UStageLocalCommerceBackend::StaticClass();
	}
	Backend = NewObject<UStageCommerceBackend>(this, BackendClass);

	UAssetManager::CallOrRegister_OnCompletedInitialScan(FSimpleMulticastDelegate::FDelegate::CreateUObject(this, &ThisClass::LoadProductCatalog));
}

void UStageEconomySubsystem::Deinitialize()
{
	OnProductCatalogLoaded.Clear();
	OnPurchaseCompleted.Clear();
	OnListingPurchaseCompleted.Clear();
	OnCreditTopUpCompleted.Clear();
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		for (TPair<FGuid, FTimerHandle>& Timer : BackendTimers)
		{
			GameInstance->GetTimerManager().ClearTimer(Timer.Value);
		}
	}
	BackendTimers.Reset();
	// Answers that arrive after this are dropped by the bound delegates (weak) and by the lookups below finding nothing.
	PendingPurchases.Reset();
	PendingListingOffers.Reset();
	PendingTopUp = FStageTopUpRequest();
	UnresolvedProducts.Reset();
	UnresolvedListingOffers.Reset();
	UnresolvedTopUps.Reset();
	Products.Reset();
	Backend = nullptr;
	bCatalogLoaded = false;

	Super::Deinitialize();
}

float UStageEconomySubsystem::GetBackendTimeoutSeconds() const
{
	constexpr float DefaultTimeoutSeconds = 30.f;
	return BackendTimeoutSeconds > 0.f ? BackendTimeoutSeconds : DefaultTimeoutSeconds;
}

void UStageEconomySubsystem::StartBackendTimer(const FGuid& TransactionId)
{
	UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}
	FTimerHandle& Handle = BackendTimers.FindOrAdd(TransactionId);
	GameInstance->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateUObject(this, &ThisClass::HandleBackendTimeout, TransactionId),
		GetBackendTimeoutSeconds(), /*bLoop*/ false);
}

void UStageEconomySubsystem::StopBackendTimer(const FGuid& TransactionId)
{
	FTimerHandle Handle;
	if (BackendTimers.RemoveAndCopyValue(TransactionId, Handle))
	{
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			GameInstance->GetTimerManager().ClearTimer(Handle);
		}
	}
}

void UStageEconomySubsystem::HandleBackendTimeout(FGuid TransactionId)
{
	BackendTimers.Remove(TransactionId);
	const FText Seconds = FText::AsNumber(FMath::RoundToInt(GetBackendTimeoutSeconds()));
	const FStageEconomyResultInfo TimedOut = FStageEconomyResultInfo::Fail(EStageEconomyResult::BackendTimeout,
		FText::Format(LOCTEXT("BackendTimeout", "The store did not answer within {0} seconds. Nothing was charged here; if the store charged you, the purchase will be applied when it answers."), Seconds));

	FStageMarketplaceListing Offer;
	if (PendingListingOffers.RemoveAndCopyValue(TransactionId, Offer))
	{
		PendingPurchases.Remove(StageMarketplaceRules::MakeListingProductId(Offer.ListingId));
		UnresolvedListingOffers.Add(TransactionId, Offer);
		UE_LOG(LogStageCraft, Warning, TEXT("Purchase %s of listing %s: no answer from %s; waiting for a late answer."), *TransactionId.ToString(), *Offer.ListingId.ToString(), *GetNameSafe(Backend));
		OnListingPurchaseCompleted.Broadcast(Offer.ListingId, TimedOut);
		return;
	}
	if (PendingTopUp.TransactionId == TransactionId)
	{
		const FStageTopUpRequest Request = PendingTopUp;
		PendingTopUp = FStageTopUpRequest();
		UnresolvedTopUps.Add(TransactionId, Request);
		UE_LOG(LogStageCraft, Warning, TEXT("Top-up %s: no answer from %s; waiting for a late answer."), *TransactionId.ToString(), *GetNameSafe(Backend));
		OnCreditTopUpCompleted.Broadcast(Request.Currency, 0, TimedOut);
		return;
	}
	const FPrimaryAssetId* ProductId = PendingPurchases.FindKey(TransactionId);
	if (!ProductId)
	{
		return;
	}
	const FPrimaryAssetId Id = *ProductId;
	PendingPurchases.Remove(Id);
	const TObjectPtr<UStageProductData>* Product = Products.FindByPredicate([&Id](const UStageProductData* Candidate) { return Candidate && Candidate->GetPrimaryAssetId() == Id; });
	UnresolvedProducts.Add(TransactionId, Product ? Product->Get() : nullptr);
	UE_LOG(LogStageCraft, Warning, TEXT("Purchase %s of %s: no answer from %s; waiting for a late answer."), *TransactionId.ToString(), *Id.ToString(), *GetNameSafe(Backend));
	OnPurchaseCompleted.Broadcast(Product ? Product->Get() : nullptr, TimedOut);
}

UStageProfileSubsystem* UStageEconomySubsystem::GetProfile() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageProfileSubsystem>() : nullptr;
}

// --- Product catalog ---

void UStageEconomySubsystem::LoadProductCatalog()
{
	UAssetManager& AssetManager = UAssetManager::Get();

	TArray<FPrimaryAssetId> ProductIds;
	AssetManager.GetPrimaryAssetIdList(UStageProductData::ProductAssetType, ProductIds);

	// No products is a valid setup (everything free), so this is not a warning.
	AssetManager.LoadPrimaryAssets(ProductIds, { FName(TEXT("UI")) },
		FStreamableDelegate::CreateUObject(this, &ThisClass::HandleProductCatalogLoaded));
}

void UStageEconomySubsystem::HandleProductCatalogLoaded()
{
	TArray<UObject*> LoadedObjects;
	UAssetManager::Get().GetPrimaryAssetObjectList(UStageProductData::ProductAssetType, LoadedObjects);

	Products.Reset(LoadedObjects.Num());
	for (UObject* Object : LoadedObjects)
	{
		if (UStageProductData* Product = Cast<UStageProductData>(Object))
		{
			Products.Add(Product);
		}
	}
	Products.Sort([](const UStageProductData& A, const UStageProductData& B) { return A.DisplayName.CompareTo(B.DisplayName) < 0; });

	bCatalogLoaded = true;
	UE_LOG(LogStageCraft, Log, TEXT("Product catalog loaded: %d products."), Products.Num());
	OnProductCatalogLoaded.Broadcast();
}

UStageProductData* UStageEconomySubsystem::FindProduct(FPrimaryAssetId ProductId) const
{
	for (UStageProductData* Product : Products)
	{
		if (Product && Product->GetPrimaryAssetId() == ProductId)
		{
			return Product;
		}
	}
	return nullptr;
}

TArray<UStageProductData*> UStageEconomySubsystem::GetProductsGranting(FGameplayTag Entitlement) const
{
	TArray<UStageProductData*> Result;
	for (UStageProductData* Product : Products)
	{
		if (Product && Product->GrantedEntitlements.HasTagExact(Entitlement))
		{
			Result.Add(Product);
		}
	}
	return Result;
}

// --- Ownership ---

EStageOwnershipState UStageEconomySubsystem::GetEntitlementState(FGameplayTag Entitlement) const
{
	if (!Entitlement.IsValid())
	{
		return EStageOwnershipState::Free;
	}

	const UStageProfileSubsystem* Profile = GetProfile();
	if (Profile && Profile->HasEntitlement(Entitlement))
	{
		return EStageOwnershipState::Owned;
	}

	// Purchasable = for sale to this player now. Funds are not part of it: a player short of credits
	// still sees the price, and RequestPurchase reports InsufficientFunds.
	for (const UStageProductData* Product : GetProductsGranting(Entitlement))
	{
		const FStageEconomyResultInfo Check = ValidatePurchase(Product, /*bIgnorePending*/ true);
		if (Check.IsSuccess() || Check.Code == EStageEconomyResult::InsufficientFunds)
		{
			return EStageOwnershipState::Purchasable;
		}
	}
	return EStageOwnershipState::Locked;
}

EStageOwnershipState UStageEconomySubsystem::GetItemState(const UBaseItemData* Item) const
{
	return Item ? GetEntitlementState(Item->RequiredEntitlement) : EStageOwnershipState::Locked;
}

EStageOwnershipState UStageEconomySubsystem::GetParameterState(const UBaseItemData* Item, FGameplayTag ParameterId) const
{
	const FGameplayTag* Required = Item ? Item->ParameterEntitlements.Find(ParameterId) : nullptr;
	return Required ? GetEntitlementState(*Required) : EStageOwnershipState::Free;
}

// --- Guarded actions ---

FStageEconomyResultInfo UStageEconomySubsystem::ValidateItemUse(const UBaseItemData* Item) const
{
	if (!Item)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoItem", "No item."));
	}
	if (!IsUsable(GetItemState(Item)))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::Locked,
			FText::Format(LOCTEXT("ItemLocked", "{0} is locked. Unlock it in the shop."), StageEconomy::ItemName(Item)));
	}
	return FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo UStageEconomySubsystem::ValidateParameterChange(const UObject* Target, FGameplayTag ParameterId, const FStageParameterValue& Value) const
{
	if (!Target || !ParameterId.IsValid())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoTarget", "Nothing to edit."));
	}

	// Only catalog items carry entitlement data; other parameter objects are not economy-managed.
	const AModularBaseActor* Actor = Cast<AModularBaseActor>(Target);
	if (!Actor)
	{
		return FStageEconomyResultInfo::Ok();
	}

	const UBaseItemData* Item = Actor->GetItemData();
	if (!IsUsable(GetParameterState(Item, ParameterId)))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::Locked,
			FText::Format(LOCTEXT("ParameterLocked", "{0} on {1} is locked. Unlock it in the shop."),
				FText::FromName(ParameterId.GetTagName()), StageEconomy::ItemName(Item)));
	}

	// Switching the Type is "using" the target item, so it is held to the same rule as placing it.
	if (ParameterId == StageCraftTags::Param_Info_Type && Value.Type == EStageParameterType::Enum)
	{
		TArray<UBaseItemData*> Options;
		Actor->GetSwappableItems(Options);
		if (Options.IsValidIndex(Value.Integer) && Options[Value.Integer] != Item)
		{
			return ValidateItemUse(Options[Value.Integer]);
		}
	}
	return FStageEconomyResultInfo::Ok();
}

// --- Purchases ---

FStageEconomyResultInfo UStageEconomySubsystem::ValidatePurchase(const UStageProductData* Product, bool bIgnorePending) const
{
	if (!Product)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoProduct", "No product."));
	}
	if (!Products.Contains(Product))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::UnknownProduct, LOCTEXT("UnknownProduct", "This product is not sold in the shop."));
	}

	const UStageProfileSubsystem* Profile = GetProfile();
	if (!Profile)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoProfile", "No player profile."));
	}
	if (!Profile->IsIntegrityValid())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::IntegrityViolation,
			LOCTEXT("Integrity", "Your profile could not be verified, so purchases are disabled."));
	}
	if (!bIgnorePending && PendingPurchases.Contains(Product->GetPrimaryAssetId()))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::PurchasePending, LOCTEXT("Pending", "This purchase is already in progress."));
	}
	if (Product->GrantedEntitlements.IsEmpty())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("GrantsNothing", "This product grants nothing."));
	}
	if (Profile->GetEntitlements().HasAllExact(Product->GrantedEntitlements))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::AlreadyOwned, LOCTEXT("Owned", "You already own this."));
	}

	for (const UStageUnlockCondition* Condition : Product->Requirements)
	{
		FText Reason;
		if (!Condition || !Condition->IsMet(Profile, Reason))
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::RequirementsNotMet,
				Reason.IsEmpty() ? LOCTEXT("RequirementUnmet", "A requirement is not met.") : Reason);
		}
	}

	TMap<FGameplayTag, int64> Totals;
	if (!StageEconomy::SumPrice(Product->Price, Totals))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("BadPrice", "This product has an invalid price."));
	}
	for (const TPair<FGameplayTag, int64>& Cost : Totals)
	{
		const int64 Balance = Profile->GetBalance(Cost.Key);
		if (Balance < Cost.Value)
		{
			return FStageEconomyResultInfo::Fail(EStageEconomyResult::InsufficientFunds,
				FText::Format(LOCTEXT("Funds", "Needs {0} {1}; you have {2}."),
					FText::AsNumber(Cost.Value), FText::FromName(Cost.Key.GetTagName()), FText::AsNumber(Balance)));
		}
	}
	return FStageEconomyResultInfo::Ok();
}

FStageEconomyResultInfo UStageEconomySubsystem::CanPurchase(const UStageProductData* Product) const
{
	return ValidatePurchase(Product, /*bIgnorePending*/ false);
}

bool UStageEconomySubsystem::IsPurchasePending(const UStageProductData* Product) const
{
	return Product && PendingPurchases.Contains(Product->GetPrimaryAssetId());
}

FStageEconomyResultInfo UStageEconomySubsystem::RequestPurchase(UStageProductData* Product)
{
	const FStageEconomyResultInfo Validation = CanPurchase(Product);
	if (!Validation.IsSuccess())
	{
		UE_LOG(LogStageCraft, Log, TEXT("Purchase of %s refused: %s"), *GetNameSafe(Product), *Validation.Message.ToString());
		return Validation;
	}

	FStagePurchaseRequest Request;
	Request.TransactionId = FGuid::NewGuid();
	Request.ProductId = Product->GetPrimaryAssetId();
	Request.Price = Product->Price;

	PendingPurchases.Add(Request.ProductId, Request.TransactionId);
	UE_LOG(LogStageCraft, Log, TEXT("Purchase %s of %s sent to %s."), *Request.TransactionId.ToString(), *Request.ProductId.ToString(), *GetNameSafe(Backend));

	// May complete synchronously (local backend) or much later (server); both paths are the same.
	StartBackendTimer(Request.TransactionId);
	Backend->ProcessPurchase(Request, FOnStagePurchaseProcessed::CreateUObject(this, &ThisClass::HandlePurchaseProcessed,
		Request.TransactionId, TWeakObjectPtr<UStageProductData>(Product)));
	return FStageEconomyResultInfo::Ok();
}

void UStageEconomySubsystem::HandlePurchaseProcessed(bool bApproved, const FText& Reason, FGuid TransactionId, TWeakObjectPtr<UStageProductData> WeakProduct)
{
	UStageProductData* Product = WeakProduct.Get();
	const FGuid* Pending = Product ? PendingPurchases.Find(Product->GetPrimaryAssetId()) : nullptr;
	const bool bOnTime = Pending && *Pending == TransactionId;
	const bool bLate = !bOnTime && UnresolvedProducts.Contains(TransactionId);
	if (!bOnTime && !bLate)
	{
		// Stale or duplicate answer (a backend must answer once; this keeps a second answer harmless).
		UE_LOG(LogStageCraft, Warning, TEXT("Ignoring backend answer for unknown transaction %s."), *TransactionId.ToString());
		return;
	}
	StopBackendTimer(TransactionId);
	UnresolvedProducts.Remove(TransactionId);
	if (bOnTime)
	{
		PendingPurchases.Remove(Product->GetPrimaryAssetId());
	}
	if (bLate && !bApproved)
	{
		// The requester was already told BackendTimeout and nothing was applied: a late refusal changes nothing.
		UE_LOG(LogStageCraft, Log, TEXT("Purchase %s: late refusal after the timeout; nothing to undo."), *TransactionId.ToString());
		return;
	}
	if (!Product)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Purchase %s: approved, but the product is no longer loaded; left for the server to reconcile."), *TransactionId.ToString());
		return;
	}

	FStageEconomyResultInfo Result;
	if (!bApproved)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::BackendRejected,
			Reason.IsEmpty() ? LOCTEXT("Declined", "The purchase was declined.") : Reason);
	}
	else
	{
		Result = CommitProductPurchase(TransactionId, *Product);
		if (bLate && !Result.IsSuccess())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Purchase %s: late approval could not be applied (%s); left for the server to reconcile."), *TransactionId.ToString(), *Result.Message.ToString());
		}
	}

	UE_LOG(LogStageCraft, Log, TEXT("Purchase %s of %s: %s%s %s"), *TransactionId.ToString(), *Product->GetPrimaryAssetId().ToString(),
		Result.IsSuccess() ? TEXT("completed") : TEXT("failed"), bLate ? TEXT(" (late answer)") : TEXT(""), *Result.Message.ToString());
	OnPurchaseCompleted.Broadcast(Product, Result);
}

FStageEconomyResultInfo UStageEconomySubsystem::CommitProductPurchase(const FGuid& TransactionId, UStageProductData& Product)
{
	// The world may have moved on while the backend was busy (another purchase spent the credits, a late answer for a product
	// bought again since): re-validate before anything is applied.
	FStageEconomyResultInfo Result = ValidatePurchase(&Product, /*bIgnorePending*/ true);
	if (!Result.IsSuccess())
	{
		return Result;
	}

	TMap<FGameplayTag, int64> Totals;
	StageEconomy::SumPrice(Product.Price, Totals);

	FStagePurchaseRecord Record;
	Record.TransactionId = TransactionId;
	Record.ProductId = Product.GetPrimaryAssetId();
	Record.Timestamp = FDateTime::UtcNow();
	Record.EntitlementsGranted = Product.GrantedEntitlements;
	for (const TPair<FGameplayTag, int64>& Cost : Totals)
	{
		FStageCurrencyAmount& Paid = Record.AmountPaid.AddDefaulted_GetRef();
		Paid.Currency = Cost.Key;
		Paid.Amount = Cost.Value;
	}

	UStageProfileSubsystem* Profile = GetProfile();
	if (!Profile || !Profile->CommitPurchase(Record))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InsufficientFunds, LOCTEXT("CommitFailed", "The purchase could not be applied."));
	}
	return Result;
}

// --- Community marketplace listings ---

bool UStageEconomySubsystem::CanSettleRealMoney() const
{
	return Backend && Backend->CanSettleRealMoney();
}

bool UStageEconomySubsystem::IsListingPurchasePending(FGuid ListingId) const
{
	return ListingId.IsValid() && PendingPurchases.Contains(StageMarketplaceRules::MakeListingProductId(ListingId));
}

FStageEconomyResultInfo UStageEconomySubsystem::ValidateListingPurchase(const FStageMarketplaceListing* Offer, bool bIgnorePending) const
{
	const UStageProfileSubsystem* Profile = GetProfile();

	FStageListingPurchaseContext Context;
	Context.Listing = Offer;
	Context.Profile = Profile ? &Profile->GetProfile() : nullptr;
	Context.bIntegrityValid = Profile && Profile->IsIntegrityValid();
	Context.bPending = !bIgnorePending && Offer && IsListingPurchasePending(Offer->ListingId);
	Context.bCanSettleRealMoney = CanSettleRealMoney();
	return StageMarketplaceRules::ValidateListingPurchase(Context);
}

FStageEconomyResultInfo UStageEconomySubsystem::CanPurchaseListing(FGuid ListingId) const
{
	// The offer comes from the marketplace (what the backend returned), never from the caller: deny when it is unknown.
	const UStageMarketplaceSubsystem* Marketplace = GetGameInstance()->GetSubsystem<UStageMarketplaceSubsystem>();
	return ValidateListingPurchase(Marketplace ? Marketplace->FindListing(ListingId) : nullptr, /*bIgnorePending*/ false);
}

FStageEconomyResultInfo UStageEconomySubsystem::RequestListingPurchase(FGuid ListingId)
{
	const UStageMarketplaceSubsystem* Marketplace = GetGameInstance()->GetSubsystem<UStageMarketplaceSubsystem>();
	const FStageMarketplaceListing* Listing = Marketplace ? Marketplace->FindListing(ListingId) : nullptr;
	const FStageEconomyResultInfo Validation = ValidateListingPurchase(Listing, /*bIgnorePending*/ false);
	if (!Validation.IsSuccess())
	{
		UE_LOG(LogStageCraft, Log, TEXT("Purchase of listing %s refused: %s"), *ListingId.ToString(), *Validation.Message.ToString());
		return Validation;
	}

	FStagePurchaseRequest Request;
	Request.TransactionId = FGuid::NewGuid();
	Request.ProductId = StageMarketplaceRules::MakeListingProductId(ListingId);
	FStageCurrencyAmount& Credits = Request.Price.AddDefaulted_GetRef();
	Credits.Currency = StageCraftTags::Currency_Credits;
	Credits.Amount = Listing->Price.Credits;

	PendingPurchases.Add(Request.ProductId, Request.TransactionId);
	PendingListingOffers.Add(Request.TransactionId, *Listing);
	UE_LOG(LogStageCraft, Log, TEXT("Purchase %s of listing %s (%lld credits) sent to %s."), *Request.TransactionId.ToString(), *ListingId.ToString(),
		Listing->Price.Credits, *GetNameSafe(Backend));

	// The local backend answers inside this call; a server answers later. Both paths are the same.
	StartBackendTimer(Request.TransactionId);
	Backend->ProcessPurchase(Request, FOnStagePurchaseProcessed::CreateUObject(this, &ThisClass::HandleListingPurchaseProcessed, Request.TransactionId));
	return FStageEconomyResultInfo::Ok();
}

void UStageEconomySubsystem::HandleListingPurchaseProcessed(bool bApproved, const FText& Reason, FGuid TransactionId)
{
	FStageMarketplaceListing Offer;
	const bool bOnTime = PendingListingOffers.RemoveAndCopyValue(TransactionId, Offer);
	const bool bLate = !bOnTime && UnresolvedListingOffers.RemoveAndCopyValue(TransactionId, Offer);
	if (!bOnTime && !bLate)
	{
		// Stale or duplicate answer (a backend must answer once; this keeps a second answer harmless).
		UE_LOG(LogStageCraft, Warning, TEXT("Ignoring backend answer for unknown listing transaction %s."), *TransactionId.ToString());
		return;
	}
	StopBackendTimer(TransactionId);
	if (bOnTime)
	{
		PendingPurchases.Remove(StageMarketplaceRules::MakeListingProductId(Offer.ListingId));
	}
	if (bLate && !bApproved)
	{
		// The requester was already told BackendTimeout and nothing was applied: a late refusal changes nothing.
		UE_LOG(LogStageCraft, Log, TEXT("Purchase %s of listing %s: late refusal after the timeout; nothing to undo."), *TransactionId.ToString(), *Offer.ListingId.ToString());
		return;
	}

	FStageEconomyResultInfo Result;
	if (!bApproved)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::BackendRejected,
			Reason.IsEmpty() ? LOCTEXT("ListingDeclined", "The purchase was declined.") : Reason);
	}
	else
	{
		Result = CommitListingPurchase(TransactionId, Offer);
		if (bLate && !Result.IsSuccess())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Purchase %s of listing %s: late approval could not be applied (%s); left for the server to reconcile."),
				*TransactionId.ToString(), *Offer.ListingId.ToString(), *Result.Message.ToString());
		}
	}

	UE_LOG(LogStageCraft, Log, TEXT("Purchase %s of listing %s: %s%s %s"), *TransactionId.ToString(), *Offer.ListingId.ToString(),
		Result.IsSuccess() ? TEXT("completed") : TEXT("failed"), bLate ? TEXT(" (late answer)") : TEXT(""), *Result.Message.ToString());
	OnListingPurchaseCompleted.Broadcast(Offer.ListingId, Result);
}

FStageEconomyResultInfo UStageEconomySubsystem::CommitListingPurchase(const FGuid& TransactionId, const FStageMarketplaceListing& Offer)
{
	// Credits may have been spent elsewhere while the backend was busy, or the listing bought again after a timeout.
	FStageEconomyResultInfo Result = ValidateListingPurchase(&Offer, /*bIgnorePending*/ true);
	if (!Result.IsSuccess())
	{
		return Result;
	}

	FStagePurchaseRecord Record;
	Record.TransactionId = TransactionId;
	Record.ProductId = StageMarketplaceRules::MakeListingProductId(Offer.ListingId);
	Record.ListingId = Offer.ListingId;
	Record.Timestamp = FDateTime::UtcNow();
	FStageCurrencyAmount& Paid = Record.AmountPaid.AddDefaulted_GetRef();
	Paid.Currency = StageCraftTags::Currency_Credits;
	Paid.Amount = Offer.Price.Credits;

	UStageProfileSubsystem* Profile = GetProfile();
	if (!Profile || !Profile->CommitPurchase(Record))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InsufficientFunds, LOCTEXT("ListingCommitFailed", "The purchase could not be applied."));
	}
	return Result;
}

bool UStageEconomySubsystem::CanSellCredits() const
{
	return Backend && Backend->CanSellCredits();
}

FStageEconomyResultInfo UStageEconomySubsystem::RequestCreditTopUp(FGameplayTag Currency, int64 Amount)
{
	const UStageProfileSubsystem* Profile = GetProfile();
	FStageTopUpContext Context;
	Context.Profile = Profile && Backend ? &Profile->GetProfile() : nullptr;
	Context.bIntegrityValid = Profile && Profile->IsIntegrityValid();
	Context.bPending = IsCreditTopUpPending();
	Context.bCanSellCredits = CanSellCredits();
	Context.Currency = Currency;
	Context.Amount = Amount;
	Context.MaxAmount = MaxTopUpAmount;
	const FStageEconomyResultInfo Validation = StageTopUpRules::Validate(Context);
	if (!Validation.IsSuccess())
	{
		UE_LOG(LogStageCraft, Log, TEXT("Top-up of %lld %s refused: %s"), Amount, *Currency.ToString(), *Validation.Message.ToString());
		return Validation;
	}

	PendingTopUp.TransactionId = FGuid::NewGuid();
	PendingTopUp.Currency = Currency;
	PendingTopUp.Amount = Amount;
	UE_LOG(LogStageCraft, Log, TEXT("Top-up %s of %lld %s sent to %s."), *PendingTopUp.TransactionId.ToString(), Amount, *Currency.ToString(), *GetNameSafe(Backend));
	// Copied: the backend may answer inside the call, which resets PendingTopUp.
	const FStageTopUpRequest Request = PendingTopUp;
	StartBackendTimer(Request.TransactionId);
	Backend->ProcessCreditTopUp(Request, FOnStageTopUpProcessed::CreateUObject(this, &ThisClass::HandleCreditTopUpProcessed, Request.TransactionId));
	return FStageEconomyResultInfo::Ok();
}

void UStageEconomySubsystem::HandleCreditTopUpProcessed(int64 GrantedAmount, const FText& Reason, FGuid TransactionId)
{
	FStageTopUpRequest Request;
	const bool bOnTime = PendingTopUp.TransactionId.IsValid() && PendingTopUp.TransactionId == TransactionId;
	const bool bLate = !bOnTime && UnresolvedTopUps.RemoveAndCopyValue(TransactionId, Request);
	if (!bOnTime && !bLate)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Ignoring backend answer for unknown top-up %s."), *TransactionId.ToString());
		return;
	}
	StopBackendTimer(TransactionId);
	if (bOnTime)
	{
		Request = PendingTopUp;
		PendingTopUp = FStageTopUpRequest();
	}
	if (bLate && GrantedAmount <= 0)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Top-up %s: late refusal after the timeout; nothing to undo."), *TransactionId.ToString());
		return;
	}

	const FStageEconomyResultInfo Result = ApplyTopUp(Request, GrantedAmount, Reason);
	const int64 Applied = Result.IsSuccess() ? GrantedAmount : 0;
	if (bLate && !Result.IsSuccess())
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Top-up %s: late grant could not be applied (%s); left for the server to reconcile."), *TransactionId.ToString(), *Result.Message.ToString());
	}
	UE_LOG(LogStageCraft, Log, TEXT("Top-up %s: %s%s (%lld of %lld) %s"), *TransactionId.ToString(), Result.IsSuccess() ? TEXT("completed") : TEXT("failed"),
		bLate ? TEXT(" (late answer)") : TEXT(""), Applied, Request.Amount, *Result.Message.ToString());
	OnCreditTopUpCompleted.Broadcast(Request.Currency, Applied, Result);
}

FStageEconomyResultInfo UStageEconomySubsystem::ApplyTopUp(const FStageTopUpRequest& Request, int64 GrantedAmount, const FText& Reason)
{
	FStageEconomyResultInfo Result = FStageEconomyResultInfo::Ok();
	if (GrantedAmount <= 0)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::BackendRejected, Reason.IsEmpty() ? LOCTEXT("TopUpDeclined", "The credit purchase was declined.") : Reason);
	}
	else if (GrantedAmount > Request.Amount)
	{
		// More than was asked for is not a valid answer to this request.
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::BackendRejected, LOCTEXT("TopUpMismatch", "The store's answer did not match the request."));
	}
	else
	{
		UStageProfileSubsystem* Profile = GetProfile();
		if (!Profile || !Profile->IsIntegrityValid())
		{
			Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::IntegrityViolation, LOCTEXT("TopUpFrozenLate", "The credits could not be added: the profile is frozen."));
		}
		else if (!Profile->AdjustBalance(Request.Currency, GrantedAmount))
		{
			Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("TopUpApply", "The credits could not be added to the balance."));
		}
	}
	return Result;
}

bool UStageEconomySubsystem::DevGrantCurrency(FGameplayTag Currency, int64 Amount)
{
#if !UE_BUILD_SHIPPING
	UStageProfileSubsystem* Profile = GetProfile();
	return Profile && Profile->IsIntegrityValid() && Profile->AdjustBalance(Currency, Amount);
#else
	return false;
#endif
}

#undef LOCTEXT_NAMESPACE
