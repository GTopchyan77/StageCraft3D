// Copyright Epic Games, Inc. All Rights Reserved.

#include "Marketplace/StageMarketplaceSubsystem.h"

#include "Algo/Count.h"
#include "Async/Async.h"
#include "Data/BaseItemData.h"
#include "Engine/GameInstance.h"
#include "Fixtures/StageFixtureLibraryRules.h"
#include "Fixtures/StageFixtureLibrarySubsystem.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "IO/StageAtomicFile.h"
#include "Marketplace/StageAcquireDispatchSubsystem.h"
#include "Marketplace/StageMarketplaceRules.h"
#include "Marketplace/StagePackage.h"
#include "Misc/FileHelper.h"
#include "IO/StageCraftPaths.h"
#include "Misc/Paths.h"
#include "ModularSceneBuilder.h"
#include "Scene/StageSceneStore.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageItemSubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMarketplaceSubsystem)

#define LOCTEXT_NAMESPACE "StageCraftMarketplace"

namespace StageMarketplace
{
	using FStageMarketplaceFixtureFiles = TArray<TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile>>;

	FText AddCarriedFixtures(UGameInstance* GameInstance, const FStageMarketplaceFixtureFiles& Files);

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
		case EStageMarketplaceResult::NotPortable: return LOCTEXT("Result_NotPortable", "The stage contains imported models, which exist only on this computer, or GDTF fixture types whose files are no longer in your Library.");
		case EStageMarketplaceResult::WriteFailed: return LOCTEXT("Result_WriteFailed", "The file could not be written.");
		case EStageMarketplaceResult::Busy: return LOCTEXT("Result_Busy", "This is already in progress.");
		case EStageMarketplaceResult::BackendUnavailable: return LOCTEXT("Result_BackendUnavailable", "The marketplace is not available.");
		case EStageMarketplaceResult::NotOwned: return LOCTEXT("Result_NotOwned", "Buy this listing first.");
		case EStageMarketplaceResult::PurchaseFailed: return LOCTEXT("Result_PurchaseFailed", "The purchase did not go through.");
		case EStageMarketplaceResult::ServerRequired: return LOCTEXT("Result_ServerRequired", "Real-money sales need an online store; they are not available offline.");
		case EStageMarketplaceResult::NotOriginal: return LOCTEXT("Result_NotOriginal", "This stage came from the marketplace; only your own stages can be published.");
		case EStageMarketplaceResult::TimedOut: return LOCTEXT("Result_TimedOut", "The marketplace did not deliver the package in time. If you bought it, get it again: it costs nothing.");
		case EStageMarketplaceResult::NotYours: return LOCTEXT("Result_NotYours", "Only listings published from this computer can be unpublished here.");
		case EStageMarketplaceResult::InUse: return LOCTEXT("Result_InUse", "This listing is being downloaded or delivered right now; try again when that has finished.");
		case EStageMarketplaceResult::SignInRequired: return LOCTEXT("Result_SignInRequired", "Sign in to StageCraft Cloud first (Account menu).");
		case EStageMarketplaceResult::SharingNotConfirmed: return LOCTEXT("Result_SharingNotConfirmed", "This stage uses GDTF fixtures, and their files travel with it. Confirm that you may share them, then publish again.");
		}
		return FText::GetEmpty();
	}

	FStageMarketplaceResultInfo Info(EStageMarketplaceResult Result)
	{
		return Result == EStageMarketplaceResult::Success ? FStageMarketplaceResultInfo::Ok() : FStageMarketplaceResultInfo::Fail(Result, Describe(Result));
	}

	/** Verifies a fetched package and writes its scene into the scenes folder under a fresh name. Runs on the work pipe. */
	EStageMarketplaceResult ImportPackage(const TArray<uint8>& Bytes, const FGuid& ListingId, FString& OutSceneName, FStageMarketplaceFixtureFiles& OutFixtureFiles)
	{
		OutFixtureFiles.Reset();
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
		for (FStagePackageFixtureFile& Carried : Package.FixtureFiles)
		{
			TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile> File = UStageFixtureLibrarySubsystem::ParseEmbeddedFile(MoveTemp(Carried.Bytes),
				Carried.FixtureTypeId.ToString(EGuidFormats::Digits) + TEXT(".gdtf"));
			if (!UStageFixtureLibrarySubsystem::CanAddParsedFile(*File, Carried.FixtureTypeId))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: %s carries a GDTF file for %s that cannot be used; the package is refused."), *ListingId.ToString(), *Carried.FixtureTypeId.ToString());
				OutFixtureFiles.Reset();
				return EStageMarketplaceResult::Corrupt;
			}
			OutFixtureFiles.Add(File);
		}

		const FStageSceneStore Scenes(FStageSceneStore::GetDefaultDirectory());
		OutSceneName = StageMarketplaceRules::MakeImportSceneName(Package.Manifest.Title, [&Scenes](const FString& Name) { return Scenes.SceneExists(Name); });
		if (OutSceneName.IsEmpty())
		{
			return EStageMarketplaceResult::WriteFailed;
		}
		Package.Scene.Name = OutSceneName;
		// Whatever the package says, this copy is the listing that was acquired; it stays inside the app (ADR 0022 section 4).
		Package.Scene.OriginListingId = ListingId;
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
		TArray<uint8> ThumbnailPng, bool bFixtureSharingConfirmed, TArray<uint8>& OutBytes)
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
		if (Report.InvalidItems > 0 || Report.RepairedIds > 0 || Report.InvalidCueEntries > 0 || Report.InvalidShowEntries > 0)
		{
			return EStageMarketplaceResult::Corrupt;
		}
		// Republishing someone else's stage is not allowed, whatever it is renamed to (ADR 0022 section 4).
		if (Package.Scene.IsFromMarketplace())
		{
			return EStageMarketplaceResult::NotOriginal;
		}
		if (!StagePackage::IsPortable(Package.Scene))
		{
			return EStageMarketplaceResult::NotPortable;
		}

		Manifest.ItemCount = Package.Scene.Items.Num();
		Manifest.CueCount = Package.Scene.Cues.Num();
		Manifest.FixtureCount = Algo::CountIf(Package.Scene.Items, [&LightItems](const FStageItemSnapshot& Item) { return LightItems.Contains(Item.Item.ToSoftObjectPath()); });
		Manifest.RequiredItems = StagePackage::CollectRequiredItems(Package.Scene);
		Manifest.FixtureTypes = StagePackage::CollectFixtureTypes(Package.Scene);
		if (!Manifest.FixtureTypes.IsEmpty() && !bFixtureSharingConfirmed)
		{
			return EStageMarketplaceResult::SharingNotConfirmed;
		}
		for (const FGuid& TypeId : Manifest.FixtureTypes)
		{
			const FString Path = StageFixtureLibrary::GetLibraryFilePath(TypeId);
			const int64 Size = IFileManager::Get().FileSize(*Path);
			if (Size > StagePackage::MaxFixtureFileBytes)
			{
				return EStageMarketplaceResult::TooLarge;
			}
			FStagePackageFixtureFile& File = Package.FixtureFiles.AddDefaulted_GetRef();
			File.FixtureTypeId = TypeId;
			if (Size < 0 || !FFileHelper::LoadFileToArray(File.Bytes, *Path, FILEREAD_Silent))
			{
				UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: the GDTF file of fixture type %s is not in the Library (%s); the stage cannot be published."), *TypeId.ToString(), *Path);
				return EStageMarketplaceResult::NotPortable;
			}
		}
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
	LoadLocalRecords();

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
	TArray<FGuid> Fetching;
	PendingFetches.GetKeys(Fetching);
	for (const FGuid& ListingId : Fetching)
	{
		ClearFetchTimer(ListingId);
	}
	// Imports and package builds are bounded file work; the pipe must be empty before it is destroyed.
	WorkPipe.WaitUntilEmpty();

	OnListingsUpdated.Clear();
	OnAcquireCompleted.Clear();
	OnPublishCompleted.Clear();
	OnUnpublishCompleted.Clear();
	OnLibraryChanged.Clear();
	OnPreviewReady.Clear();
	Listings.Reset();
	PendingAcquires.Reset();
	PublishedByMe.Reset();
	LibraryListings.Reset();
	bQueryInFlight = false;
	bPreviewLoading = false;
	bPublishing = false;
	bUnpublishing = false;
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
	return FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Marketplace"), TEXT("Library"));
}

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::CheckExportRights(const FGuid& OriginListingId) const
{
	FStagePackageManifest Manifest;
	const bool bHasManifest = ReadLibraryManifest(OriginListingId, Manifest);
	return DecideExportRights(OriginListingId, bHasManifest ? &Manifest : nullptr);
}

bool UStageMarketplaceSubsystem::ReadLibraryManifest(const FGuid& OriginListingId, FStagePackageManifest& OutManifest)
{
	if (!OriginListingId.IsValid())
	{
		return false;
	}
	// The library copy is the publisher's package exactly as it was acquired; a missing or unreadable copy means no rights.
	const FString LibraryPath = FPaths::Combine(GetLibraryDirectory(), OriginListingId.ToString(EGuidFormats::Digits) + StagePackage::FileExtension);
	TArray<uint8> Bytes;
	bool bHasThumbnail = false;
	return IFileManager::Get().FileSize(*LibraryPath) <= StagePackage::MaxPackageBytes
		&& FFileHelper::LoadFileToArray(Bytes, *LibraryPath, FILEREAD_Silent)
		&& StagePackage::ReadManifest(Bytes, OutManifest, bHasThumbnail) == EStageMarketplaceResult::Success;
}

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::DecideExportRights(const FGuid& OriginListingId, const FStagePackageManifest* LibraryManifest) const
{
	const UStageProfileSubsystem* Profile = GetProfile();
	if (!Profile || !OriginListingId.IsValid())
	{
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal, StageMarketplace::Describe(EStageMarketplaceResult::NotOriginal));
	}

	switch (StageMarketplaceRules::ResolveExportRights(OriginListingId, LibraryManifest, Profile->GetProfile()))
	{
	case EStageExportRights::Allowed:
		return FStageMarketplaceResultInfo::Ok();
	case EStageExportRights::NoLibraryCopy:
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal,
			LOCTEXT("ExportNoLibrary", "This stage came from the marketplace and its listing is no longer in your library. Get the listing again to export it."));
	case EStageExportRights::NotAllowedByPublisher:
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal,
			LOCTEXT("ExportNotAllowed", "The publisher of this stage does not allow it to leave StageCraft. You can still open and use it here."));
	case EStageExportRights::NotOwned:
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal,
			LOCTEXT("ExportNotOwned", "This marketplace stage is not owned by this profile, so it cannot be exported."));
	case EStageExportRights::Mismatch:
	default:
		return FStageMarketplaceResultInfo::Fail(EStageMarketplaceResult::NotOriginal, StageMarketplace::Describe(EStageMarketplaceResult::NotOriginal));
	}
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
	RequestedQuery = StageMarketplaceRules::SanitizeQuery(Query);
	Backend->Query(RequestedQuery, FOnStageMarketplaceQueried::CreateUObject(this, &ThisClass::HandleQueried, Serial));
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
		AnsweredQuery = RequestedQuery;
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
	const uint32 Serial = ++FetchSerial;
	FPendingFetch& Fetch = PendingFetches.FindOrAdd(ListingId);
	Fetch.Serial = Serial;
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		GameInstance->GetTimerManager().SetTimer(Fetch.Timer, FTimerDelegate::CreateUObject(this, &ThisClass::HandleFetchTimeout, ListingId, Serial),
			GetFetchTimeoutSeconds(), /*bLoop*/ false);
	}
	Backend->FetchPackage(ListingId, EStageMarketplaceFetchPurpose::Acquire, FOnStageMarketplacePackageFetched::CreateUObject(this, &ThisClass::HandlePackageFetched, ListingId, Serial));
}

float UStageMarketplaceSubsystem::GetFetchTimeoutSeconds() const
{
	constexpr float DefaultTimeoutSeconds = 60.f;
	return FetchTimeoutSeconds > 0.f ? FetchTimeoutSeconds : DefaultTimeoutSeconds;
}

void UStageMarketplaceSubsystem::ClearFetchTimer(const FGuid& ListingId)
{
	FPendingFetch Fetch;
	if (PendingFetches.RemoveAndCopyValue(ListingId, Fetch))
	{
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			GameInstance->GetTimerManager().ClearTimer(Fetch.Timer);
		}
	}
}

void UStageMarketplaceSubsystem::HandleFetchTimeout(FGuid ListingId, uint32 Serial)
{
	const FPendingFetch* Fetch = PendingFetches.Find(ListingId);
	if (!Fetch || Fetch->Serial != Serial)
	{
		return;
	}
	PendingFetches.Remove(ListingId);
	UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: the package of %s did not arrive within %.0f s."), *ListingId.ToString(), GetFetchTimeoutSeconds());
	FinishAcquire(ListingId, StageMarketplace::Info(EStageMarketplaceResult::TimedOut), FString());
}

void UStageMarketplaceSubsystem::HandlePackageFetched(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes, FGuid ListingId, uint32 Serial)
{
	// Only the fetch still awaited counts: a package that arrives after its deadline (or after a newer fetch) is dropped.
	const FPendingFetch* Fetch = PendingFetches.Find(ListingId);
	if (!Fetch || Fetch->Serial != Serial)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Marketplace: ignored a package of %s that is no longer awaited."), *ListingId.ToString());
		return;
	}
	ClearFetchTimer(ListingId);
	if (Result != EStageMarketplaceResult::Success || !Bytes.IsValid())
	{
		FinishAcquire(ListingId, StageMarketplace::Info(Result == EStageMarketplaceResult::Success ? EStageMarketplaceResult::Corrupt : Result), FString());
		return;
	}

	WorkPipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), Bytes, ListingId]()
	{
		FString SceneName;
		StageMarketplace::FStageMarketplaceFixtureFiles FixtureFiles;
		const EStageMarketplaceResult ImportResult = StageMarketplace::ImportPackage(*Bytes, ListingId, SceneName, FixtureFiles);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ImportResult, ListingId, SceneName = MoveTemp(SceneName), FixtureFiles = MoveTemp(FixtureFiles)]()
		{
			if (ThisClass* This = WeakThis.Get())
			{
				const FText Note = ImportResult == EStageMarketplaceResult::Success ? StageMarketplace::AddCarriedFixtures(This->GetGameInstance(), FixtureFiles) : FText::GetEmpty();
				This->FinishAcquire(ListingId, StageMarketplace::Info(ImportResult), SceneName, Note);
			}
		});
	});
}

FText StageMarketplace::AddCarriedFixtures(UGameInstance* GameInstance, const FStageMarketplaceFixtureFiles& Files)
{
	UStageFixtureLibrarySubsystem* Library = GameInstance ? GameInstance->GetSubsystem<UStageFixtureLibrarySubsystem>() : nullptr;
	if (Files.IsEmpty())
	{
		return FText::GetEmpty();
	}
	if (!Library)
	{
		return LOCTEXT("NoFixtureLibrary", "Its GDTF fixtures could not be added: the fixture Library is not available.");
	}
	int32 Added = 0;
	TArray<FText> Failures;
	for (const TSharedRef<UStageFixtureLibrarySubsystem::FParsedFile>& File : Files)
	{
		const FStageFixtureImportOutcome Outcome = Library->AddEmbeddedFile(*File);
		if (Outcome.IsSuccess())
		{
			++Added;
		}
		else if (Outcome.Result != EStageFixtureImportResult::AlreadyInLibrary)
		{
			Failures.Add(Outcome.Message);
		}
	}
	if (!Failures.IsEmpty())
	{
		return FText::Format(LOCTEXT("CarriedFixturesFailed", "{0} of its GDTF fixture types could not be added: {1}"), FText::AsNumber(Failures.Num()), Failures[0]);
	}
	return Added > 0
		? FText::Format(LOCTEXT("CarriedFixturesAdded", "Added {0} GDTF fixture {0}|plural(one=type,other=types) to your Library."), FText::AsNumber(Added))
		: FText::GetEmpty();
}

void UStageMarketplaceSubsystem::FinishAcquire(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FString& SceneName, const FText& Note)
{
	PendingAcquires.Remove(ListingId);
	FStageMarketplaceResultInfo Final = Result;
	if (Final.IsSuccess())
	{
		Final.Message = FText::Format(LOCTEXT("Imported", "Added to your scenes as \"{0}\"."), FText::FromString(SceneName));
		if (!Note.IsEmpty())
		{
			Final.Message = FText::Format(LOCTEXT("ImportedWithNote", "{0} {1}"), Final.Message, Note);
		}
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace acquire %s: %s %s"), *ListingId.ToString(), *UEnum::GetValueAsString(Final.Code), *Final.Message.ToString());
	if (Final.IsSuccess())
	{
		// The import keeps the library copy (a failed write is logged there, and export rights then say so).
		LibraryListings.Add(ListingId);
	}
	OnAcquireCompleted.Broadcast(ListingId, Final, Final.IsSuccess() ? SceneName : FString());
	if (Final.IsSuccess())
	{
		OnLibraryChanged.Broadcast(ListingId, FStageMarketplaceResultInfo::Ok());
	}
}

// --- Read-only preview ---

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::RequestPreview(FGuid ListingId)
{
	if (!Backend)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::BackendUnavailable);
	}
	// Only offers the backend returned; a caller cannot point the preview at an arbitrary id.
	if (!FindListing(ListingId))
	{
		return StageMarketplace::Info(EStageMarketplaceResult::NotFound);
	}
	const uint32 Serial = ++PreviewSerial;
	bPreviewLoading = true;
	Backend->FetchPackage(ListingId, EStageMarketplaceFetchPurpose::Preview,
		FOnStageMarketplacePackageFetched::CreateUObject(this, &ThisClass::HandlePreviewFetched, ListingId, Serial));
	return FStageMarketplaceResultInfo::Ok();
}

void UStageMarketplaceSubsystem::HandlePreviewFetched(EStageMarketplaceResult Result, const FStageMarketplacePackageBytes& Bytes, FGuid ListingId, uint32 Serial)
{
	if (Serial != PreviewSerial)
	{
		return; // Superseded: another listing is being previewed now.
	}
	if (Result != EStageMarketplaceResult::Success || !Bytes.IsValid())
	{
		FinishPreview(ListingId, StageMarketplace::Info(Result == EStageMarketplaceResult::Success ? EStageMarketplaceResult::Corrupt : Result), nullptr, Serial);
		return;
	}

	WorkPipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), Bytes, ListingId, Serial]()
	{
		// The same verification as an import: a package that would not import is not shown either.
		FStagePackage Package;
		EStageMarketplaceResult ReadResult = StagePackage::Read(*Bytes, Package);
		if (ReadResult == EStageMarketplaceResult::Success && Package.Manifest.PackageId != ListingId)
		{
			ReadResult = EStageMarketplaceResult::Corrupt;
		}
		FStageMarketplacePreviewScene Scene;
		if (ReadResult == EStageMarketplaceResult::Success)
		{
			Scene = MakeShared<const FStageSceneDocument, ESPMode::ThreadSafe>(MoveTemp(Package.Scene));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ReadResult, Scene = MoveTemp(Scene), ListingId, Serial]()
		{
			if (ThisClass* This = WeakThis.Get())
			{
				This->FinishPreview(ListingId, StageMarketplace::Info(ReadResult), Scene, Serial);
			}
		});
	});
}

void UStageMarketplaceSubsystem::FinishPreview(const FGuid& ListingId, const FStageMarketplaceResultInfo& Result, const FStageMarketplacePreviewScene& Scene, uint32 Serial)
{
	if (Serial != PreviewSerial)
	{
		return;
	}
	bPreviewLoading = false;
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace preview %s: %s %s"), *ListingId.ToString(), *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString());
	OnPreviewReady.Broadcast(ListingId, Result, Result.IsSuccess() ? Scene : nullptr);
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
	Manifest.bAllowExport = Request.bAllowExport;
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
		LightItems = MoveTemp(LightItems), Thumbnail = Request.ThumbnailPng, bConfirmFixtureSharing = Request.bConfirmFixtureSharing]() mutable
	{
		const TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
		const EStageMarketplaceResult Result = StageMarketplace::BuildPackage(MoveTemp(Manifest), SceneName, LightItems, MoveTemp(Thumbnail), bConfirmFixtureSharing, *Bytes);
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
	if (Result.IsSuccess() && Listing.ListingId.IsValid())
	{
		PublishedByMe.Add(Listing.ListingId);
		SavePublishRecord();
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace publish: %s %s (listing %s)"), *UEnum::GetValueAsString(Result.Code), *Result.Message.ToString(), *Listing.ListingId.ToString());
	OnPublishCompleted.Broadcast(Result, Listing);
}

// --- Removing what was put in (ADR 0021 §15) ---

namespace StageMarketplacePublishRecord
{
	constexpr int32 Version = 1;
	const TCHAR* VersionField = TEXT("version");
	const TCHAR* ListingsField = TEXT("listings");
}

FString UStageMarketplaceSubsystem::GetPublishRecordPath()
{
	return FPaths::Combine(StageCraftPaths::DataRoot(), TEXT("Marketplace"), TEXT("Published.json"));
}

void UStageMarketplaceSubsystem::LoadLocalRecords()
{
	using namespace StageMarketplacePublishRecord;
	PublishedByMe.Reset();
	LibraryListings.Reset();

	// Bounded start-up reads (one small file and one folder listing), done once so every later question is answered from memory.
	FString Contents;
	TSharedPtr<FJsonObject> Root;
	const FString RecordPath = GetPublishRecordPath();
	if (FFileHelper::LoadFileToString(Contents, *RecordPath, FFileHelper::EHashOptions::None, FILEREAD_Silent))
	{
		const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Contents), Root) && Root.IsValid() && Root->TryGetArrayField(ListingsField, Ids))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Ids)
			{
				FGuid Id;
				if (Value.IsValid() && Value->Type == EJson::String && FGuid::Parse(Value->AsString(), Id) && Id.IsValid())
				{
					PublishedByMe.Add(Id);
				}
			}
		}
		else
		{
			// Unreadable: nothing can be unpublished from here until the next publish rewrites it. Never a reason to fail start-up.
			UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: the publish record %s could not be read; treating it as empty."), *RecordPath);
		}
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(GetLibraryDirectory(), FString(TEXT("*")) + StagePackage::FileExtension), /*Files*/ true, /*Directories*/ false);
	for (const FString& File : Files)
	{
		FGuid Id;
		if (FGuid::Parse(FPaths::GetBaseFilename(File), Id) && Id.IsValid())
		{
			LibraryListings.Add(Id);
		}
	}
}

void UStageMarketplaceSubsystem::SavePublishRecord()
{
	using namespace StageMarketplacePublishRecord;
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(VersionField, Version);
	TArray<TSharedPtr<FJsonValue>> Ids;
	for (const FGuid& Id : PublishedByMe)
	{
		Ids.Add(MakeShared<FJsonValueString>(Id.ToString(EGuidFormats::Digits)));
	}
	Root->SetArrayField(ListingsField, Ids);
	FString Contents;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<TCHAR>::Create(&Contents));

	// On the work pipe, after the imports and builds before it; only strings cross (Rules §29).
	WorkPipe.Launch(UE_SOURCE_LOCATION, [Path = GetPublishRecordPath(), Contents = MoveTemp(Contents)]()
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
		if (!StageAtomicFile::WriteText(Path, Contents))
		{
			UE_LOG(LogStageCraft, Warning, TEXT("Marketplace: could not write the publish record %s."), *Path);
		}
	});
}

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::RequestUnpublish(FGuid ListingId)
{
	if (!Backend)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::BackendUnavailable);
	}
	if (!ListingId.IsValid())
	{
		return StageMarketplace::Info(EStageMarketplaceResult::InvalidRequest);
	}
	// Deny by default: only listings this install published. The folder itself cannot tell who published what.
	if (!PublishedByMe.Contains(ListingId))
	{
		return StageMarketplace::Info(EStageMarketplaceResult::NotYours);
	}
	if (bUnpublishing)
	{
		return StageMarketplace::Info(EStageMarketplaceResult::Busy);
	}
	if (PendingAcquires.Contains(ListingId))
	{
		return StageMarketplace::Info(EStageMarketplaceResult::InUse);
	}
	bUnpublishing = true;
	Backend->Unpublish(ListingId, FOnStageMarketplaceUnpublished::CreateUObject(this, &ThisClass::HandleUnpublished, ListingId));
	return FStageMarketplaceResultInfo::Ok();
}

void UStageMarketplaceSubsystem::HandleUnpublished(EStageMarketplaceResult Result, FGuid ListingId)
{
	bUnpublishing = false;
	FStageMarketplaceResultInfo Info = StageMarketplace::Info(Result);
	// Already gone from the folder (removed by hand, or by another machine): the record catches up, as after a success.
	if (Result == EStageMarketplaceResult::Success || Result == EStageMarketplaceResult::NotFound)
	{
		const FStageMarketplaceListing* Listing = FindListing(ListingId);
		Info = FStageMarketplaceResultInfo::Ok(Listing
			? FText::Format(LOCTEXT("Unpublished", "Unpublished \"{0}\". People who already got it keep their copy."), FText::FromString(Listing->Title))
			: LOCTEXT("UnpublishedNoTitle", "Unpublished. People who already got it keep their copy."));
		PublishedByMe.Remove(ListingId);
		SavePublishRecord();
		Listings.RemoveAll([&ListingId](const FStageMarketplaceListing& Candidate) { return Candidate.ListingId == ListingId; });
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace unpublish %s: %s %s"), *ListingId.ToString(), *UEnum::GetValueAsString(Result), *Info.Message.ToString());
	OnUnpublishCompleted.Broadcast(ListingId, Info);
	if (Info.IsSuccess())
	{
		RequestQuery(RequestedQuery);
	}
}

FStageMarketplaceResultInfo UStageMarketplaceSubsystem::RequestRemoveFromLibrary(FGuid ListingId)
{
	if (!ListingId.IsValid())
	{
		return StageMarketplace::Info(EStageMarketplaceResult::InvalidRequest);
	}
	if (!LibraryListings.Contains(ListingId))
	{
		return StageMarketplace::Info(EStageMarketplaceResult::NotFound);
	}
	// An acquire writes the copy and an order's delivery reads it (export rights): neither may lose it half-way.
	const UStageAcquireDispatchSubsystem* Dispatch = GetGameInstance()->GetSubsystem<UStageAcquireDispatchSubsystem>();
	if (PendingAcquires.Contains(ListingId) || (Dispatch && Dispatch->IsOrderPending() && Dispatch->GetPendingOrder().ListingId == ListingId))
	{
		return StageMarketplace::Info(EStageMarketplaceResult::InUse);
	}
	// Taken out of the set now, so a second request is NotFound rather than a second delete.
	LibraryListings.Remove(ListingId);
	WorkPipe.Launch(UE_SOURCE_LOCATION, [WeakThis = TWeakObjectPtr<ThisClass>(this), ListingId, Path = FPaths::Combine(GetLibraryDirectory(), ListingId.ToString(EGuidFormats::Digits) + StagePackage::FileExtension)]()
	{
		const bool bDeleted = IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ false, /*Quiet*/ true) && !IFileManager::Get().FileExists(*Path);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, ListingId, bDeleted]()
		{
			if (ThisClass* This = WeakThis.Get())
			{
				This->FinishRemoveFromLibrary(ListingId, bDeleted ? EStageMarketplaceResult::Success : EStageMarketplaceResult::WriteFailed);
			}
		});
	});
	return FStageMarketplaceResultInfo::Ok();
}

void UStageMarketplaceSubsystem::FinishRemoveFromLibrary(const FGuid& ListingId, EStageMarketplaceResult Result)
{
	FStageMarketplaceResultInfo Info = StageMarketplace::Info(Result);
	if (Info.IsSuccess())
	{
		Info.Message = LOCTEXT("RemovedFromLibrary", "Removed from your library. Scenes made from it stay; get the listing again to export them.");
	}
	else
	{
		LibraryListings.Add(ListingId); // Still on disk: still in the library.
	}
	UE_LOG(LogStageCraft, Log, TEXT("Marketplace remove from library %s: %s"), *ListingId.ToString(), *UEnum::GetValueAsString(Result));
	OnLibraryChanged.Broadcast(ListingId, Info);
}

#undef LOCTEXT_NAMESPACE
