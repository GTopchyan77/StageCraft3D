// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageMarketplaceSubsystem.h"

#include "Algo/Count.h"
#include "Async/Async.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "IO/StageAtomicFile.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Marketplace/StagePackage.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Scene/StageSceneStore.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMarketplaceSubsystem)

#define LOCTEXT_NAMESPACE "StageCraftMarketplace"

namespace StageMarketplace
{
	/** User-facing text for each result, so every refusal and failure says what happened. */
	FText Describe(EStageMarketplaceResult Result)
	{
		switch (Result)
		{
		case EStageMarketplaceResult::Success: return LOCTEXT("Result_Success", "Done.");
		case EStageMarketplaceResult::InvalidRequest: return LOCTEXT("Result_InvalidRequest", "The request is not valid.");
		case EStageMarketplaceResult::NotFound: return LOCTEXT("Result_NotFound", "It is no longer in the marketplace.");
		case EStageMarketplaceResult::Corrupt: return LOCTEXT("Result_Corrupt", "The package is damaged or is not a StageCraft package.");
		case EStageMarketplaceResult::VersionMismatch: return LOCTEXT("Result_VersionMismatch", "The package was made by a newer version of StageCraft.");
		case EStageMarketplaceResult::TooLarge: return LOCTEXT("Result_TooLarge", "The package is too large.");
		case EStageMarketplaceResult::HashMismatch: return LOCTEXT("Result_HashMismatch", "The package was damaged in transit or on disk.");
		case EStageMarketplaceResult::AlreadyExists: return LOCTEXT("Result_AlreadyExists", "This package is already published.");
		case EStageMarketplaceResult::NotPortable: return LOCTEXT("Result_NotPortable", "The stage contains imported models, which exist only on this computer.");
		case EStageMarketplaceResult::WriteFailed: return LOCTEXT("Result_WriteFailed", "The file could not be written.");
		case EStageMarketplaceResult::Busy: return LOCTEXT("Result_Busy", "This is already in progress.");
		case EStageMarketplaceResult::BackendUnavailable: return LOCTEXT("Result_BackendUnavailable", "The marketplace is not available.");
		case EStageMarketplaceResult::NotOwned: return LOCTEXT("Result_NotOwned", "Buy this listing first.");
		case EStageMarketplaceResult::PurchaseFailed: return LOCTEXT("Result_PurchaseFailed", "The purchase did not go through.");
		case EStageMarketplaceResult::ServerRequired: return LOCTEXT("Result_ServerRequired", "Real-money sales need an online store; they are not available offline.");
		}
		return FText::GetEmpty();
	}

	FStageMarketplaceResultInfo Info(EStageMarketplaceResult Result)
	{
		return Result == EStageMarketplaceResult::Success ? FStageMarketplaceResultInfo::Ok() : FStageMarketplaceResultInfo::Fail(Result, Describe(Result));
	}

	/** Verifies a fetched package and writes its scene into the scenes folder under a fresh name. Runs on the work pipe. */
	EStageMarketplaceResult ImportPackage(const TArray<uint8>& Bytes, const FGuid& ListingId, FString& OutSceneName)
	{
		FStagePackage Package;
		EStageMarketplaceResult Result = StagePackage::Read(Bytes, Package);
		if (Result != EStageMarketplaceResult::Success)
		{
			return Result;
		}
		// The bytes must be the listing that was bought, not another package served under its id.
		if (Package.Manifest.PackageId != ListingId)
		{
			return EStageMarketplaceResult::Corrupt;
		}

		const FStageSceneStore Scenes(FStageSceneStore::GetDefaultDirectory());
		OutSceneName = StageMarketplaceRules::MakeImportSceneName(Package.Manifest.Title, [&Scenes](const FString& Name) { return Scenes.SceneExists(Name); });
		if (OutSceneName.IsEmpty())
		{
			return EStageMarketplaceResult::WriteFailed;
		}
		Package.Scene.Name = OutSceneName;
		Package.Scene.SavedAtUtc = FDateTime::UtcNow();
		if (Scenes.Save(Package.Scene) != EStageSceneResult::Success)
		{
			return EStageMarketplaceResult::WriteFailed;
		}

		// A copy of what was acquired, for support and re-imports. Losing it loses nothing the user sees.
		const FString LibraryPath = FPaths::Combine(UStageMarketplaceSubsystem::GetLibraryDirectory(), ListingId.ToString(EGuidFormats::Digits) + StagePackage::FileExtension);
		IFileManager::Get().MakeDirectory(*UStageMarketplaceSubsystem::GetLibraryDirectory(), /*Tree*/ true);
		if (!StageAtomicFile::WriteBytes(LibraryPath, TConstArrayView64<uint8>(Bytes.GetData(), Bytes.Num())))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: could not keep a copy of %s at %s."), *ListingId.ToString(), *LibraryPath);
		}
		return EStageMarketplaceResult::Success;
	}

	/** Loads the saved scene, checks it can travel and writes the package. Runs on the work pipe. */
	EStageMarketplaceResult BuildPackage(FStagePackageManifest Manifest, const FString& SceneName, const TSet<FSoftObjectPath>& LightItems,
		TArray<uint8> ThumbnailPng, TArray<uint8>& OutBytes)
	{
		const FStageSceneStore Scenes(FStageSceneStore::GetDefaultDirectory());
		FStagePackage Package;
		FStageSceneParseReport Report;
		switch (Scenes.Load(SceneName, Package.Scene, Report))
		{
		case EStageSceneResult::Success:
			break;
		case EStageSceneResult::NotFound:
		case EStageSceneResult::InvalidName:
			return EStageMarketplaceResult::NotFound;
		case EStageSceneResult::VersionMismatch:
			return EStageMarketplaceResult::VersionMismatch;
		default:
			return EStageMarketplaceResult::Corrupt;
		}
		// What is published must be what the creator sees: a scene the parser had to repair is not published until it is re-saved.
		if (Report.InvalidItems > 0 || Report.RepairedIds > 0 || Report.InvalidCueEntries > 0)
		{
			return EStageMarketplaceResult::Corrupt;
		}
		if (!StagePackage::IsPortable(Package.Scene))
		{
			return EStageMarketplaceResult::NotPortable;
		}

		Manifest.ItemCount = Package.Scene.Items.Num();
		Manifest.CueCount = Package.Scene.Cues.Num();
		Manifest.FixtureCount = Algo::CountIf(Package.Scene.Items, [&LightItems](const FStageItemSnapshot& Item) { return LightItems.Contains(Item.Item.ToSoftObjectPath()); });
		Manifest.RequiredItems = StagePackage::CollectRequiredItems(Package.Scene);
		Package.Manifest = MoveTemp(Manifest);
		Package.ThumbnailPng = MoveTemp(ThumbnailPng);
		return StagePackage::Write(Package, OutBytes);
	}
}

void UStageMarketplaceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// The economy settles paid listings and reports back; the profile says who owns what.
	Collection.InitializeDependency<UStageProfileSubsystem>();
	Collection.InitializeDependency<UStageEconomySubsystem>();
	Super::Initialize(Collection);

	UClass* BackendClass = MarketplaceBackendClass.IsNull() ? nullptr : MarketplaceBackendClass.LoadSynchronous();
	if (!BackendClass || BackendClass->HasAnyClassFlags(CLASS_Abstract) || !BackendClass->IsChildOf<UStageMarketplaceBackend>())
	{
		if (!MarketplaceBackendClass.IsNull())
		{
			UE_LOG(LogStageCraft, Warning, TEXT("MarketplaceBackendClass '%s' is not a concrete UStageMarketplaceBackend; using the local backend."), *MarketplaceBackendClass.ToString());
		}
		BackendClass = UStageLocalMarketplaceBackend::StaticClass();
	}
	Backend = NewObject<UStageMarketplaceBackend>(this, BackendClass);

	if (UStageEconomySubsystem* Economy = GetEconomy())
	{
		Economy->OnListingPurchaseCompleted.AddUniqueDynamic(this, &ThisClass::HandleListingPurchaseCompleted);
	}
}

void UStageMarketplaceSubsystem::Deinitialize()
{
	if (UStageEconomySubsystem* Economy = GetEconomy())
	{
		Economy->OnListingPurchaseCompleted.RemoveDynamic(this, &ThisClass::HandleListingPurchaseCompleted);
	}
	// Imports and package builds are bounded file work; the pipe must be empty before it is destroyed.
	WorkPipe.WaitUntilEmpty();

	OnListingsUpdated.Clear();
	OnAcquireCompleted.Clear();
	OnPublishCompleted.Clear();
	Listings.Reset();
	PendingAcquires.Reset();
	bQueryInFlight = false;
	bPublishing = false;
	Backend = nullptr;

	Super::Deinitialize();
}

UStageEconomySubsystem* UStageMarketplaceSubsystem::GetEconomy() const
{
	return GetGameInstance()->GetSubsystem<UStageEconomySubsystem>();
}

UStageProfileSubsystem* UStageMarketplaceSubsystem::GetProfile() const
{
	return GetGameInstance()->GetSubsystem<UStageProfileSubsystem>();
}

FText UStageMarketplaceSubsystem::GetBackendName() const
{
	return Backend ? Backend->GetDisplayName() : StageMarketplace::Describe(EStageMarketplaceResult::BackendUnavailable);
}

FString UStageMarketplaceSubsystem::GetLibraryDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("StageCraft"), TEXT("Marketplace"), TEXT("Library"));
}

// --- Browsing ---

void UStageMarketplaceSubsystem::RequestQuery(const FStageMarketplaceQuery& Query)
{
	if (!Backend)
	{
		OnListingsUpdated.Broadcast(StageMarketplace::Info(EStageMarketplaceResult::BackendUnavailable));
		return;
	}
	const uint32 Serial = ++QuerySerial;
	bQueryInFlight = true;
	Backend->Query(StageMarketplaceRules::SanitizeQuery(Query), FOnStageMarketplaceQueried::CreateUObject(this, &ThisClass::HandleQueried, Serial));
}

void UStageMarketplaceSubsystem::HandleQueried(EStageMarketplaceResult Result, const FStageMarketplaceQueryResult& Page, uint32 Serial)
{
	if (Serial != QuerySerial)
	{
		return; // Superseded by a newer query; its answer is the one that counts.
	}
	bQueryInFlight = false;
	if (Result == EStageMarketplaceResult::Success)
	{
		Listings = Page.Listings;
		TotalMatches = Page.TotalMatches;
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace query: %s, %d of %d listings."), *UEnum::GetValueAsString(Result), Listings.Num(), TotalMatches);
	OnListingsUpdated.Broadcast(StageMarketplace::Info(Result));
}

const FStageMarketplaceListing* UStageMarketplaceSubsystem::FindListing(const FGuid& ListingId) const
{
	return ListingId.IsValid() ? Listings.FindByPredicate([&ListingId](const FStageMarketplaceListing& Listing) { return Listing.ListingId == ListingId; }) : nullptr;
}

EStageOwnershipState UStageMarketplaceSubsystem::GetListingOwnership(FGuid ListingId) const
{
	const FStageMarketplaceListing* Listing = FindListing(ListingId);
	const UStageProfileSubsystem* Profile = GetProfile();
	const UStageEconomySubsystem* Economy = GetEconomy();
	if (!Listing || !Profile || !Economy)
	{
		return EStageOwnershipState::Locked;
	}
	return StageMarketplaceRules::GetOwnership(*Listing, Profile->GetProfile(), Profile->IsIntegrityValid(), Economy->CanSettleRealMoney());
}

// --- Acquiring ---

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::RequestAcquire(FGuid ListingId)
{
	const FStageMarketplaceListing* Listing = FindListing(ListingId);
	UStageEconomySubsystem* Economy = GetEconomy();
	if (!Backend || !Economy)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::BackendUnavailable);
	}
	if (!Listing)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::NotFound);
	}
	if (PendingAcquires.Contains(ListingId))
	{
		return StageMarketplace::Info(EStageMarketplaceResult::Busy);
	}

	switch (GetListingOwnership(ListingId))
	{
	case EStageOwnershipState::Free:
	case EStageOwnershipState::Owned:
		PendingAcquires.Add(ListingId);
		StartDownload(ListingId);
		return FStageMarketplaceResultInfo::Ok();

	case EStageOwnershipState::Purchasable:
	case EStageOwnershipState::Locked:
	{
		// Registered first: the offline commerce backend completes the purchase inside RequestListingPurchase.
		PendingAcquires.Add(ListingId);
		const FStageEconomyResultInfo Purchase = Economy->RequestListingPurchase(ListingId);
		if (!Purchase.IsSuccess())
		{
			PendingAcquires.Remove(ListingId);
			const EStageMarketplaceResult Code = Purchase.Code == EStageEconomyResult::ServerRequired ? EStageMarketplaceResult::ServerRequired : EStageMarketplaceResult::PurchaseFailed;
			return FStageMarketplaceResultInfo::Fail(Code, Purchase.Message);
		}
		return FStageMarketplaceResultInfo::Ok();
	}
	}
	return StageMarketplace::Info(EStageMarketplaceResult::InvalidRequest);
}

void UStageMarketplaceSubsystem::HandleListingPurchaseCompleted(FGuid ListingId, FStageEconomyResultInfo Result)
{
	if (!PendingAcquires.Contains(ListingId))
	{
		return; // Bought elsewhere (e.g. directly through the economy); nothing to download here.
	}
	if (!Result.IsSuccess())
	{
		FinishAcquire(ListingId, FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::PurchaseFailed, Result.Message), FString());
		return;
	}
	StartDownload(ListingId);
}

void UStageMarketplaceSubsystem::StartDownload(const FGuid& ListingId)
{
	if (!Backend)
	{
		FinishAcquire(ListingId, StageMarketplace::Info(EStageMarketplaceResult::BackendUnavailable), FString());
		return;
	}
	Backend->FetchPackage(ListingId, EStageMarketplaceFetchPurpose::Acquire, FOnStageMarketplacePackageFetched::CreateUObject(this, &ThisClass::HandlePackageFetched, ListingId));
}

void UStageMarketplaceSubsystem::HandlePackageFetched(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes, FGuid ListingId)
{
	if (Result != EStageMarketplaceResult::Success || !Bytes.IsValid())
	{
		FinishAcquire(ListingId, StageMarketplace::Info(Result == EStageMarketplaceResult::Success ? EStageMarketplaceResult::Corrupt : Result), FString());
		return;
	}

	WorkPipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), Bytes, ListingId]()
	{
		FString SceneName;
		const EStageMarketplaceResult ImportResult = StageMarketplace::ImportPackage(*Bytes, ListingId, SceneName);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ImportResult, ListingId, SceneName = MoveTemp(SceneName)]()
		{
			if (ThisClass* This = WeakThis.Get())
			{
				This->FinishAcquire(ListingId, StageMarketplace::Info(ImportResult), SceneName);
			}
		});
	});
}

void UStageMarketplaceSubsystem::FinishAcquire(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FString& SceneName)
{
	PendingAcquires.Remove(ListingId);
	FStageMarketplaceResultInfo Final = Result;
	if (Final.IsSuccess())
	{
		Final.Message = FText::Format(LOCTEXT("Imported", "Added to your scenes as \"{0}\"."), FText::FromString(SceneName));
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace acquire %s: %s %s"), *ListingId.ToString(), *UEnum::GetValueAsString(Final.Code), *Final.Message.ToString());
	OnAcquireCompleted.Broadcast(ListingId, Final, Final.IsSuccess() ? SceneName : FString());
}

// --- Publishing ---

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::RequestPublish(const FStageMarketplacePublishRequest& Request)
{
	const UStageEconomySubsystem* Economy = GetEconomy();
	const UStageProfileSubsystem* Profile = GetProfile();
	const UStageItemSubsystem* Items = GetGameInstance()->GetSubsystem<UStageItemSubsystem>();
	if (!Backend || !Economy || !Profile || !Items)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::BackendUnavailable);
	}
	if (bPublishing)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::Busy);
	}
	if (!Items->IsCatalogLoaded())
	{
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::Busy, LOCTEXT("CatalogLoading", "The item library is still loading; try again in a moment."));
	}
	if (!FStageSceneStore::IsValidSceneName(Request.SceneName))
	{
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::InvalidRequest, LOCTEXT("SceneName", "Choose a saved scene to publish."));
	}
	// Deny by default: a real-money price is published only where a server can sell it.
	if (Request.Price.HasRealMoneyPrice() && !Economy->CanSettleRealMoney())
	{
		return StageMarketplace::Info(EStageMarketplaceResult::ServerRequired);
	}
	if (!StagePackage::IsValidThumbnail(Request.ThumbnailPng))
	{
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::InvalidRequest, LOCTEXT("Thumbnail", "The thumbnail must be a PNG of at most 2 MB."));
	}

	FStagePackageManifest Manifest;
	Manifest.PackageId = FGuid::NewGuid();
	Manifest.Title = Request.Title;
	Manifest.Description = Request.Description;
	Manifest.AuthorName = Profile->GetProfile().DisplayName;
	Manifest.Kind = Request.Kind;
	Manifest.Category = Request.Category;
	Manifest.Keywords = Request.Keywords;
	Manifest.Price = Request.Price;
	Manifest.CreatedAtUtc = FDateTime::UtcNow();
	StagePackage::NormalizeManifest(Manifest);
	// The text and price are checked now, for immediate feedback; the counts are filled in from the scene and checked again by Write.
	if (StagePackage::ValidateManifest(Manifest) != EStageMarketplaceResult::Success)
	{
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::InvalidRequest,
			FText::Format(LOCTEXT("Manifest", "Check the listing: a title of 1-{0} characters, a description of at most {1}, at most {2} keywords of up to {3} characters, and a price of 0-{4} credits."),
				StagePackage::MaxTitleLength, StagePackage::MaxDescriptionLength, StagePackage::MaxKeywords, StagePackage::MaxKeywordLength, StagePackage::MaxCreditsPrice));
	}

	// Gathered on the game thread (the catalog is UObjects); the background task gets plain paths.
	TSet<FSoftObjectPath> LightItems;
	for (const UBaseItemData* Item : Items->GetCatalog())
	{
		if (Item && Item->IsLight())
		{
			LightItems.Add(FSoftObjectPath(Item));
		}
	}

	bPublishing = true;
	WorkPipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), Manifest = MoveTemp(Manifest), SceneName = Request.SceneName,
		LightItems = MoveTemp(LightItems), Thumbnail = Request.ThumbnailPng]() mutable
	{
		const TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
		const EStageMarketplaceResult Result = StageMarketplace::BuildPackage(MoveTemp(Manifest), SceneName, LightItems, MoveTemp(Thumbnail), *Bytes);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Bytes]()
		{
			if (ThisClass* This = WeakThis.Get())
			{
				This->HandlePackageBuilt(Result, Bytes);
			}
		});
	});
	return FStageMarketplaceResultInfo::Ok();
}

void UStageMarketplaceSubsystem::HandlePackageBuilt(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes)
{
	if (Result == EStageMarketplaceResult::NotFound)
	{
		FinishPublish(FStageMarketplaceResultInfo::Fail(Result, LOCTEXT("SceneNotFound", "That scene is not saved. Save the stage first (File > Save), then publish it.")),
			FStageMarketplaceListing());
		return;
	}
	if (Result != EStageMarketplaceResult::Success || !Backend)
	{
		FinishPublish(StageMarketplace::Info(Backend ? Result : EStageMarketplaceResult::BackendUnavailable), FStageMarketplaceListing());
		return;
	}
	Backend->Publish(Bytes, FOnStageMarketplacePublished::CreateUObject(this, &ThisClass::HandlePublished));
}

void UStageMarketplaceSubsystem::HandlePublished(EStageMarketplaceResult Result, const FStageMarketplaceListing& Listing)
{
	FStageMarketplaceResultInfo Info = StageMarketplace::Info(Result);
	if (Info.IsSuccess())
	{
		Info.Message = FText::Format(LOCTEXT("Published", "Published \"{0}\" to {1}."), FText::FromString(Listing.Title), GetBackendName());
	}
	FinishPublish(Info, Listing);
}

void UStageMarketplaceSubsystem::FinishPublish(const FStageMarketplaceResultInfo& Result, const FStageMarketplaceListing& Listing)
{
	bPublishing = false;
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace publish: %s %s (listing %s)"), *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString(), *Listing.ListingId.ToString());
	OnPublishCompleted.Broadcast(Result, Listing);
}

#undef LOCTEXT_NAMESPACE
