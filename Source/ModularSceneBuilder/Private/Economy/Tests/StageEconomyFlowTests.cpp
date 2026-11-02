// Copyright Epic Games, Inc. All Rights Reserved.

// Integration tests of the live economy (Docs/ADR/0021-community-marketplace.md §5 and §14): the real profile, economy and
// marketplace subsystems in an isolated game instance (FStageTestGameInstance: temporary folders, the real save slot untouched),
// with a commerce backend the test answers for. Covers credit checks, the purchase in flight, refusals, a backend that does not
// answer in time and answers late (approval honoured once, refusal dropped), top-ups, and exact-tag entitlement gates.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Economy; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Economy/StageProductData.h"
#include "Economy/Tests/StageTestCommerceBackend.h"
#include "Engine/GameInstance.h"
#include "GameplayTagsManager.h"
#include "Misc/AutomationTest.h"
#include "Tests/StageTestEventRecorder.h"
#include "Tests/StageTestFixtures.h"
#include "UObject/StrongObjectPtr.h"

namespace StageEconomyFlowTests
{
	/** A recorder bound to Game for the scope; unbound before the game instance goes. */
	struct FScopedRecorder
	{
		TStrongObjectPtr<UStageTestEventRecorder> Recorder;

		explicit FScopedRecorder(FStageTestGameInstance& Game)
			: Recorder(NewObject<UStageTestEventRecorder>())
		{
			Recorder->Bind(Game.Get());
		}
		~FScopedRecorder()
		{
			Recorder->Unbind();
		}
		UStageTestEventRecorder* operator->() const { return Recorder.Get(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageEconomyListingPurchaseFlowTest, "StageCraft.Economy.ListingPurchaseFlow", StageTestFixtures::Flags)

bool FStageEconomyListingPurchaseFlowTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageEconomyFlowTests;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageEconomySubsystem* Economy = Game.Subsystem<UStageEconomySubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	if (!TestNotNull(TEXT("Economy"), Economy) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("The test commerce backend is in use"), Commerce))
	{
		return false;
	}
	FScopedRecorder Recorder(Game);

	const FGuid Rig = PublishListing(TEXT("Flow Rig"), 120, /*bAllowExport*/ false);
	const FGuid Second = PublishListing(TEXT("Flow Second"), 120, /*bAllowExport*/ false);
	TestTrue(TEXT("Two listings published"), Rig.IsValid() && Second.IsValid());
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	AddInfo(TEXT("Not enough credits: refused before the backend is asked, nothing changes."));
	TestEqual(TEXT("Balance set to 100"), SetCredits(Game, 100), int64(100));
	TestEqual(TEXT("Short by 20: InsufficientFunds"), Str(Economy->CanPurchaseListing(Rig).Code), Str(EStageEconomyResult::InsufficientFunds));
	TestEqual(TEXT("The request is refused the same way"), Str(Economy->RequestListingPurchase(Rig).Code), Str(EStageEconomyResult::InsufficientFunds));
	TestEqual(TEXT("The backend was not asked"), Commerce->PurchaseRequests.Num(), 0);
	TestEqual(TEXT("Balance unchanged"), GetCredits(Game), int64(100));

	AddInfo(TEXT("Enough credits, the store answers later: one purchase in flight, a second request is PurchasePending."));
	SetCredits(Game, 500);
	Commerce->Mode = UStageTestCommerceBackend::EMode::Hold;
	TestTrue(TEXT("Accepted and sent to the store"), Economy->RequestListingPurchase(Rig).IsSuccess());
	TestTrue(TEXT("Pending"), Economy->IsListingPurchasePending(Rig));
	TestEqual(TEXT("A second request while pending"), Str(Economy->RequestListingPurchase(Rig).Code), Str(EStageEconomyResult::PurchasePending));
	if (!TestEqual(TEXT("The store saw one request"), Commerce->PurchaseRequests.Num(), 1))
	{
		return false;
	}
	const FStagePurchaseRequest Sent = Commerce->PurchaseRequests[0];
	TestTrue(TEXT("It carries a transaction id"), Sent.TransactionId.IsValid());
	TestTrue(TEXT("It charges the listing's price in credits"), Sent.Price.Num() == 1 && Sent.Price[0].Amount == 120 && Sent.Price[0].Currency == StageCraftTags::Currency_Credits);
	TestEqual(TEXT("Nothing charged before the answer"), GetCredits(Game), int64(500));

	TestTrue(TEXT("The store approves"), Commerce->AnswerHeldPurchase(Sent.TransactionId, true));
	TestEqual(TEXT("Charged exactly once"), GetCredits(Game), int64(380));
	TestTrue(TEXT("Owned"), Profile->OwnsListing(Rig));
	TestFalse(TEXT("No longer pending"), Economy->IsListingPurchasePending(Rig));
	TestTrue(TEXT("The purchase is in the history"), Profile->GetProfile().PurchaseHistory.ContainsByPredicate([&Sent, &Rig](const FStagePurchaseRecord& Record)
	{
		return Record.TransactionId == Sent.TransactionId && Record.ListingId == Rig;
	}));
	TestTrue(TEXT("Completion was broadcast as a success"), Recorder->ListingPurchases.Num() == 1 && Recorder->ListingPurchases[0].Result.IsSuccess());
	TestEqual(TEXT("Owned: buying again is refused"), Str(Economy->CanPurchaseListing(Rig).Code), Str(EStageEconomyResult::AlreadyOwned));

	AddInfo(TEXT("The store refuses: nothing changes, and the refusal is reported with the store's reason."));
	Commerce->Mode = UStageTestCommerceBackend::EMode::DeclineNow;
	Commerce->RefusalReason = FText::FromString(TEXT("Card declined"));
	TestTrue(TEXT("Accepted and sent"), Economy->RequestListingPurchase(Second).IsSuccess());
	TestEqual(TEXT("Balance unchanged"), GetCredits(Game), int64(380));
	TestFalse(TEXT("Not owned"), Profile->OwnsListing(Second));
	if (TestEqual(TEXT("A second completion"), Recorder->ListingPurchases.Num(), 2))
	{
		TestEqual(TEXT("BackendRejected"), Str(Recorder->ListingPurchases[1].Result.Code), Str(EStageEconomyResult::BackendRejected));
		TestEqual(TEXT("with the store's reason"), Recorder->ListingPurchases[1].Result.Message.ToString(), FString(TEXT("Card declined")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageEconomyBackendTimeoutTest, "StageCraft.Economy.BackendTimeout", StageTestFixtures::Flags)

bool FStageEconomyBackendTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;
	using namespace StageEconomyFlowTests;

	// Each timeout and each late answer that can no longer be applied is logged as a warning on purpose.
	AddExpectedMessage(TEXT("waiting for a late answer"), EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessage(TEXT("left for the server to reconcile"), EAutomationExpectedMessageFlags::Contains, 0);
	AddExpectedMessage(TEXT("Ignoring backend answer"), EAutomationExpectedMessageFlags::Contains, 0);

	FStageTestGameInstanceOptions Options;
	Options.BackendTimeoutSeconds = 0.2f;
	FStageTestGameInstance Game(Options);
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageEconomySubsystem* Economy = Game.Subsystem<UStageEconomySubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	if (!TestNotNull(TEXT("Economy"), Economy) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Commerce"), Commerce))
	{
		return false;
	}
	FScopedRecorder Recorder(Game);
	TestEqual(TEXT("The configured timeout is in use"), Economy->GetBackendTimeoutSeconds(), 0.2f);

	const FGuid Late = PublishListing(TEXT("Late Approval"), 100, false);
	const FGuid Refused = PublishListing(TEXT("Late Refusal"), 100, false);
	const FGuid Rebought = PublishListing(TEXT("Bought Again"), 100, false);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	SetCredits(Game, 1000);
	Commerce->Mode = UStageTestCommerceBackend::EMode::Hold;

	AddInfo(TEXT("No answer in time: BackendTimeout, nothing applied, the listing can be requested again."));
	TestTrue(TEXT("Sent"), Economy->RequestListingPurchase(Late).IsSuccess());
	const FGuid LateId = Commerce->PurchaseRequests.Last().TransactionId;
	TestTrue(TEXT("The timeout is reported"), Game.Pump([&Recorder]() { return Recorder->ListingPurchases.Num() == 1; }, 3.0));
	if (Recorder->ListingPurchases.Num() == 1)
	{
		TestEqual(TEXT("as BackendTimeout"), Str(Recorder->ListingPurchases[0].Result.Code), Str(EStageEconomyResult::BackendTimeout));
	}
	TestEqual(TEXT("Nothing charged"), GetCredits(Game), int64(1000));
	TestFalse(TEXT("No longer pending"), Economy->IsListingPurchasePending(Late));
	TestFalse(TEXT("Not owned"), Profile->OwnsListing(Late));
	TestEqual(TEXT("One transaction awaits a late answer"), Economy->GetUnresolvedTransactionCount(), 1);

	AddInfo(TEXT("The store approves late: applied (it may have charged), exactly once."));
	TestTrue(TEXT("Late approval"), Commerce->AnswerHeldPurchase(LateId, true));
	TestEqual(TEXT("Charged now"), GetCredits(Game), int64(900));
	TestTrue(TEXT("Owned now"), Profile->OwnsListing(Late));
	TestEqual(TEXT("Resolved"), Economy->GetUnresolvedTransactionCount(), 0);
	TestTrue(TEXT("The late success is broadcast"), Recorder->ListingPurchases.Num() == 2 && Recorder->ListingPurchases[1].Result.IsSuccess());
	TestTrue(TEXT("The store answers the same transaction again"), Commerce->AnswerAgain(LateId, true));
	TestEqual(TEXT("A second answer charges nothing"), GetCredits(Game), int64(900));
	TestEqual(TEXT("and is not broadcast"), Recorder->ListingPurchases.Num(), 2);

	AddInfo(TEXT("The store refuses late: nothing to undo, nothing broadcast."));
	TestTrue(TEXT("Sent"), Economy->RequestListingPurchase(Refused).IsSuccess());
	const FGuid RefusedId = Commerce->PurchaseRequests.Last().TransactionId;
	TestTrue(TEXT("Timed out"), Game.Pump([&Recorder]() { return Recorder->ListingPurchases.Num() == 3; }, 3.0));
	TestTrue(TEXT("Late refusal"), Commerce->AnswerHeldPurchase(RefusedId, false));
	TestEqual(TEXT("Balance unchanged"), GetCredits(Game), int64(900));
	TestFalse(TEXT("Not owned"), Profile->OwnsListing(Refused));
	TestEqual(TEXT("Not broadcast"), Recorder->ListingPurchases.Num(), 3);
	TestEqual(TEXT("Resolved"), Economy->GetUnresolvedTransactionCount(), 0);

	AddInfo(TEXT("Bought again after a timeout, then the first approval arrives: re-validated, not charged twice."));
	TestTrue(TEXT("First attempt sent"), Economy->RequestListingPurchase(Rebought).IsSuccess());
	const FGuid FirstId = Commerce->PurchaseRequests.Last().TransactionId;
	TestTrue(TEXT("First attempt timed out"), Game.Pump([&Recorder]() { return Recorder->ListingPurchases.Num() == 4; }, 3.0));
	Commerce->Mode = UStageTestCommerceBackend::EMode::ApproveNow;
	TestTrue(TEXT("Second attempt sent and approved"), Economy->RequestListingPurchase(Rebought).IsSuccess());
	TestEqual(TEXT("Charged once"), GetCredits(Game), int64(800));
	TestTrue(TEXT("The first approval arrives late"), Commerce->AnswerHeldPurchase(FirstId, true));
	TestEqual(TEXT("Not charged a second time"), GetCredits(Game), int64(800));
	if (TestEqual(TEXT("The late answer is reported"), Recorder->ListingPurchases.Num(), 6))
	{
		TestEqual(TEXT("as AlreadyOwned (left to the server to refund)"), Str(Recorder->ListingPurchases[5].Result.Code), Str(EStageEconomyResult::AlreadyOwned));
	}

	AddInfo(TEXT("A credit top-up that times out and is granted late."));
	Commerce->bSellsCredits = true;
	Commerce->Mode = UStageTestCommerceBackend::EMode::Hold;
	TestTrue(TEXT("Top-up sent"), Economy->RequestCreditTopUp(StageCraftTags::Currency_Credits, 70).IsSuccess());
	const FGuid TopUpId = Commerce->TopUpRequests.Last().TransactionId;
	TestTrue(TEXT("Top-up timed out"), Game.Pump([&Recorder]() { return Recorder->TopUps.Num() == 1; }, 3.0));
	TestTrue(TEXT("as BackendTimeout with nothing granted"), Recorder->TopUps.Num() == 1 && Recorder->TopUps[0].Result.Code == EStageEconomyResult::BackendTimeout && Recorder->TopUps[0].Granted == 0);
	TestFalse(TEXT("No top-up pending"), Economy->IsCreditTopUpPending());
	TestTrue(TEXT("Granted late"), Commerce->AnswerHeldTopUp(TopUpId, 70));
	TestEqual(TEXT("The credits arrive"), GetCredits(Game), int64(870));

	AddInfo(TEXT("The game instance shuts down with a purchase in flight; the answer afterwards changes nothing."));
	const FGuid Leftover = PublishListing(TEXT("Shutdown"), 100, false);
	TestTrue(TEXT("The query answers"), QueryAll(Game));
	TestTrue(TEXT("Sent"), Economy->RequestListingPurchase(Leftover).IsSuccess());
	const FGuid LeftoverId = Commerce->PurchaseRequests.Last().TransactionId;
	TStrongObjectPtr<UStageTestCommerceBackend> KeepCommerce(Commerce);
	Recorder->Unbind();
	Game.Shutdown();
	TestTrue(TEXT("The answer after shutdown is delivered to nobody"), KeepCommerce->AnswerHeldPurchase(LeftoverId, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageEconomyEntitlementGatesTest, "StageCraft.Economy.EntitlementGates", StageTestFixtures::Flags)

bool FStageEconomyEntitlementGatesTest::RunTest(const FString& Parameters)
{
	using namespace StageTestFixtures;

	FStageTestGameInstance Game;
	if (!TestTrue(TEXT("The test game instance starts"), Game.IsValid()))
	{
		return false;
	}
	UStageEconomySubsystem* Economy = Game.Subsystem<UStageEconomySubsystem>();
	UStageProfileSubsystem* Profile = Game.Subsystem<UStageProfileSubsystem>();
	UStageTestCommerceBackend* Commerce = Game.GetCommerce();
	if (!TestNotNull(TEXT("Economy"), Economy) || !TestNotNull(TEXT("Profile"), Profile) || !TestNotNull(TEXT("Commerce"), Commerce))
	{
		return false;
	}
	TestTrue(TEXT("The product catalog loads"), Game.Pump([Economy]() { return Economy->IsProductCatalogLoaded(); }, 10.0));

	// A paid entitlement is a "subscription tier" in this model: a product grants a tag, content asks for that exact tag.
	const FGameplayTag Wash = FGameplayTag::RequestGameplayTag(TEXT("StageCraft.Entitlement.Item.MovingHeadWash"));
	const FGameplayTag Parent = FGameplayTag::RequestGameplayTag(TEXT("StageCraft.Entitlement.Item"));
	const TArray<UStageProductData*> Granting = Economy->GetProductsGranting(Wash);
	if (!TestTrue(TEXT("A product sells the moving head wash"), Granting.Num() > 0 && Granting[0] != nullptr))
	{
		return false;
	}
	UStageProductData* Product = Granting[0];
	int64 Price = 0;
	for (const FStageCurrencyAmount& Cost : Product->Price)
	{
		if (Cost.Currency == StageCraftTags::Currency_Credits)
		{
			Price += Cost.Amount;
		}
	}

	TestFalse(TEXT("A new profile does not own it"), Profile->HasEntitlement(Wash));
	SetCredits(Game, Price + 10);
	TestEqual(TEXT("It can be bought"), Str(Economy->GetEntitlementState(Wash)), Str(EStageOwnershipState::Purchasable));
	TestEqual(TEXT("Nothing grants the parent tag itself"), Str(Economy->GetEntitlementState(Parent)), Str(EStageOwnershipState::Locked));
	TestEqual(TEXT("An empty tag is free"), Str(Economy->GetEntitlementState(FGameplayTag())), Str(EStageOwnershipState::Free));

	Commerce->Mode = UStageTestCommerceBackend::EMode::ApproveNow;
	TestTrue(TEXT("Bought"), Economy->RequestPurchase(Product).IsSuccess());
	TestEqual(TEXT("Charged the product's price"), GetCredits(Game), int64(10));
	TestTrue(TEXT("The exact tag is owned"), Profile->HasEntitlement(Wash));
	TestEqual(TEXT("Owned"), Str(Economy->GetEntitlementState(Wash)), Str(EStageOwnershipState::Owned));
	TestFalse(TEXT("Owning a child never grants its parent"), Profile->HasEntitlement(Parent));
	TestEqual(TEXT("Buying again"), Str(Economy->CanPurchase(Product).Code), Str(EStageEconomyResult::AlreadyOwned));

	AddInfo(TEXT("Credits cannot be bought from a store that does not sell them (always so offline)."));
	TestEqual(TEXT("Top-up without a store"), Str(Economy->RequestCreditTopUp(StageCraftTags::Currency_Credits, 50).Code), Str(EStageEconomyResult::ServerRequired));
	TestEqual(TEXT("The store was not asked"), Commerce->TopUpRequests.Num(), 0);
	Commerce->bSellsCredits = true;
	TestTrue(TEXT("Top-up from a store that sells credits"), Economy->RequestCreditTopUp(StageCraftTags::Currency_Credits, 50).IsSuccess());
	TestEqual(TEXT("Granted"), GetCredits(Game), int64(60));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
