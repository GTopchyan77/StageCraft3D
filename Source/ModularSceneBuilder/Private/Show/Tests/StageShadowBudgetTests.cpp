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
	TArray<bool> Casting = StageShadowBudget::SelectCasters(Candidates, 2, 0.f);
	TestTrue(TEXT("two brightest cast"), Casting[1] && Casting[3] && !Casting[0] && !Casting[2]);

	// Dark fixtures never cast, even under the budget; invalid importances count as dark.
	Candidates = Make({ 0.f, 2.f, std::numeric_limits<float>::quiet_NaN(), -1.f });
	Casting = StageShadowBudget::SelectCasters(Candidates, 10, 0.f);
	TestEqual(TEXT("only the lit one casts"), Count(Casting), 1);
	TestTrue(TEXT("... index 1"), Casting[1]);

	// Budget 0: nobody; budget off (-1): every lit fixture.
	TestEqual(TEXT("budget 0"), Count(StageShadowBudget::SelectCasters(Make({ 1.f, 2.f }), 0, 0.f)), 0);
	TestEqual(TEXT("budget off"), Count(StageShadowBudget::SelectCasters(Make({ 1.f, 2.f, 0.f }), -1, 0.f)), 2);

	// Hysteresis: a caster keeps its place against a newcomer up to 25 % brighter, not against a much brighter one.
	Candidates = Make({ 1.f, 1.2f });
	Candidates[0].bCasting = true;
	TestTrue(TEXT("caster keeps its place"), StageShadowBudget::SelectCasters(Candidates, 1, 0.25f)[0]);
	Candidates[1].Importance = 2.f;
	TestTrue(TEXT("much brighter newcomer takes it"), StageShadowBudget::SelectCasters(Candidates, 1, 0.25f)[1]);

	// Ties keep the lower index, whatever the order of evaluation.
	Casting = StageShadowBudget::SelectCasters(Make({ 2.f, 2.f, 2.f }), 2, 0.f);
	TestTrue(TEXT("ties keep registration order"), Casting[0] && Casting[1] && !Casting[2]);
	TestEqual(TEXT("empty input"), StageShadowBudget::SelectCasters({}, 4, 0.f).Num(), 0);

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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
