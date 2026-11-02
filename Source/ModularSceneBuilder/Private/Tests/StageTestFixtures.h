// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Economy/StageEconomyTypes.h"
#include "Marketplace/StageLocalMarketplaceStore.h"
#include "Marketplace/StageMarketplaceSubsystem.h"
#include "Marketplace/StagePackage.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/StageEconomySubsystem.h"
#include "Subsystems/StageProfileSubsystem.h"
#include "Tests/StageTestGameInstance.h"

/** Tests only: content for the integration tests that run on FStageTestGameInstance (STATE.md #55). */
namespace StageTestFixtures
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	inline FString Str(EStageEconomyResult Result) { return UEnum::GetValueAsString(Result); }
	inline FString Str(EStageMarketplaceResult Result) { return UEnum::GetValueAsString(Result); }
	inline FString Str(EStageOwnershipState State) { return UEnum::GetValueAsString(State); }

	/** Two catalog items that exist in the project's content (the marketplace tests use the same). */
	inline FStageSceneDocument MakeScene(const FString& Name)
	{
		FStageSceneDocument Scene;
		Scene.Name = Name;
		Scene.LevelName = TEXT("L_StageTest");
		for (const TCHAR* Path : { TEXT("/Game/StageCraft/Items/DA_Test_Crate.DA_Test_Crate"), TEXT("/Game/StageCraft/Items/DA_Test_Spot.DA_Test_Spot") })
		{
			FStageItemSnapshot& Item = Scene.Items.AddDefaulted_GetRef();
			Item.InstanceId = FGuid::NewGuid();
			Item.Item = TSoftObjectPtr<UBaseItemData>(FSoftObjectPath(Path));
			Item.Transform = FTransform(FVector(100.0 * Scene.Items.Num(), 0.0, 0.0));
			Item.DisplayName = FText::FromString(TEXT("Item"));
		}
		return Scene;
	}

	/**
	 * Publishes a listing straight into the community folder of the current data root, as another user's StageCraft would have.
	 * Returns its id, or an invalid id when the package was refused.
	 */
	inline FGuid PublishListing(const FString& Title, int64 Credits, bool bAllowExport)
	{
		FStagePackage Package;
		Package.Scene = MakeScene(Title);
		Package.Manifest.PackageId = FGuid::NewGuid();
		Package.Manifest.Title = Title;
		Package.Manifest.Description = TEXT("Integration test listing.");
		Package.Manifest.AuthorName = TEXT("Test Publisher");
		Package.Manifest.Kind = EStagePackageKind::StageDesign;
		Package.Manifest.Category = StageCraftTags::Marketplace_Category_Concert;
		Package.Manifest.Keywords = { TEXT("test") };
		Package.Manifest.Price.Credits = Credits;
		Package.Manifest.bAllowExport = bAllowExport;
		Package.Manifest.CreatedAtUtc = FDateTime::UtcNow();
		Package.Manifest.ItemCount = Package.Scene.Items.Num();
		Package.Manifest.RequiredItems = StagePackage::CollectRequiredItems(Package.Scene);

		TArray<uint8> Bytes;
		FStageMarketplaceListing Listing;
		const FStageLocalMarketplaceStore Store(FStageLocalMarketplaceStore::GetDefaultDirectory());
		if (StagePackage::Write(Package, Bytes) != EStageMarketplaceResult::Success || Store.Publish(Bytes, Listing) != EStageMarketplaceResult::Success)
		{
			return FGuid();
		}
		return Listing.ListingId;
	}

	/** Runs a query for every listing and waits for its answer. False when it did not answer. */
	inline bool QueryAll(FStageTestGameInstance& Game)
	{
		UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
		if (!Marketplace)
		{
			return false;
		}
		FStageMarketplaceQuery Query;
		Query.Limit = FStageMarketplaceQuery::MaxPageSize;
		Marketplace->RequestQuery(Query);
		return Game.Pump([Marketplace]() { return !Marketplace->IsQueryInFlight(); });
	}

	/** Sets the credits balance to exactly Amount (development grant). Returns the balance afterwards. */
	inline int64 SetCredits(FStageTestGameInstance& Game, int64 Amount)
	{
		UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
		UStageEconomySubsystem* Economy = Game.Subsystem<UStageEconomySubsystem>();
		if (!Profile || !Economy)
		{
			return -1;
		}
		Economy->DevGrantCurrency(StageCraftTags::Currency_Credits, Amount - Profile->GetBalance(StageCraftTags::Currency_Credits));
		return Profile->GetBalance(StageCraftTags::Currency_Credits);
	}

	inline int64 GetCredits(FStageTestGameInstance& Game)
	{
		const UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
		return Profile ? Profile->GetBalance(StageCraftTags::Currency_Credits) : -1;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
