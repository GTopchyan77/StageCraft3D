// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the cue rules (Docs/ADR/0011-cue-list-and-patch-sheet-export.md §2.1): valid numbers, rounding, fade and label
// limits, and the repair of untrusted cue lists. No world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Show; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Show/ShowControlTypes.h"

#include <limits>

namespace StageCueRulesTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	FShowCue MakeCue(float Number, const TCHAR* Label = TEXT(""), float Fade = 1.f)
	{
		FShowCue Cue;
		Cue.Number = Number;
		Cue.Label = FText::FromString(Label);
		Cue.FadeTime = Fade;
		return Cue;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageCueRulesTest, "StageCraft.Show.CueRules", StageCueRulesTests::Flags)

bool FStageCueRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageCueRulesTests;
	using namespace StageCueRules;

	// Numbers: the ends of the range are valid, outside it and non-finite are not.
	TestTrue(TEXT("0.001 is valid"), IsValidCueNumber(0.001f));
	TestTrue(TEXT("1 is valid"), IsValidCueNumber(1.f));
	TestTrue(TEXT("9999.999 is valid"), IsValidCueNumber(9999.999f));
	TestFalse(TEXT("0 is not"), IsValidCueNumber(0.f));
	TestFalse(TEXT("0.0004 rounds to 0, so it is not"), IsValidCueNumber(0.0004f));
	TestFalse(TEXT("Negative is not"), IsValidCueNumber(-1.f));
	TestFalse(TEXT("10000 is not"), IsValidCueNumber(10000.f));
	TestFalse(TEXT("NaN is not"), IsValidCueNumber(std::numeric_limits<float>::quiet_NaN()));
	TestFalse(TEXT("Infinity is not"), IsValidCueNumber(std::numeric_limits<float>::infinity()));
	TestEqual(TEXT("Rounded to the resolution"), RoundCueNumber(2.5004f), 2.5f, 1.e-5f);
	TestEqual(TEXT("Rounded up"), RoundCueNumber(1.0006f), 1.001f, 1.e-5f);

	// Fades and labels.
	TestEqual(TEXT("Negative fade is 0"), SanitizeFadeTime(-3.f), 0.f);
	TestEqual(TEXT("Huge fade is clamped"), SanitizeFadeTime(1.e9f), MaxFadeTime);
	TestEqual(TEXT("NaN fade is 0"), SanitizeFadeTime(std::numeric_limits<float>::quiet_NaN()), 0.f);
	TestEqual(TEXT("A fade in range is kept"), SanitizeFadeTime(2.5f), 2.5f);
	TestEqual(TEXT("Label trimmed"), SanitizeLabel(FText::FromString(TEXT("  Blackout "))).ToString(), FString(TEXT("Blackout")));
	TestEqual(TEXT("Label cut to the limit"), SanitizeLabel(FText::FromString(FString::ChrN(MaxLabelLength + 50, TEXT('x')))).ToString().Len(), MaxLabelLength);

	// A list from a file or an undo step: invalid and duplicate numbers dropped (first wins), the rest sorted and sanitized.
	TArray<FShowCue> Cues = {
		MakeCue(3.f, TEXT("C")), MakeCue(1.f, TEXT("A"), -1.f), MakeCue(0.f), MakeCue(3.0002f, TEXT("C2")),
		MakeCue(std::numeric_limits<float>::quiet_NaN()), MakeCue(2.0004f, TEXT(" B ")),
	};
	TestEqual(TEXT("Three dropped"), SanitizeCueList(Cues), 3);
	if (TestEqual(TEXT("Three kept"), Cues.Num(), 3))
	{
		TestEqual(TEXT("Sorted: first"), Cues[0].Label.ToString(), FString(TEXT("A")));
		TestEqual(TEXT("Sorted: second, number rounded"), Cues[1].Number, 2.f, 1.e-5f);
		TestEqual(TEXT("Sorted: second, label trimmed"), Cues[1].Label.ToString(), FString(TEXT("B")));
		TestEqual(TEXT("The first of a duplicate wins"), Cues[2].Label.ToString(), FString(TEXT("C")));
		TestEqual(TEXT("Fade sanitized"), Cues[0].FadeTime, 0.f);
	}
	TArray<FShowCue> Empty;
	TestEqual(TEXT("An empty list is fine"), SanitizeCueList(Empty), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
