// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageMarketplaceRules.h"

#include "Scene/StageSceneStore.h"

#define LOCTEXT_NAMESPACE "StageCraftMarketplace"

namespace StageMarketplaceRulesImpl
{
	const TCHAR* FallbackSceneName = TEXT("Marketplace Stage");
	/** Leaves room for " (999)" within the 64-character scene name limit. */
	constexpr int32 MaxImportBaseLength = 56;
	constexpr int32 MaxImportSuffix = 999;

	bool ContainsWord(const FStageMarketplaceListing& Listing, const FString& Word)
	{
		return Listing.Title.Contains(Word, ESearchCase::IgnoreCase) || Listing.Description.Contains(Word, ESearchCase::IgnoreCase)
			|| Listing.AuthorName.Contains(Word, ESearchCase::IgnoreCase)
			|| Listing.Keywords.ContainsByPredicate([&Word](const FString& Keyword) { return Keyword.Contains(Word, ESearchCase::IgnoreCase); });
	}

	/** Credits first, then real money; within each, by amount. */
	bool PriceLess(const FStageMarketplacePrice& A, const FStageMarketplacePrice& B)
	{
		if (A.HasRealMoneyPrice() != B.HasRealMoneyPrice())
		{
			return !A.HasRealMoneyPrice();
		}
		return A.HasRealMoneyPrice() ? A.RealMoneyMinorUnits < B.RealMoneyMinorUnits : A.Credits < B.Credits;
	}

	bool TieBreakLess(const FStageMarketplaceListing& A, const FStageMarketplaceListing& B)
	{
		const int32 Compare = A.Title.Compare(B.Title, ESearchCase::IgnoreCase);
		return Compare != 0 ? Compare < 0 : A.ListingId < B.ListingId;
	}
}

FPrimaryAssetId StageMarketplaceRules::MakeListingProductId(const FGuid& ListingId)
{
	return FPrimaryAssetId(FPrimaryAssetType(ListingProductType), FName(*ListingId.ToString(EGuidFormats::Digits)));
}

FStageMarketplaceQuery StageMarketplaceRules::SanitizeQuery(const FStageMarketplaceQuery& Query)
{
	FStageMarketplaceQuery Clean = Query;
	Clean.SearchText = Query.SearchText.Left(FStageMarketplaceQuery::MaxSearchTextLength).TrimStartAndEnd();
	Clean.Offset = FMath::Max(Query.Offset, 0);
	Clean.MaxCredits = FMath::Max<int64>(Query.MaxCredits, 0);
	Clean.Limit = FMath::Clamp(Query.Limit, 1, FStageMarketplaceQuery::MaxPageSize);
	return Clean;
}

bool StageMarketplaceRules::Matches(const FStageMarketplaceListing& Listing, const FStageMarketplaceQuery& Query)
{
	if (!Query.Kinds.IsEmpty() && !Query.Kinds.Contains(Listing.Kind))
	{
		return false;
	}
	if (Query.Category.IsValid() && Listing.Category != Query.Category)
	{
		return false;
	}
	if ((Query.PriceFilter == EStageMarketplacePriceFilter::Free && !Listing.Price.IsFree())
		|| (Query.PriceFilter == EStageMarketplacePriceFilter::Paid && Listing.Price.IsFree()))
	{
		return false;
	}
	if (Query.MaxCredits > 0 && (Listing.Price.HasRealMoneyPrice() || Listing.Price.Credits > Query.MaxCredits))
	{
		return false;
	}

	TArray<FString> Words;
	Query.SearchText.ParseIntoArrayWS(Words);
	for (const FString& Word : Words)
	{
		if (!StageMarketplaceRulesImpl::ContainsWord(Listing, Word))
		{
			return false;
		}
	}
	return true;
}

FStageMarketplaceQueryResult StageMarketplaceRules::Apply(TArray<FStageMarketplaceListing> Listings, const FStageMarketplaceQuery& InQuery)
{
	using namespace StageMarketplaceRulesImpl;

	const FStageMarketplaceQuery Query = SanitizeQuery(InQuery);
	Listings.RemoveAll([&Query](const FStageMarketplaceListing& Listing) { return !Matches(Listing, Query); });

	Listings.Sort([Sort = Query.Sort](const FStageMarketplaceListing& A, const FStageMarketplaceListing& B)
	{
		switch (Sort)
		{
		case EStageMarketplaceSort::Popular:
			if (A.Downloads != B.Downloads)
			{
				return A.Downloads > B.Downloads;
			}
			break;
		case EStageMarketplaceSort::Newest:
			if (A.PublishedAtUtc != B.PublishedAtUtc)
			{
				return A.PublishedAtUtc > B.PublishedAtUtc;
			}
			break;
		case EStageMarketplaceSort::PriceLowToHigh:
			if (PriceLess(A.Price, B.Price) || PriceLess(B.Price, A.Price))
			{
				return PriceLess(A.Price, B.Price);
			}
			break;
		case EStageMarketplaceSort::PriceHighToLow:
			if (PriceLess(A.Price, B.Price) || PriceLess(B.Price, A.Price))
			{
				return PriceLess(B.Price, A.Price);
			}
			break;
		case EStageMarketplaceSort::Title:
			break;
		}
		return TieBreakLess(A, B);
	});

	FStageMarketplaceQueryResult Result;
	Result.TotalMatches = Listings.Num();
	const int32 First = FMath::Min(Query.Offset, Listings.Num());
	const int32 Count = FMath::Min(Query.Limit, Listings.Num() - First);
	Result.Listings.Append(Listings.GetData() + First, Count);
	return Result;
}

bool StageMarketplaceRules::OwnsListing(const FStagePlayerProfile& Profile, const FGuid& ListingId)
{
	return ListingId.IsValid() && Profile.OwnedListings.Contains(ListingId);
}

EStageOwnershipState StageMarketplaceRules::GetOwnership(const FStageMarketplaceListing& Listing, const FStagePlayerProfile& Profile, bool bIntegrityValid,
	bool bCanSettleRealMoney)
{
	if (Listing.Price.IsFree())
	{
		return EStageOwnershipState::Free;
	}
	if (OwnsListing(Profile, Listing.ListingId))
	{
		return EStageOwnershipState::Owned;
	}
	if (!bIntegrityValid || (Listing.Price.HasRealMoneyPrice() && !bCanSettleRealMoney))
	{
		return EStageOwnershipState::Locked;
	}
	return EStageOwnershipState::Purchasable;
}

FStageEconomyResultInfo StageMarketplaceRules::ValidateListingPurchase(const FStageListingPurchaseContext& Context)
{
	if (!Context.Profile)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("NoProfile", "No player profile."));
	}
	if (!Context.bIntegrityValid)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::IntegrityViolation,
			LOCTEXT("Integrity", "Your profile could not be verified, so purchases are disabled."));
	}
	const FStageMarketplaceListing* Listing = Context.Listing;
	if (!Listing || !Listing->ListingId.IsValid())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::UnknownProduct, LOCTEXT("UnknownListing", "This listing is not in the marketplace."));
	}
	const FStageMarketplacePrice& Price = Listing->Price;
	if (Price.Credits < 0 || Price.RealMoneyMinorUnits < 0)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("BadPrice", "This listing has an invalid price."));
	}
	if (Price.IsFree())
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InvalidRequest, LOCTEXT("FreeListing", "This listing is free; there is nothing to buy."));
	}
	if (OwnsListing(*Context.Profile, Listing->ListingId))
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::AlreadyOwned, LOCTEXT("Owned", "You already own this."));
	}
	if (Context.bPending)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::PurchasePending, LOCTEXT("Pending", "This purchase is already in progress."));
	}
	if (Price.HasRealMoneyPrice() && !Context.bCanSettleRealMoney)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::ServerRequired,
			LOCTEXT("ServerRequired", "This listing is sold for real money, which needs an online store. It cannot be bought offline."));
	}

	const int64* Balance = Context.Profile->Balances.Find(StageCraftTags::Currency_Credits);
	const int64 Credits = Balance ? *Balance : 0;
	if (Credits < Price.Credits)
	{
		return FStageEconomyResultInfo::Fail(EStageEconomyResult::InsufficientFunds,
			FText::Format(LOCTEXT("Funds", "Needs {0} credits; you have {1}."), FText::AsNumber(Price.Credits), FText::AsNumber(Credits)));
	}
	return FStageEconomyResultInfo::Ok();
}

FString StageMarketplaceRules::MakeImportSceneName(const FString& Title, TFunctionRef<bool(const FString&)> Exists)
{
	using namespace StageMarketplaceRulesImpl;

	FString Base;
	Base.Reserve(Title.Len());
	for (const TCHAR Char : Title)
	{
		const bool bAllowed = FChar::IsAlnum(Char) || Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('(') || Char == TEXT(')');
		// Runs of spaces (from removed characters) collapse to one.
		if (bAllowed && !(Char == TEXT(' ') && Base.EndsWith(TEXT(" "))))
		{
			Base.AppendChar(Char);
		}
	}
	Base = Base.Left(MaxImportBaseLength).TrimStartAndEnd();
	if (!FStageSceneStore::IsValidSceneName(Base))
	{
		Base = FallbackSceneName;
	}

	if (!Exists(Base))
	{
		return Base;
	}
	for (int32 Suffix = 2; Suffix <= MaxImportSuffix; ++Suffix)
	{
		const FString Candidate = FString::Printf(TEXT("%s (%d)"), *Base, Suffix);
		if (!Exists(Candidate))
		{
			return Candidate;
		}
	}
	return FString();
}

EStageExportRights StageMarketplaceRules::ResolveExportRights(const FGuid& ListingId, const FStagePackageManifest* Manifest, const FStagePlayerProfile& Profile)
{
	if (!Manifest)
	{
		return EStageExportRights::NoLibraryCopy;
	}
	if (!ListingId.IsValid() || Manifest->PackageId != ListingId)
	{
		return EStageExportRights::Mismatch;
	}
	if (!Manifest->bAllowExport)
	{
		return EStageExportRights::NotAllowedByPublisher;
	}
	if (!Manifest->Price.IsFree() && !OwnsListing(Profile, ListingId))
	{
		return EStageExportRights::NotOwned;
	}
	return EStageExportRights::Allowed;
}

#undef LOCTEXT_NAMESPACE
