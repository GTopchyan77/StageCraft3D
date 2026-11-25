// Copyright Epic Games, Inc. All Rights Reserved.

// Tests for the fixture shadow budget (Docs/ADR/0018 §4): the brightest fixtures nearest the viewer cast, dark ones never do,
// current casters keep their place against slightly brighter newcomers, ties are deterministic, and the budget can be off.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show.ShadowBudget; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Show/StageShadowBudget.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShadowBudgetTest, "StageCraft.Show.ShadowBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageShadowBudgetTest::RunTest(const FString& Parameters)
{
	const auto Make = [](std::initializer_list<float> Importances)
	{
		TArray<FStageShadowCandidate> Candidates;
		for (const float Importance : Importances)
		{
			Candidates.Add({ Importance, false });
		}
		return Candidates;
	};
	const auto Count = [](const TArray<bool>& Casting) { return Casting.FilterByPredicate([](bool bCasting) { return bCasting; }).Num(); };

	// The brightest within the budget.
	TArray<FStageShadowCandidate> Candidates = Make({ 1.f, 5.f, 3.f, 4.f });
	TArray<bool> Casting = StageShadowBudget::SelectCasters(Candidates, { 2 }, 0.f);
	TestTrue(TEXT("two brightest cast"), Casting[1] && Casting[3] && !Casting[0] && !Casting[2]);

	// Dark fixtures never cast, even under the budget; invalid importances count as dark.
	Candidates = Make({ 0.f, 2.f, std::numeric_limits<float>::quiet_NaN(), -1.f });
	Casting = StageShadowBudget::SelectCasters(Candidates, { 10 }, 0.f);
	TestEqual(TEXT("only the lit one casts"), Count(Casting), 1);
	TestTrue(TEXT("... index 1"), Casting[1]);

	// Budget 0: nobody; budget off (-1): every lit fixture.
	TestEqual(TEXT("budget 0"), Count(StageShadowBudget::SelectCasters(Make({ 1.f, 2.f }), { 0 }, 0.f)), 0);
	TestEqual(TEXT("budget off"), Count(StageShadowBudget::SelectCasters(Make({ 1.f, 2.f, 0.f }), { -1 }, 0.f)), 2);

	// Hysteresis: a caster keeps its place against a newcomer up to 25 % brighter, not against a much brighter one.
	Candidates = Make({ 1.f, 1.2f });
	Candidates[0].bCasting = true;
	TestTrue(TEXT("caster keeps its place"), StageShadowBudget::SelectCasters(Candidates, { 1 }, 0.25f)[0]);
	Candidates[1].Importance = 2.f;
	TestTrue(TEXT("much brighter newcomer takes it"), StageShadowBudget::SelectCasters(Candidates, { 1 }, 0.25f)[1]);

	// Ties keep the lower index, whatever the order of evaluation.
	Casting = StageShadowBudget::SelectCasters(Make({ 2.f, 2.f, 2.f }), { 2 }, 0.f);
	TestTrue(TEXT("ties keep registration order"), Casting[0] && Casting[1] && !Casting[2]);
	TestEqual(TEXT("empty input"), StageShadowBudget::SelectCasters({}, { 4 }, 0.f).Num(), 0);

	// Importance: inverse square in metres, closer than a metre counts as a metre.
	TestTrue(TEXT("10 m away"), FMath::IsNearlyEqual(StageShadowBudget::GetImportance(10000.f, 1000.0), 100.f));
	TestTrue(TEXT("closer than 1 m"), FMath::IsNearlyEqual(StageShadowBudget::GetImportance(10000.f, 10.0), 10000.f));
	TestEqual(TEXT("dark"), StageShadowBudget::GetImportance(0.f, 500.0), 0.f);
	TestEqual(TEXT("infinite distance"), StageShadowBudget::GetImportance(100.f, std::numeric_limits<double>::infinity()), 0.f);
	TestTrue(TEXT("nearer is more important"), StageShadowBudget::GetImportance(1000.f, 500.0) > StageShadowBudget::GetImportance(1000.f, 2000.0));

	// Project settings: clamped, -1 when off.
	const UStageShowPerformanceSettings* Settings = GetDefault<UStageShowPerformanceSettings>();
	TestTrue(TEXT("budget in range"), Settings->GetShadowBudget() >= -1 && Settings->GetShadowBudget() <= 2000);
	TestTrue(TEXT("interval in range"), Settings->GetUpdateSeconds() >= 0.05f && Settings->GetUpdateSeconds() <= 2.f);
	TestTrue(TEXT("hysteresis in range"), Settings->GetHysteresis() >= 0.f && Settings->GetHysteresis() <= 2.f);
	TestTrue(TEXT("hold in range"), Settings->GetMinHoldSeconds() >= 0.f && Settings->GetMinHoldSeconds() <= 10.f);
	TestTrue(TEXT("moving budget in range"), Settings->GetMovingShadowBudget() >= -1 && Settings->GetMovingShadowBudget() <= 2000);
	TestTrue(TEXT("moving budget off with the budget"), (Settings->GetShadowBudget() < 0) == (Settings->GetMovingShadowBudget() < 0));
	TestTrue(TEXT("moving seconds in range"), Settings->GetMovingSeconds() >= 0.1f && Settings->GetMovingSeconds() <= 10.f);
	TestEqual(TEXT("limits = both budgets"), Settings->GetShadowLimits().Moving, Settings->GetMovingShadowBudget());
	return true;
}

// The moving-caster limit (STATE.md #108): a moving shadowed beam re-renders its shadows every frame, so only Limits.Moving of the
// casters may be moving; the places they cannot take go to still fixtures further down.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShadowBudgetMovingTest, "StageCraft.Show.ShadowBudgetMoving",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageShadowBudgetMovingTest::RunTest(const FString& Parameters)
{
	using StageShadowBudget::SelectCasters;
	using StageShadowBudget::SelectCastersWithHold;
	const auto Count = [](const TArray<bool>& Casting) { return Casting.FilterByPredicate([](bool bCasting) { return bCasting; }).Num(); };
	const auto Make = [](std::initializer_list<TPair<float, bool>> ImportanceAndMoving)
	{
		TArray<FStageShadowCandidate> Candidates;
		for (const TPair<float, bool>& Entry : ImportanceAndMoving)
		{
			FStageShadowCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Importance = Entry.Key;
			Candidate.bMoving = Entry.Value;
		}
		return Candidates;
	};

	// The three brightest move: only the brightest of them casts; the three still ones fill the rest of the budget.
	TArray<FStageShadowCandidate> Candidates = Make({ { 6.f, true }, { 5.f, true }, { 4.f, true }, { 3.f, false }, { 2.f, false }, { 1.f, false } });
	TArray<bool> Casting = SelectCasters(Candidates, { 4, 1 }, 0.f);
	TestTrue(TEXT("one mover, three still"), Casting == TArray<bool>({ true, false, false, true, true, true }));

	// Moving limit 0: no moving fixture casts, still ones do. A moving limit above the total is the total.
	TestTrue(TEXT("moving limit 0"), SelectCasters(Candidates, { 4, 0 }, 0.f) == TArray<bool>({ false, false, false, true, true, true }));
	Candidates = Make({ { 3.f, true }, { 2.f, true }, { 1.f, true } });
	TestEqual(TEXT("moving limit above the total"), Count(SelectCasters(Candidates, { 2, 5 }, 0.f)), 2);
	// No moving limit: the total alone, as before #108. Budget off: every lit fixture, moving or not.
	TestEqual(TEXT("no moving limit"), Count(SelectCasters(Candidates, { 2, -1 }, 0.f)), 2);
	TestEqual(TEXT("budget off ignores the moving limit"), Count(SelectCasters(Candidates, { -1, 0 }, 0.f)), 3);

	// Nothing moving: the moving limit changes nothing.
	Candidates = Make({ { 1.f, false }, { 4.f, false }, { 2.f, false }, { 3.f, false } });
	TestTrue(TEXT("no movers = total only"), SelectCasters(Candidates, { 2, 0 }, 0.25f) == SelectCasters(Candidates, { 2, -1 }, 0.25f));

	// A head stops: once it counts as still it may cast again under the wider budget.
	Candidates = Make({ { 3.f, true }, { 2.f, true }, { 1.f, true } });
	TestEqual(TEXT("while moving, one casts"), Count(SelectCasters(Candidates, { 3, 1 }, 0.f)), 1);
	for (FStageShadowCandidate& Candidate : Candidates)
	{
		Candidate.bMoving = false;
	}
	TestEqual(TEXT("once still, all three cast"), Count(SelectCasters(Candidates, { 3, 1 }, 0.f)), 3);

	// The bench case: 500 lit fixtures, the 100 brightest moving; 64 casters of which 16 move.
	Candidates.Reset();
	for (int32 Index = 0; Index < 500; ++Index)
	{
		FStageShadowCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Importance = 1000.f - Index;
		Candidate.bMoving = Index < 100;
	}
	Casting = SelectCasters(Candidates, { 64, 16 }, 0.25f);
	int32 MovingCasters = 0;
	for (int32 Index = 0; Index < Casting.Num(); ++Index)
	{
		MovingCasters += Casting[Index] && Candidates[Index].bMoving ? 1 : 0;
	}
	TestEqual(TEXT("500: 64 cast"), Count(Casting), 64);
	TestEqual(TEXT("500: 16 of them move"), MovingCasters, 16);
	TestTrue(TEXT("500: the brightest still ones take the rest"), Casting[100] && Casting[147] && !Casting[148]);

	// With the hold: held moving casters count first, and the limit is hard, so the lower ranked held mover drops at once.
	constexpr double Hold = 1.0;
	Candidates = { { 5.f, true, 0.1, true }, { 4.f, true, 0.1, true }, { 1.f, false } };
	FStageShadowSelection Selection = SelectCastersWithHold(Candidates, { 3, 1 }, 0.25f, Hold);
	TestTrue(TEXT("held movers over the limit: the lower drops"), Selection.Casting == TArray<bool>({ true, false, true }));

	// A held moving caster keeps its place against a brighter free mover; the free one waits for the hold to end.
	Candidates = { { 9.f, false, TNumericLimits<double>::Max(), true }, { 2.f, true, 0.1, true } };
	Selection = SelectCastersWithHold(Candidates, { 4, 1 }, 0.25f, Hold);
	TestTrue(TEXT("held mover keeps the moving place"), !Selection.Casting[0] && Selection.Casting[1]);
	TestTrue(TEXT("retry when its hold ends (0.9 s)"), FMath::IsNearlyEqual(Selection.RetryAfterSeconds, 0.9, 1e-6));

	// Moving means a pan/tilt step within the last MovingSeconds; invalid or backwards clocks count as still.
	TestTrue(TEXT("0.5 s ago is moving"), StageShadowBudget::IsMoving(0.5, 1.0));
	TestFalse(TEXT("1 s ago is still"), StageShadowBudget::IsMoving(1.0, 1.0));
	TestFalse(TEXT("negative is still"), StageShadowBudget::IsMoving(-0.1, 1.0));
	TestFalse(TEXT("NaN is still"), StageShadowBudget::IsMoving(std::numeric_limits<double>::quiet_NaN(), 1.0));
	return true;
}

// The minimum hold (STATE.md #48): a fixture at the cut-off cannot switch its shadows on and off while the camera moves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShadowBudgetHoldTest, "StageCraft.Show.ShadowBudgetHold",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageShadowBudgetHoldTest::RunTest(const FString& Parameters)
{
	using StageShadowBudget::SelectCastersWithHold;
	constexpr double Hold = 1.0;
	constexpr int32 Budget = 1;

	// Two fixtures trade places as the camera moves between them, re-ranked every 0.25 s like UShowControlSubsystem does.
	// Without the hold they would swap on every ranking; with it, each state lasts at least Hold seconds.
	TArray<FStageShadowCandidate> Candidates = { { 1.f, true }, { 1.f, false } };
	TArray<double> SwitchedAt = { -100.0, -100.0 };
	int32 Switches = 0;
	double ShortestState = TNumericLimits<double>::Max();
	for (int32 Step = 0; Step < 40; ++Step)
	{
		const double Now = Step * 0.25;
		const bool bFirstNearer = Step % 2 == 0;
		Candidates[0].Importance = bFirstNearer ? 3.f : 1.f;
		Candidates[1].Importance = bFirstNearer ? 1.f : 3.f;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			Candidates[Index].SecondsSinceSwitch = Now - SwitchedAt[Index];
		}
		const FStageShadowSelection Selection = SelectCastersWithHold(Candidates, { Budget }, 0.25f, Hold);
		TestTrue(TEXT("never over the budget"), Selection.Casting.FilterByPredicate([](bool bCasting) { return bCasting; }).Num() <= Budget);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			if (Selection.Casting[Index] != Candidates[Index].bCasting)
			{
				ShortestState = FMath::Min(ShortestState, Now - SwitchedAt[Index]);
				SwitchedAt[Index] = Now;
				Candidates[Index].bCasting = Selection.Casting[Index];
				++Switches;
			}
		}
	}
	TestTrue(*FString::Printf(TEXT("no state shorter than the hold (shortest %.2f s)"), ShortestState), ShortestState >= Hold);
	TestTrue(*FString::Printf(TEXT("far fewer switches than rankings (%d)"), Switches), Switches <= 20);

	// A held caster that goes dark stops casting at once.
	Candidates = { { 0.f, true, 0.1 } };
	TestFalse(TEXT("dark drops despite the hold"), SelectCastersWithHold(Candidates, { 4 }, 0.25f, Hold).Casting[0]);

	// A held non-caster is not picked even when it outranks everyone, and the free budget goes to the next one.
	Candidates = { { 9.f, false, 0.2 }, { 1.f, false } };
	FStageShadowSelection Selection = SelectCastersWithHold(Candidates, { 1 }, 0.25f, Hold);
	TestTrue(TEXT("held non-caster stays off, the free one casts"), !Selection.Casting[0] && Selection.Casting[1]);
	TestTrue(TEXT("retry when the hold ends (0.8 s)"), FMath::IsNearlyEqual(Selection.RetryAfterSeconds, 0.8, 1e-6));

	// Held casters fill the budget first; a shrunken budget drops the dimmest of them.
	Candidates = { { 2.f, true, 0.1 }, { 5.f, true, 0.1 }, { 9.f, false } };
	Selection = SelectCastersWithHold(Candidates, { 1 }, 0.25f, Hold);
	TestTrue(TEXT("budget 1 keeps the brightest held caster only"), !Selection.Casting[0] && Selection.Casting[1] && !Selection.Casting[2]);

	// Nothing held: identical to the plain ranking, no retry. Hold 0 turns the rule off.
	Candidates = { { 1.f, false }, { 3.f, true }, { 2.f, false } };
	Selection = SelectCastersWithHold(Candidates, { 2 }, 0.25f, Hold);
	TestTrue(TEXT("unheld = SelectCasters"), Selection.Casting == StageShadowBudget::SelectCasters(Candidates, { 2 }, 0.25f));
	TestEqual(TEXT("no retry"), Selection.RetryAfterSeconds, 0.0);
	Candidates = { { 9.f, false, 0.1 } };
	TestTrue(TEXT("hold 0 is off"), SelectCastersWithHold(Candidates, { 1 }, 0.25f, 0.0).Casting[0]);
	TestTrue(TEXT("NaN hold is off"), SelectCastersWithHold(Candidates, { 1 }, 0.25f, std::numeric_limits<double>::quiet_NaN()).Casting[0]);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
