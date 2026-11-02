// Copyright Epic Games, Inc. All Rights Reserved.

// Integration tests of acquire orders (Docs/ADR/0022-dcc-bridge.md §11.2, ADR 0023): UStageAcquireDispatchSubsystem with the
// live marketplace, economy, bridge and AI tool subsystems of an isolated game instance (FStageTestGameInstance).
//  - OrderFlow: refusals before anything is bought (unknown listing, changed price, no path, rights), one order at a time, a
//    bought-and-opened order, and a bought order whose delivery fails (the credits stay spent and both parts are reported).
//  - OrderTimeout: a store that does not answer ends the order as TimedOut; the purchase still completes when it answers.
//  - VaultBeforeDispatch: the tool's key goes from the vault to the tool and is checked before anything is bought; a refused
//    key, a removed key and a tool that never answers.
// Deliveries that export a .glb need a renderer: with -nullrhi they end as DeliveryFailed (asserted as such); a run with a
// renderer also asserts the file and the hand-off (Scripts/Tests/RunAutomation.ps1 runs both).
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Dispatch+StageCraft.Services; Quit" -unattended

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Economy/Tests/StageTestCommerceBackend.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Marketplace/StageAcquireDispatchSubsystem.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scene/StageSceneStore.h"
#include "Services/StageAIToolSubsystem.h"
#include "Services/StageCredentialVault.h"
#include "Services/Tests/StageTestAIToolProvider.h"
#include "Tests/StageTestEventRecorder.h"
#include "Tests/StageTestFixtures.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

namespace StageDispatchOrderTests
{
	FString Str(EStageOrderResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageDispatchAvailability Availability) { return UEnum::GetValueAsString(Availability); }
	FString Str(EStageAIToolResult Result) { return UEnum::GetValueAsString(Result); }
	FString Str(EStageAIToolKeyState State) { return UEnum::GetValueAsString(State); }

	struct FScopedRecorder
	{
		TStrongObjectPtr<UStageTestEventRecorder> Recorder;
		explicit FScopedRecorder(FStageTestGameInstance& Game) : Recorder(NewObject<UStageTestEventRecorder>()) { Recorder->Bind(Game.Get()); }
		~FScopedRecorder() { Recorder->Unbind(); }
		UStageTestEventRecorder* operator->() const { return Recorder.Get(); }
	};

	FStageAcquireOrder MakeOrder(const UStageMarketplaceSubsystem& Marketplace, const FGuid& ListingId, EStageDispatchKind Kind)
	{
		FStageAcquireOrder Order;
		Order.ListingId = ListingId;
		if (const FStageMarketplaceListing* Listing = Marketplace.FindListing(ListingId))
		{
			Order.ExpectedPrice = Listing->Price;
		}
		Order.Target.Kind = Kind;
		return Order;
	}

	/** True when Path is a binary glTF file ("glTF" magic). */
	bool IsGlb(const FString& Path)
	{
		TArray<uint8> Bytes;
		return FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent) && Bytes.Num() > 12 && FMemory::Memcmp(Bytes.GetData(), "glTF", 4) == 0;
	}

	UStageTestAIToolProvider* FindTestTool(UStageAIToolSubsystem& Tools)
	{
		UStageTestAIToolProvider* Found = nullptr;
		ForEachObjectWithOuter(&Tools, [&Found](UObject* Object)
		{
			Found = Found ? Found : Cast<UStageTestAIToolProvider>(Object);
		}, EGetObjectsFlags::None);
		return Found;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDispatchOrderFlowTest, "StageCraft.Dispatch.OrderFlow", StageTestFixtures::Flags)

bool FStageDispatchOrderFlowTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageDispatchOrderTests;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageAcquireDispatchSubsystem* Dispatch = Game.Subsystem<UStageAcquireDispatchSubsystem>();
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	if (!TestNotNull(TEXT("Dispatch"), Dispatch) || !TestNotNull(TEXT("Marketplace"), Marketplace) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Commerce"), Commerce))
	{
		return false;
	}
	FScopedRecorder Recorder(Game);

	const FGuid Closed = PublishListing(TEXT("Kept Inside"), 100, /*bAllowExport*/ false);
	const FGuid Open = PublishListing(TEXT("Exportable"), 150, /*bAllowExport*/ true);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	SetCredits(Game, 1000);

	AddInfo(TEXT("Refused before anything is bought."));
	FStageAcquireOrder Unknown = MakeOrder(*Marketplace, FGuid::NewGuid(), EStageDispatchKind::OpenInStageCraft);
	TestEqual(TEXT("An unknown listing"), Str(Dispatch->RequestOrder(Unknown).Code), Str(EStageOrderResult::NotFound));

	FStageAcquireOrder Cheaper = MakeOrder(*Marketplace, Closed, EStageDispatchKind::OpenInStageCraft);
	Cheaper.ExpectedPrice.Credits = 99;
	TestEqual(TEXT("A price that is not the one confirmed"), Str(Dispatch->RequestOrder(Cheaper).Code), Str(EStageOrderResult::PriceChanged));
	FStageAcquireOrder RealMoney = MakeOrder(*Marketplace, Closed, EStageDispatchKind::OpenInStageCraft);
	RealMoney.ExpectedPrice.RealMoneyMinorUnits = 499;
	RealMoney.ExpectedPrice.RealMoneyCurrency = TEXT("USD");
	TestEqual(TEXT("A real-money part the listing does not have"), Str(Dispatch->RequestOrder(RealMoney).Code), Str(EStageOrderResult::PriceChanged));

	FStageAcquireOrder NoPath = MakeOrder(*Marketplace, Open, EStageDispatchKind::SaveFile);
	TestEqual(TEXT("A .glb target without a path"), Str(Dispatch->RequestOrder(NoPath).Code), Str(EStageOrderResult::InvalidRequest));

	FStageAcquireOrder ClosedFile = MakeOrder(*Marketplace, Closed, EStageDispatchKind::SaveFile);
	ClosedFile.Target.FilePath = FPaths::Combine(Game.GetRoot(), TEXT("Closed.glb"));
	const FStageOrderResultInfo Rights = Dispatch->RequestOrder(ClosedFile);
	TestEqual(TEXT("A stage the publisher keeps inside, to a file"), Str(Rights.Code), Str(EStageOrderResult::TargetUnavailable));
	TestEqual(TEXT("because of the publisher's rights"), Str(Rights.Availability), Str(EStageDispatchAvailability::NeedsExportRights));
	TestEqual(TEXT("Nothing was asked of the store"), Commerce->PurchaseRequests.Num(), 0);
	TestEqual(TEXT("Nothing was charged"), GetCredits(Game), int64(1000));

	AddInfo(TEXT("Bought and opened; one order at a time."));
	Commerce->Mode = UStageTestCommerceBackend::EMode::Hold;
	TestTrue(TEXT("The order starts"), Dispatch->RequestOrder(MakeOrder(*Marketplace, Closed, EStageDispatchKind::OpenInStageCraft)).IsSuccess());
	TestTrue(TEXT("It is pending"), Dispatch->IsOrderPending());
	TestEqual(TEXT("A second order meanwhile"), Str(Dispatch->RequestOrder(MakeOrder(*Marketplace, Open, EStageDispatchKind::OpenInStageCraft)).Code), Str(EStageOrderResult::Busy));
	TestTrue(TEXT("The store approves"), Commerce->PurchaseRequests.Num() == 1 && Commerce->AnswerHeldPurchase(Commerce->PurchaseRequests[0].TransactionId, true));
	TestTrue(TEXT("The order ends"), Game.Pump([&Recorder]() { return Recorder->Orders.Num() == 1; }, 10.0));
	if (Recorder->Orders.Num() == 1)
	{
		const FStageOrderResultInfo& Done = Recorder->Orders[0];
		TestEqual(TEXT("in success"), Str(Done.Code), Str(EStageOrderResult::Success));
		TestTrue(TEXT("with the imported scene"), FStageSceneStore(FStageSceneStore::GetDefaultDirectory()).SceneExists(Done.SceneName));
	}
	TestFalse(TEXT("Not pending any more"), Dispatch->IsOrderPending());
	TestEqual(TEXT("Charged the price"), GetCredits(Game), int64(900));
	TestTrue(TEXT("Owned"), Profile->OwnsListing(Closed));
	TestTrue(TEXT("The package is kept in the library (it decides export rights)"),
		IFileManager::Get().FileExists(*FPaths::Combine(UStageMarketplaceSubsystem::GetLibraryDirectory(), Closed.ToString(EGuidFormats::Digits) + StagePackage::FileExtension)));

	AddInfo(TEXT("Bought, then delivered to a .glb file (or, without a renderer, bought and not delivered)."));
	Commerce->Mode = UStageTestCommerceBackend::EMode::ApproveNow;
	FStageAcquireOrder ToFile = MakeOrder(*Marketplace, Open, EStageDispatchKind::SaveFile);
	ToFile.Target.FilePath = FPaths::Combine(Game.GetRoot(), TEXT("Exports"), TEXT("Exportable.glb"));
	TestTrue(TEXT("The order starts"), Dispatch->RequestOrder(ToFile).IsSuccess());
	TestTrue(TEXT("The order ends"), Game.Pump([&Recorder]() { return Recorder->Orders.Num() == 2; }, 30.0));
	TestEqual(TEXT("Charged, whatever happens to the delivery"), GetCredits(Game), int64(750));
	TestTrue(TEXT("Owned"), Profile->OwnsListing(Open));
	if (Recorder->Orders.Num() == 2)
	{
		const FStageOrderResultInfo& Done = Recorder->Orders[1];
		TestTrue(TEXT("The stage was acquired"), Done.WasAcquired());
		if (FApp::CanEverRender())
		{
			TestEqual(TEXT("and saved"), Str(Done.Code), Str(EStageOrderResult::Success));
			TestTrue(TEXT("as a binary glTF file"), IsGlb(ToFile.Target.FilePath));
		}
		else
		{
			TestEqual(TEXT("and not delivered (no renderer)"), Str(Done.Code), Str(EStageOrderResult::DeliveryFailed));
			TestTrue(TEXT("The message says both: in your scenes, and why not delivered"), Done.Message.ToString().Contains(Done.SceneName) && Done.Message.ToString().Contains(TEXT("renderer")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageDispatchOrderTimeoutTest, "StageCraft.Dispatch.OrderTimeout", StageTestFixtures::Flags)

bool FStageDispatchOrderTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageDispatchOrderTests;

	FStageTestGameInstanceOptions Options;
	Options.OrderTimeoutSeconds = 0.3f;
	FStageTestGameInstance Game(Options);
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageAcquireDispatchSubsystem* Dispatch = Game.Subsystem<UStageAcquireDispatchSubsystem>();
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	if (!TestNotNull(TEXT("Dispatch"), Dispatch) || !TestNotNull(TEXT("Marketplace"), Marketplace) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Commerce"), Commerce))
	{
		return false;
	}
	FScopedRecorder Recorder(Game);
	const FGuid Slow = PublishListing(TEXT("Slow Store"), 100, false);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	SetCredits(Game, 500);

	Commerce->Mode = UStageTestCommerceBackend::EMode::Hold;
	TestTrue(TEXT("The order starts"), Dispatch->RequestOrder(MakeOrder(*Marketplace, Slow, EStageDispatchKind::OpenInStageCraft)).IsSuccess());
	TestTrue(TEXT("The order gives up"), Game.Pump([&Recorder]() { return Recorder->Orders.Num() == 1; }, 3.0));
	if (Recorder->Orders.Num() == 1)
	{
		TestEqual(TEXT("as TimedOut"), Str(Recorder->Orders[0].Code), Str(EStageOrderResult::TimedOut));
		TestFalse(TEXT("before anything was acquired"), Recorder->Orders[0].WasAcquired());
	}
	TestFalse(TEXT("The dispatcher is free again"), Dispatch->IsOrderPending());
	TestEqual(TEXT("Nothing charged yet"), GetCredits(Game), int64(500));

	AddInfo(TEXT("The store answers after the order gave up: the purchase completes and the stage arrives in the scenes."));
	TestTrue(TEXT("The store approves"), Commerce->PurchaseRequests.Num() == 1 && Commerce->AnswerHeldPurchase(Commerce->PurchaseRequests[0].TransactionId, true));
	TestTrue(TEXT("The acquire completes"), Game.Pump([&Recorder]() { return Recorder->Acquires.Num() == 1; }, 10.0));
	TestTrue(TEXT("with the scene imported"), Recorder->Acquires.Num() == 1 && Recorder->Acquires[0].Result.IsSuccess() && !Recorder->Acquires[0].SceneName.IsEmpty());
	TestTrue(TEXT("Owned"), Profile->OwnsListing(Slow));
	TestEqual(TEXT("Charged once"), GetCredits(Game), int64(400));
	TestEqual(TEXT("No second order result"), Recorder->Orders.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageServicesVaultBeforeDispatchTest, "StageCraft.Services.VaultBeforeDispatch", StageTestFixtures::Flags)

bool FStageServicesVaultBeforeDispatchTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageDispatchOrderTests;

	AddExpectedMessage(TEXT("did not answer within"), EAutomationExpectedMessageFlags::Contains, 0);

	FStageTestGameInstanceOptions Options;
	Options.ToolTimeoutSeconds = 0.3f;
	FStageTestGameInstance Game(Options);
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageAIToolSubsystem* Tools = Game.Subsystem<UStageAIToolSubsystem>();
	UStageAcquireDispatchSubsystem* Dispatch = Game.Subsystem<UStageAcquireDispatchSubsystem>();
	UStageMarketplaceSubsystem* Marketplace = Game.Subsystem<UStageMarketplaceSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	if (!TestNotNull(TEXT("Tools"), Tools) || !TestNotNull(TEXT("Dispatch"), Dispatch) || !TestNotNull(TEXT("Marketplace"), Marketplace) || !TestNotNull(TEXT("Commerce"), Commerce))
	{
		return false;
	}
	Tools->RegisterProviderForDevelopment(UStageTestAIToolProvider::StaticClass());
	UStageTestAIToolProvider* Tool = FindTestTool(*Tools);
	if (!TestNotNull(TEXT("The test tool is registered"), Tool))
	{
		return false;
	}
	FScopedRecorder Recorder(Game);
	const FName Id = UStageTestAIToolProvider::ProviderId;
	FStageAIToolInfo Info;
	TestTrue(TEXT("Listed"), Tools->FindTool(Id, Info));
	TestEqual(TEXT("with no key"), Str(Info.KeyState), Str(EStageAIToolKeyState::Missing));

	const FString ModelPath = FPaths::Combine(Game.GetRoot(), TEXT("Model.glb"));
	FFileHelper::SaveStringToFile(TEXT("glTF test model"), *ModelPath);
	FStageAIToolJob Job;
	Job.ModelPath = ModelPath;
	Job.Name = TEXT("Model");

	AddInfo(TEXT("No key: refused by the subsystem; the tool is never called."));
	TestEqual(TEXT("Hand-off without a key"), Str(Tools->SubmitModel(Id, Job).Code), Str(EStageAIToolResult::NeedsKey));
	TestEqual(TEXT("The tool was not called"), Tool->SubmitCount, 0);

	AddInfo(TEXT("A key typed with spaces is stored trimmed and encrypted, and reaches only the tool."));
	TestTrue(TEXT("Stored"), Tools->StoreKey(Id, TEXT("  mock-secret-1  ")).IsSuccess());
	TestTrue(TEXT("Listed"), Tools->FindTool(Id, Info));
	TestEqual(TEXT("as saved"), Str(Info.KeyState), Str(EStageAIToolKeyState::Saved));
	const FString KeyFile = FPaths::Combine(FStageCredentialVault::GetDefaultDirectory(), Id.ToString() + TEXT(".key"));
	TestTrue(TEXT("The vault file is in the test's own folder"), KeyFile.StartsWith(Game.GetRoot()) && IFileManager::Get().FileExists(*KeyFile));
	FString Stored;
	FFileHelper::LoadFileToString(Stored, *KeyFile);
	TestFalse(TEXT("The file does not hold the key in plain text"), Stored.Contains(TEXT("mock-secret-1")));
	TestTrue(TEXT("Handed to the tool"), Tools->SubmitModel(Id, Job).IsSuccess());
	TestEqual(TEXT("The tool got the key from the vault, trimmed"), Tool->LastKey, FString(TEXT("mock-secret-1")));
	TestEqual(TEXT("and the model"), Tool->LastJob.ModelPath, ModelPath);
	TestTrue(TEXT("The hand-off is reported"), Recorder->ToolJobs.Num() == 1 && Recorder->ToolJobs[0].IsSuccess());

	AddInfo(TEXT("The tool refuses the key: remembered, and the next hand-off is refused without calling the tool."));
	Tool->Mode = UStageTestAIToolProvider::EMode::RejectKey;
	TestTrue(TEXT("Started"), Tools->SubmitModel(Id, Job).IsSuccess());
	TestTrue(TEXT("Refused by the tool"), Recorder->ToolJobs.Num() == 2 && Recorder->ToolJobs[1].Code == EStageAIToolResult::KeyRejected);
	TestTrue(TEXT("Listed"), Tools->FindTool(Id, Info));
	TestEqual(TEXT("as rejected"), Str(Info.KeyState), Str(EStageAIToolKeyState::Rejected));
	const int32 CallsBefore = Tool->SubmitCount;
	TestEqual(TEXT("A hand-off with the refused key"), Str(Tools->SubmitModel(Id, Job).Code), Str(EStageAIToolResult::KeyRejected));
	TestEqual(TEXT("did not call the tool"), Tool->SubmitCount, CallsBefore);

	AddInfo(TEXT("A tool that never answers: the hand-off times out, the tool is cancelled, the subsystem is free again."));
	TestTrue(TEXT("A new key"), Tools->StoreKey(Id, TEXT("mock-secret-2")).IsSuccess());
	Tool->Mode = UStageTestAIToolProvider::EMode::Hold;
	TestTrue(TEXT("Started"), Tools->SubmitModel(Id, Job).IsSuccess());
	TestTrue(TEXT("Busy meanwhile"), Tools->IsBusy());
	TestTrue(TEXT("Ends by timeout"), Game.Pump([&Recorder]() { return Recorder->ToolJobs.Num() == 3; }, 3.0));
	TestTrue(TEXT("as TimedOut"), Recorder->ToolJobs.Num() == 3 && Recorder->ToolJobs[2].Code == EStageAIToolResult::TimedOut);
	TestFalse(TEXT("Free again"), Tools->IsBusy());
	TestTrue(TEXT("The tool was told to cancel"), Tool->CancelCount > 0);

	AddInfo(TEXT("Through the dispatcher: the key is checked before anything is bought."));
	const FGuid Open = PublishListing(TEXT("For The Tool"), 100, /*bAllowExport*/ true);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	SetCredits(Game, 1000);
	TestTrue(TEXT("Key removed"), Tools->RemoveKey(Id).IsSuccess());
	TestFalse(TEXT("The vault file is gone"), IFileManager::Get().FileExists(*KeyFile));
	FStageAcquireOrder ToTool;
	ToTool.ListingId = Open;
	ToTool.ExpectedPrice = Marketplace->FindListing(Open) ? Marketplace->FindListing(Open)->Price : FStageMarketplacePrice();
	ToTool.Target.Kind = EStageDispatchKind::AITool;
	ToTool.Target.ProviderId = Id;
	const FStageOrderResultInfo NoKey = Dispatch->RequestOrder(ToTool);
	TestEqual(TEXT("An order to a tool without a key"), Str(NoKey.Code), Str(EStageOrderResult::TargetUnavailable));
	TestEqual(TEXT("needs the key"), Str(NoKey.Availability), Str(EStageDispatchAvailability::NeedsKey));
	TestEqual(TEXT("The store was not asked"), Commerce->PurchaseRequests.Num(), 0);
	TestEqual(TEXT("Nothing charged"), GetCredits(Game), int64(1000));

	TestTrue(TEXT("Key stored again"), Tools->StoreKey(Id, TEXT("mock-secret-3")).IsSuccess());
	Tool->Mode = UStageTestAIToolProvider::EMode::AnswerNow;
	const int32 SubmitsBefore = Tool->SubmitCount;
	TestTrue(TEXT("The order starts"), Dispatch->RequestOrder(ToTool).IsSuccess());
	TestTrue(TEXT("The order ends"), Game.Pump([&Recorder]() { return Recorder->Orders.Num() == 1; }, 30.0));
	TestEqual(TEXT("Charged"), GetCredits(Game), int64(900));
	if (Recorder->Orders.Num() == 1 && FApp::CanEverRender())
	{
		TestEqual(TEXT("Handed to the tool"), Str(Recorder->Orders[0].Code), Str(EStageOrderResult::Success));
		TestEqual(TEXT("with the vault's key"), Tool->LastKey, FString(TEXT("mock-secret-3")));
		TestFalse(TEXT("The outbox copy is removed after the hand-off"), IFileManager::Get().FileExists(*Tool->LastJob.ModelPath));
	}
	else if (Recorder->Orders.Num() == 1)
	{
		TestEqual(TEXT("Acquired, not handed over (no renderer to export)"), Str(Recorder->Orders[0].Code), Str(EStageOrderResult::DeliveryFailed));
		TestEqual(TEXT("The tool was not called"), Tool->SubmitCount, SubmitsBefore);
	}
	FStageCredentialVault::WipeString(Tool->LastKey);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
