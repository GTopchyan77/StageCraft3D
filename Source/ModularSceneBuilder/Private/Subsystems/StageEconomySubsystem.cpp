// Copyright Epic Games, Inc. All Rights Reserved.

#include "Subsystems/StageEconomySubsystem.h"

#include "Actors/ModularBaseActor.h"
#include "Data/BaseItemData.h"
#include "Economy/StageCommerceBackend.h"
#include "Economy/StageProductData.h"
#include "Economy/StageUnlockCondition.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
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
	PendingPurchases.Reset();
	Products.Reset();
	Backend = nullptr;
	bCatalogLoaded = false;

	Super::Deinitialize();
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
	Backend->ProcessPurchase(Request, FOnStagePurchaseProcessed::CreateUObject(this, &ThisClass::HandlePurchaseProcessed,
		Request.TransactionId, TWeakObjectPtr<UStageProductData>(Product)));
	return FStageEconomyResultInfo::Ok();
}

void UStageEconomySubsystem::HandlePurchaseProcessed(bool bApproved, const FText& Reason, FGuid TransactionId, TWeakObjectPtr<UStageProductData> WeakProduct)
{
	UStageProductData* Product = WeakProduct.Get();
	const FGuid* Pending = Product ? PendingPurchases.Find(Product->GetPrimaryAssetId()) : nullptr;
	if (!Pending || *Pending != TransactionId)
	{
		// Stale or duplicate answer (a backend must answer once; this keeps a second answer harmless).
		UE_LOG(LogStageCraft, Warning, TEXT("Ignoring backend answer for unknown transaction %s."), *TransactionId.ToString());
		return;
	}
	PendingPurchases.Remove(Product->GetPrimaryAssetId());

	FStageEconomyResultInfo Result;
	if (!bApproved)
	{
		Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::BackendRejected,
			Reason.IsEmpty() ? LOCTEXT("Declined", "The purchase was declined.") : Reason);
	}
	else
	{
		// The world may have moved on while the backend was busy (another purchase spent the credits).
		Result = ValidatePurchase(Product, /*bIgnorePending*/ true);
	}

	if (Result.IsSuccess())
	{
		TMap<FGameplayTag, int64> Totals;
		StageEconomy::SumPrice(Product->Price, Totals);

		FStagePurchaseRecord Record;
		Record.TransactionId = TransactionId;
		Record.ProductId = Product->GetPrimaryAssetId();
		Record.Timestamp = FDateTime::UtcNow();
		Record.EntitlementsGranted = Product->GrantedEntitlements;
		for (const TPair<FGameplayTag, int64>& Cost : Totals)
		{
			FStageCurrencyAmount& Paid = Record.AmountPaid.AddDefaulted_GetRef();
			Paid.Currency = Cost.Key;
			Paid.Amount = Cost.Value;
		}

		UStageProfileSubsystem* Profile = GetProfile();
		if (!Profile || !Profile->CommitPurchase(Record))
		{
			Result = FStageEconomyResultInfo::Fail(EStageEconomyResult::InsufficientFunds, LOCTEXT("CommitFailed", "The purchase could not be applied."));
		}
	}

	UE_LOG(LogStageCraft, Log, TEXT("Purchase %s of %s: %s %s"), *TransactionId.ToString(), *Product->GetPrimaryAssetId().ToString(),
		Result.IsSuccess() ? TEXT("completed") : TEXT("failed"), *Result.Message.ToString());
	OnPurchaseCompleted.Broadcast(Product, Result);
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
