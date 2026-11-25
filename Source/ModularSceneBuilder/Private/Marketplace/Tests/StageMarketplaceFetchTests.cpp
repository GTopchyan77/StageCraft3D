// Copyright Epic Games, Inc. All Rights Reserved.

// Integration test of the marketplace download deadline (Docs/ADR/0021-community-marketplace.md §14) on an isolated game instance
// (FStageTestGameInstance) with the live economy and marketplace subsystems.
//  - FetchTimeout: a package that does not arrive in time ends the acquire as TimedOut with the purchase kept; the late package
//    is dropped; Get again downloads it without a second charge; a game instance stopped while a download is awaited shuts down
//    cleanly.
//  - UnpublishFlow (ADR 0021 §15): only listings this install published can be unpublished; removing a library copy keeps
//    ownership, takes the export rights with it until the listing is got again, and waits while the listing is being acquired.
//  - PublishRecordFile (audit R8): a damaged publish record is kept as .bak; an unreadable one is not written over that session.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Marketplace.FetchTimeout+StageCraft.Marketplace.UnpublishFlow+StageCraft.Marketplace.PublishRecordFile; Quit" -unattended

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Economy/Tests/StageTestCommerceBackend.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Marketplace/StageLocalMarketplaceStore.h"
#include "Marketplace/Tests/StageTestMarketplaceBackend.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
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

// The publish record is the only proof of which listings this install may unpublish, and every publish rewrites it from memory
// (audit risk R8, STATE.md #103): a damaged record is kept as .bak, and one that cannot be read is never written over that session.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageMarketplacePublishRecordFileTest, "StageCraft.Marketplace.PublishRecordFile", StageTestFixtures::Flags)

bool FStageMarketplacePublishRecordFileTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;

	AddExpectedMessage(TEXT("was damaged; it was kept as"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessage(TEXT("cannot be read; it is not written this session"), EAutomationExpectedMessageFlags::Contains, 1);

	FStageTestGameInstanceOptions Options;
	Options.Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("StagePublishRecord"), FGuid::NewGuid().ToString()));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Options.Root, /*RequireExists*/ false, /*Tree*/ true); };
	const FString RecordPath = FPaths::Combine(Options.Root, TEXT("Marketplace"), TEXT("Published.json"));
	const auto ReadBack = [](const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	};
	// Publishes one scene and returns its listing (invalid on failure).
	const auto PublishOne = [this](FStageTestGameInstance& Game, const TCHAR* Name)
	{
		UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
		TStrongObjectPtr<UStageTestEventRecorder> Recorder(NewObject<UStageTestEventRecorder>());
		Recorder->Bind(Game.Get());
		ON_SCOPE_EXIT { Recorder->Unbind(); };
		FStageMarketplacePublishRequest Request;
		Request.SceneName = Name;
		Request.Title = Name;
		Request.Category = StageCraftTags::Marketplace_Category_Concert;
		if (!Marketplace || FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).Save(MakeScene(Name)) != EStageSceneResult::Success
			|| !Marketplace->RequestPublish(Request).IsSuccess()
			|| !Game.Pump([&Recorder]() { return Recorder->Publishes.Num() > 0; }, 10.0)
			|| !Recorder->Publishes.Last().Result.IsSuccess())
		{
			AddError(FString::Printf(TEXT("%s was not published"), Name));
			return FGuid();
		}
		return Recorder->Publishes.Last().ListingId;
	};

	AddInfo(TEXT("A damaged record is kept as .bak, and the next publish starts a new one."));
	const FString Damaged = TEXT("{\"version\":1,\"listings\":[\"0A1B2C3D");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(RecordPath), /*Tree*/ true);
	FFileHelper::SaveStringToFile(Damaged, *RecordPath);
	FGuid First;
	{
		FStageTestGameInstance Game(Options);
		if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
		{
			return false;
		}
		TestEqual(TEXT("The record is where the test put it"), UStageMarketplaceSubsystem::GetPublishRecordPath(), RecordPath);
		TestFalse(TEXT("The damaged record left the name the next publish writes"), IFileManager::Get().FileExists(*RecordPath));
		TestEqual(TEXT("It is kept byte for byte as .bak"), ReadBack(RecordPath + TEXT(".bak")), Damaged);
		First = PublishOne(Game, TEXT("Record Arena"));
		Game.Shutdown();
		TestTrue(TEXT("The new record lists the new listing"), First.IsValid() && ReadBack(RecordPath).Contains(First.ToString(EGuidFormats::Digits)));
		TestEqual(TEXT("The .bak is untouched"), ReadBack(RecordPath + TEXT(".bak")), Damaged);
	}

	AddInfo(TEXT("A record that cannot be read (held open by another program) is not written over in that session."));
	const FString UserRecord = ReadBack(RecordPath);
	{
		// Locked while the marketplace starts (it reads the record in Initialize), then released.
		TUniquePtr<IFileHandle> Lock(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*RecordPath, /*bAppend*/ true, /*bAllowRead*/ false));
		if (!TestNotNull(TEXT("The record can be locked"), Lock.Get()))
		{
			return false;
		}
		FStageTestGameInstance Game(Options);
		Lock.Reset();
		if (!TestTrue(TEXT("The second game instance starts"), Game.IsValid()))
		{
			return false;
		}
		UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
		TestFalse(TEXT("The earlier listing is not known as this install's"), Marketplace && Marketplace->IsPublishedByMe(First));
		// The lock is gone: a write now would succeed, so only the session's read-only state keeps the user's record.
		const FGuid Second = PublishOne(Game, TEXT("Record Club"));
		TestTrue(TEXT("Publishing still works"), Second.IsValid() && Marketplace && Marketplace->IsPublishedByMe(Second));
		Game.Shutdown();
		TestEqual(TEXT("The user's record is unchanged"), ReadBack(RecordPath), UserRecord);
	}

	AddInfo(TEXT("The next session reads it again."));
	{
		FStageTestGameInstance Game(Options);
		UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
		TestTrue(TEXT("The earlier listing is this install's again"), Game.IsValid() && Marketplace && Marketplace->IsPublishedByMe(First));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
