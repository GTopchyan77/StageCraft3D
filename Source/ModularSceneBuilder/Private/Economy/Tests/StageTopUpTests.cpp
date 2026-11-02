// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the currency top-up checks (Docs/ADR/0022-dcc-bridge.md §11.4). Pure rules: no profile save slot is touched.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Economy; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Economy/StageCommerceBackend.h"
#include "Economy/StageTopUpRules.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

namespace StageTopUpTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FString Str(EStageEconomyResult Result) { return StaticEnum<EStageEconomyResult>()->GetNameStringByValue(static_cast<int64>(Result)); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageTopUpRulesTest, "StageCraft.Economy.TopUp", StageTopUpTests::Flags)

bool FStageTopUpRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageTopUpTests;

	FStagePlayerProfile Profile;
	Profile.Balances.Add(StageCraftTags::Currency_Credits, 300);

	FStageTopUpContext Context;
	Context.Profile = &Profile;
	Context.bIntegrityValid = true;
	Context.bCanSellCredits = true;
	Context.Currency = StageCraftTags::Currency_Credits;
	Context.Amount = 500;
	Context.MaxAmount = 1000;
	const auto Check = [&Context]() { return Str(StageTopUpRules::Validate(Context).Code); };

	TestEqual(TEXT("A store that sells credits"), Check(), Str(EStageEconomyResult::Success));

	Context.bCanSellCredits = false;
	TestEqual(TEXT("Offline: buying credits needs a server"), Check(), Str(EStageEconomyResult::ServerRequired));
	Context.bCanSellCredits = true;

	Context.bPending = true;
	TestEqual(TEXT("One top-up at a time"), Check(), Str(EStageEconomyResult::PurchasePending));
	Context.bPending = false;

	Context.bIntegrityValid = false;
	TestEqual(TEXT("Frozen profile"), Check(), Str(EStageEconomyResult::IntegrityViolation));
	Context.bIntegrityValid = true;

	Context.Amount = 0;
	TestEqual(TEXT("Zero"), Check(), Str(EStageEconomyResult::InvalidRequest));
	Context.Amount = -50;
	TestEqual(TEXT("Negative"), Check(), Str(EStageEconomyResult::InvalidRequest));
	Context.Amount = 1001;
	TestEqual(TEXT("Over the limit"), Check(), Str(EStageEconomyResult::InvalidRequest));
	Context.Amount = 500;

	Profile.Balances.Add(StageCraftTags::Currency_Credits, MAX_int64 - 100);
	TestEqual(TEXT("Would overflow the balance"), Check(), Str(EStageEconomyResult::InvalidRequest));
	Profile.Balances.Add(StageCraftTags::Currency_Credits, 300);

	Context.Currency = StageCraftTags::Currency;
	TestEqual(TEXT("The currency root is not a currency"), Check(), Str(EStageEconomyResult::InvalidRequest));
	Context.Currency = StageCraftTags::Entitlement;
	TestEqual(TEXT("Not a currency at all"), Check(), Str(EStageEconomyResult::InvalidRequest));
	Context.Currency = StageCraftTags::Currency_Credits;

	Context.Profile = nullptr;
	TestEqual(TEXT("No profile (deny by default)"), Check(), Str(EStageEconomyResult::InvalidRequest));

	// The offline backend sells nothing and, asked anyway, declines.
	UStageCommerceBackend* Local = NewObject<UStageLocalCommerceBackend>(GetTransientPackage());
	TestFalse(TEXT("The local backend does not sell credits"), Local->CanSellCredits());
	TestFalse(TEXT("... nor settle real money"), Local->CanSettleRealMoney());
	int64 Granted = -1;
	int32 Answers = 0;
	Local->ProcessCreditTopUp(FStageTopUpRequest(), FOnStageTopUpProcessed::CreateLambda([&Granted, &Answers](int64 Amount, const FText&)
	{
		Granted = Amount;
		++Answers;
	}));
	TestEqual(TEXT("Declined"), Granted, int64(0));
	TestEqual(TEXT("Answered exactly once"), Answers, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
