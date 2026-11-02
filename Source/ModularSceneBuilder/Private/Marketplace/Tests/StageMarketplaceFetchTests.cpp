// Copyright Epic Games, Inc. All Rights Reserved.

// Integration test of the marketplace download deadline (Docs/ADR/0021-community-marketplace.md §14) on an isolated game instance
// (FStageTestGameInstance) with the live economy and marketplace subsystems.
//  - FetchTimeout: a package that does not arrive in time ends the acquire as TimedOut with the purchase kept; the late package
//    is dropped; Get again downloads it without a second charge; a game instance stopped while a download is awaited shuts down
//    cleanly.
//  - UnpublishFlow (ADR 0021 §15): only listings this install published can be unpublished; removing a library copy keeps
//    ownership, takes the export rights with it until the listing is got again, and waits while the listing is being acquired.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Marketplace.FetchTimeout+StageCraft.Marketplace.UnpublishFlow; Quit" -unattended

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Economy/Tests/StageTestCommerceBackend.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageLocalMarketplaceStore.h"
#include "Marketplace/Tests/StageTestMarketplaceBackend.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scene/StageSceneStore.h"
#include "Tests/StageTestEventRecorder.h"
#include "Tests/StageTestFixtures.h"
#include "UObject/StrongObjectPtr.h"

namespace StageMarketplaceFetchTests
{
	FString LibraryPackagePath(const FGuid& ListingId)
	{
		return FPaths::Combine(UStageMarketplaceSubsystem::GetLibraryDirectory(), ListingId.ToString(EGuidFormats::Digits) + StagePackage::FileExtension);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplaceFetchTimeoutTest, "StageCraft.Marketplace.FetchTimeout", StageTestFixtures::Flags)

bool FStageMarketplaceFetchTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageMarketplaceFetchTests;

	// The timeout is logged as a warning on purpose.
	AddExpectedMessage(TEXT("did not arrive within"), EAutomationExpectedMessageFlags::Contains, 1);

	FStageTestGameInstanceOptions Options;
	Options.FetchTimeoutSeconds = 0.3f;
	FStageTestGameInstance Game(Options);
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	UStageTestMarketplaceBackend* Store = UStageTestMarketplaceBackend::Find(Game.Get());
	if (!TestNotNull(TEXT("Marketplace"), Marketplace) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Commerce"), Commerce)
		|| !TestNotNull(TEXT("The test marketplace backend is in use"), Store))
	{
		return false;
	}
	TStrongObjectPtr<UStageTestEventRecorder> Recorder(NewObject<UStageTestEventRecorder>());
	Recorder->Bind(Game.Get());

	const FGuid Slow = PublishListing(TEXT("Slow Download"), 100, false);
	const FGuid Stuck = PublishListing(TEXT("Never Arrives"), 0, false);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	SetCredits(Game, 500);

	AddInfo(TEXT("Bought, then the package does not arrive in time: TimedOut, the purchase is kept."));
	Store->bHoldFetches = true;
	TestTrue(TEXT("The acquire starts"), Marketplace->RequestAcquire(Slow).IsSuccess());
	TestEqual(TEXT("The store was asked once"), Commerce->PurchaseRequests.Num(), 1);
	TestEqual(TEXT("The download was requested"), Store->GetHeldFetchCount(), 1);
	TestTrue(TEXT("The acquire ends"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 1; }, 3.0));
	if (Recorder->Acquires.Num() == 1)
	{
		TestEqual(TEXT("as TimedOut"), Str(Recorder->Acquires[0].Result.Code), Str(EStageMarketplaceResult::TimedOut));
		TestTrue(TEXT("with no scene"), Recorder->Acquires[0].SceneName.IsEmpty());
	}
	TestFalse(TEXT("Not acquiring any more"), Marketplace->IsAcquiring(Slow));
	TestEqual(TEXT("Charged once"), GetCredits(Game), int64(400));
	TestTrue(TEXT("Owned"), Profile->OwnsListing(Slow));

	AddInfo(TEXT("The package arrives late: dropped, nothing imported or kept."));
	TestTrue(TEXT("Delivered late"), Store->DeliverHeldFetch(Slow));
	Game.PumpFor(0.5);
	TestEqual(TEXT("No second result"), Recorder->Acquires.Num(), 1);
	TestFalse(TEXT("Not kept in the library"), IFileManager::Get().FileExists(*LibraryPackagePath(Slow)));

	AddInfo(TEXT("Get again: downloaded for free (it is owned)."));
	Store->bHoldFetches = false;
	TestTrue(TEXT("The second acquire starts"), Marketplace->RequestAcquire(Slow).IsSuccess());
	TestTrue(TEXT("It ends"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 2; }, 10.0));
	if (Recorder->Acquires.Num() == 2)
	{
		const UStageTestEventRecorder::FAcquire& Done = Recorder->Acquires[1];
		TestEqual(TEXT("in success"), Str(Done.Result.Code), Str(EStageMarketplaceResult::Success));
		TestTrue(TEXT("with the imported scene"), FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).SceneExists(Done.SceneName));
	}
	TestEqual(TEXT("The store was not asked again"), Commerce->PurchaseRequests.Num(), 1);
	TestEqual(TEXT("Not charged again"), GetCredits(Game), int64(400));
	TestTrue(TEXT("Kept in the library now"), IFileManager::Get().FileExists(*LibraryPackagePath(Slow)));

	AddInfo(TEXT("Stopped while a download is awaited: a clean shutdown, no result."));
	Store->bHoldFetches = true;
	TestTrue(TEXT("A free acquire starts"), Marketplace->RequestAcquire(Stuck).IsSuccess());
	TestTrue(TEXT("Its download is held"), Marketplace->IsAcquiring(Stuck) && Store->GetHeldFetchCount() == 1);
	Recorder->Unbind();
	Game.Shutdown();
	Game.PumpFor(0.5);
	TestEqual(TEXT("No result after the shutdown"), Recorder->Acquires.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplaceUnpublishFlowTest, "StageCraft.Marketplace.UnpublishFlow", StageTestFixtures::Flags)

bool FStageMarketplaceUnpublishFlowTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageMarketplaceFetchTests;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	UStageTestMarketplaceBackend* Store = UStageTestMarketplaceBackend::Find(Game.Get());
	if (!TestNotNull(TEXT("Marketplace"), Marketplace) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Commerce"), Commerce) || !TestNotNull(TEXT("Store"), Store))
	{
		return false;
	}
	TStrongObjectPtr<UStageTestEventRecorder> Recorder(NewObject<UStageTestEventRecorder>());
	Recorder->Bind(Game.Get());
	const FStageLocalMarketplaceStore Folder(Store->GetDirectory());

	AddInfo(TEXT("Publishing from here records the listing as this install's."));
	TArray<FGuid> Mine;
	for (const TCHAR* Name : { TEXT("My Arena"), TEXT("My Club") })
	{
		TestTrue(FString::Printf(TEXT("%s saved"), Name), FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).Save(MakeScene(Name)) == EStageSceneResult::Success);
		FStageMarketplacePublishRequest Request;
		Request.SceneName = Name;
		Request.Title = Name;
		Request.Category = StageCraftTags::Marketplace_Category_Concert;
		const int32 Before = Recorder->Publishes.Num();
		TestTrue(FString::Printf(TEXT("%s publish starts"), Name), Marketplace->RequestPublish(Request).IsSuccess());
		TestTrue(FString::Printf(TEXT("%s published"), Name), Game.Pump([&Recorder, Before]() { return Recorder->Publishes.Num() > Before; }, 10.0));
		if (Recorder->Publishes.Num() > Before && TestTrue(TEXT("in success"), Recorder->Publishes.Last().Result.IsSuccess()))
		{
			Mine.Add(Recorder->Publishes.Last().ListingId);
		}
	}
	if (!TestEqual(TEXT("Two listings published"), Mine.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("Both are this install's"), Marketplace->IsPublishedByMe(Mine[0]) && Marketplace->IsPublishedByMe(Mine[1]));
	const FGuid Theirs = PublishListing(TEXT("Someone Else's Stage"), 100, /*bAllowExport*/ true);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	TestFalse(TEXT("Another publisher's listing is not this install's"), Marketplace->IsPublishedByMe(Theirs));

	AddInfo(TEXT("Unpublish: only what this install published, one at a time."));
	TestEqual(TEXT("Another publisher's listing"), Str(Marketplace->RequestUnpublish(Theirs).Code), Str(EStageMarketplaceResult::NotYours));
	TestTrue(TEXT("... stays in the folder"), IFileManager::Get().FileExists(*Folder.GetPackagePath(Theirs)));
	TestTrue(TEXT("Mine starts"), Marketplace->RequestUnpublish(Mine[0]).IsSuccess());
	TestEqual(TEXT("A second unpublish meanwhile"), Str(Marketplace->RequestUnpublish(Mine[1]).Code), Str(EStageMarketplaceResult::Busy));
	TestTrue(TEXT("It finishes"), Game.Pump([&Recorder]() { return Recorder->Unpublishes.Num() == 1; }, 10.0));
	TestTrue(TEXT("in success"), Recorder->Unpublishes.Num() == 1 && Recorder->Unpublishes[0].Result.IsSuccess());
	TestFalse(TEXT("The package is gone from the folder"), IFileManager::Get().FileExists(*Folder.GetPackagePath(Mine[0])));
	TestFalse(TEXT("It is no longer recorded as mine"), Marketplace->IsPublishedByMe(Mine[0]));
	TestTrue(TEXT("The listings are queried again"), Game.Pump([Marketplace]() { return !Marketplace->IsQueryInFlight(); }, 5.0));
	TestNull(TEXT("and no longer list it"), Marketplace->FindListing(Mine[0]));
	TestNotNull(TEXT("but still list the others"), Marketplace->FindListing(Theirs));

	AddInfo(TEXT("Remove from library: the copy goes, ownership stays, and so do export rights' rules."));
	SetCredits(Game, 500);
	TestTrue(TEXT("Bought"), Marketplace->RequestAcquire(Theirs).IsSuccess());
	TestTrue(TEXT("and imported"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 1; }, 10.0));
	TestTrue(TEXT("The library has a copy"), Marketplace->HasLibraryCopy(Theirs) && IFileManager::Get().FileExists(*LibraryPackagePath(Theirs)));
	TestTrue(TEXT("Export is allowed (the publisher allows it, and it is owned)"), Marketplace->CheckExportRights(Theirs).IsSuccess());
	const int32 LibraryEventsBefore = Recorder->LibraryChanges.Num();
	TestTrue(TEXT("Removal starts"), Marketplace->RequestRemoveFromLibrary(Theirs).IsSuccess());
	TestEqual(TEXT("A second removal meanwhile"), Str(Marketplace->RequestRemoveFromLibrary(Theirs).Code), Str(EStageMarketplaceResult::NotFound));
	TestTrue(TEXT("It finishes"), Game.Pump([&Recorder, LibraryEventsBefore]() { return Recorder->LibraryChanges.Num() > LibraryEventsBefore; }, 10.0));
	TestFalse(TEXT("The copy is gone"), Marketplace->HasLibraryCopy(Theirs) || IFileManager::Get().FileExists(*LibraryPackagePath(Theirs)));
	TestTrue(TEXT("Still owned"), Profile->OwnsListing(Theirs));
	TestEqual(TEXT("Nothing refunded or charged"), GetCredits(Game), int64(400));
	TestFalse(TEXT("Its scenes can no longer be exported"), Marketplace->CheckExportRights(Theirs).IsSuccess());
	TestTrue(TEXT("Got again"), Marketplace->RequestAcquire(Theirs).IsSuccess());
	TestTrue(TEXT("for free"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 2; }, 10.0));
	TestEqual(TEXT("The store was asked once in all"), Commerce->PurchaseRequests.Num(), 1);
	TestTrue(TEXT("The copy is back, and with it the export rights"), Marketplace->HasLibraryCopy(Theirs) && Marketplace->CheckExportRights(Theirs).IsSuccess());

	AddInfo(TEXT("Neither runs while the listing is being acquired."));
	Store->bHoldFetches = true;
	TestTrue(TEXT("A download starts and is held"), Marketplace->RequestAcquire(Theirs).IsSuccess() && Marketplace->IsAcquiring(Theirs));
	TestEqual(TEXT("Remove from library meanwhile"), Str(Marketplace->RequestRemoveFromLibrary(Theirs).Code), Str(EStageMarketplaceResult::InUse));
	TestTrue(TEXT("The copy is untouched"), Marketplace->HasLibraryCopy(Theirs) && IFileManager::Get().FileExists(*LibraryPackagePath(Theirs)));
	Store->DeliverHeldFetch(Theirs);
	TestTrue(TEXT("The download finishes"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 3; }, 10.0));

	AddInfo(TEXT("The publish record is on disk for the next start: the second listing only."));
	const FString RecordPath = UStageMarketplaceSubsystem::GetPublishRecordPath();
	Recorder->Unbind();
	Game.Shutdown();
	FString Record;
	TestTrue(TEXT("The record was written"), FFileHelper::LoadFileToString(Record, *RecordPath));
	TestTrue(TEXT("It lists the listing still published"), Record.Contains(Mine[1].ToString(EGuidFormats::Digits)));
	TestFalse(TEXT("and not the unpublished one"), Record.Contains(Mine[0].ToString(EGuidFormats::Digits)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
