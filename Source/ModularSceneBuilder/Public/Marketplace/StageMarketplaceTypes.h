// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "StageMarketplaceTypes.generated.h"

/** Outcome of a marketplace operation (Docs/ADR/0021-community-marketplace.md). One closed enum, so UI branches without parsing text. */
UENUM(BlueprintType)
enum class EStageMarketplaceResult : uint8
{
	Success,
	/** Malformed request: invalid id, title, price, keywords or scene name. */
	InvalidRequest,
	/** No listing, package or saved scene with that id or name. */
	NotFound,
	/** The package is not a .stage file this build can read (bad container, section or manifest). */
	Corrupt,
	/** The package was written by an incompatible container or manifest version. */
	VersionMismatch,
	/** The package, or one of its sections, is over its size limit. */
	TooLarge,
	/** A section's content does not match its hash (damaged in transit or on disk). */
	HashMismatch,
	/** A listing with this id is already published. */
	AlreadyExists,
	/** The scene contains items made from imported models, which exist only on the machine that imported them. */
	NotPortable,
	WriteFailed,
	/** The same operation is still running for this listing (or another publish is running). */
	Busy,
	/** No marketplace backend is available. */
	BackendUnavailable,
	/** The listing must be bought before it can be downloaded. */
	NotOwned,
	/** The purchase was refused by the economy (see the economy result); nothing was downloaded. */
	PurchaseFailed,
	/** A real-money price needs an authoritative server; the local backend cannot sell or publish for money. */
	ServerRequired,
};

/** Result of a marketplace request. Message is user-facing; Code is for logic. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMarketplaceResultInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	EStageMarketplaceResult Code = EStageMarketplaceResult::Success;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FText Message;

	bool IsSuccess() const { return Code == EStageMarketplaceResult::Success; }

	static FStageMarketplaceResultInfo Ok(const FText& InMessage = FText::GetEmpty())
	{
		FStageMarketplaceResultInfo Result;
		Result.Message = InMessage;
		return Result;
	}
	static FStageMarketplaceResultInfo Fail(EStageMarketplaceResult InCode, const FText& InMessage)
	{
		FStageMarketplaceResultInfo Result;
		Result.Code = InCode;
		Result.Message = InMessage;
		return Result;
	}
};

/** What a package is mainly for. Browsing filters on it; every kind holds a complete scene (ADR 0021 §2.3). */
UENUM(BlueprintType)
enum class EStagePackageKind : uint8
{
	StageDesign,
	LightingRig,
	CueList,
};

UENUM(BlueprintType)
enum class EStageMarketplaceSort : uint8
{
	/** Most downloaded first. */
	Popular,
	Newest,
	PriceLowToHigh,
	PriceHighToLow,
	Title,
};

UENUM(BlueprintType)
enum class EStageMarketplacePriceFilter : uint8
{
	Any,
	Free,
	Paid,
};

/**
 * A listing's price. Credits are the in-app currency (StageCraft.Currency.Credits), whole units. A real-money price is carried as
 * data only: it can be neither published nor bought unless the commerce backend is an authoritative server (Economy security 7).
 */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMarketplacePrice
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Marketplace", meta = (ClampMin = "0"))
	int64 Credits = 0;

	/** Minor units (cents). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Marketplace", meta = (ClampMin = "0"))
	int64 RealMoneyMinorUnits = 0;

	/** ISO 4217 code ("USD"); required when RealMoneyMinorUnits is not zero. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Marketplace")
	FString RealMoneyCurrency;

	bool IsFree() const { return Credits == 0 && RealMoneyMinorUnits == 0; }
	bool HasRealMoneyPrice() const { return RealMoneyMinorUnits != 0; }
};

/** One published package as a browser sees it. Built from the package manifest by the backend; read-only for everyone else. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMarketplaceListing
{
	GENERATED_BODY()

	/** The package id; stable across downloads. */
	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FGuid ListingId;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FString Title;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FString Description;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FString AuthorName;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	EStagePackageKind Kind = EStagePackageKind::StageDesign;

	/** A child of StageCraft.Marketplace.Category. */
	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FGameplayTag Category;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	TArray<FString> Keywords;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FStageMarketplacePrice Price;

	/** Popularity: completed downloads, as counted by the backend. */
	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	int64 Downloads = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	FDateTime PublishedAtUtc;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	int32 ItemCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	int32 FixtureCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	int32 CueCount = 0;

	/** Size of the package file. */
	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	int64 PackageBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	bool bHasThumbnail = false;
};

/** What to browse. Empty fields do not filter. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMarketplaceQuery
{
	GENERATED_BODY()

	/** Words that must all appear in the title, description, author or keywords (case-insensitive). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	FString SearchText;

	/** Empty: every kind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	TArray<EStagePackageKind> Kinds;

	/** Invalid: every category. Matching is exact (a category tag has no children). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace", meta = (Categories = "StageCraft.Marketplace.Category"))
	FGameplayTag Category;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	EStageMarketplacePriceFilter PriceFilter = EStageMarketplacePriceFilter::Any;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	EStageMarketplaceSort Sort = EStageMarketplaceSort::Popular;

	/** Zero-based index of the first result of the page. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace", meta = (ClampMin = "0"))
	int32 Offset = 0;

	/** Page size; clamped to 1..MaxPageSize. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace", meta = (ClampMin = "1"))
	int32 Limit = 50;

	static constexpr int32 MaxPageSize = 200;
	static constexpr int32 MaxSearchTextLength = 128;
};

/** One page of query results. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMarketplaceQueryResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	TArray<FStageMarketplaceListing> Listings;

	/** Matches before paging, for "page 2 of 5". */
	UPROPERTY(BlueprintReadOnly, Category = "Marketplace")
	int32 TotalMatches = 0;
};

/** What a creator fills in to publish one of their saved scenes. */
USTRUCT(BlueprintType)
struct MODULARSCENEBUILDER_API FStageMarketplacePublishRequest
{
	GENERATED_BODY()

	/** A scene saved with File > Save; what is on disk is what is published. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	FString SceneName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	FString Title;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	FString Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	EStagePackageKind Kind = EStagePackageKind::StageDesign;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace", meta = (Categories = "StageCraft.Marketplace.Category"))
	FGameplayTag Category;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	TArray<FString> Keywords;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	FStageMarketplacePrice Price;

	/** Optional PNG (checked by signature and size). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Marketplace")
	TArray<uint8> ThumbnailPng;
};

namespace StageCraftTags
{
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Concert);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Festival);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Theatre);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Corporate);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Club);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Broadcast);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Worship);
	MODULARSCENEBUILDER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Marketplace_Category_Other);
}
