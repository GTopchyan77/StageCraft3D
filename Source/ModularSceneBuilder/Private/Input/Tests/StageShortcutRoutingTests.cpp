// Copyright Epic Games, Inc. All Rights Reserved.

// Focus-independent shortcuts (ADR 0013 addendum, STATE.md #57): the decision StageShortcutRouting::ShouldRoute makes for every
// focus and blocking state. Pure logic: no Slate, no world.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Input.ShortcutRouting; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Input/StageShortcutRouting.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageShortcutRoutingTest, "StageCraft.Input.ShortcutRouting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FStageShortcutRoutingTest::RunTest(const FString& Parameters)
{
	using C = EStageInputCommand;
	using F = EStageShortcutFocus;
	using StageShortcutRouting::ShouldRoute;
	const FStageShortcutContext Free;

	for (const C Command : { C::Save, C::Undo, C::Redo, C::Duplicate })
	{
		const FString Name = UEnum::GetValueAsString(Command);
		TestTrue(Name + TEXT(" is app-wide"), StageShortcutRouting::IsAppWideCommand(Command));
		TestTrue(Name + TEXT(" runs from a panel"), ShouldRoute(Command, F::OtherWidget, Free));
		TestTrue(Name + TEXT(" runs with nothing focused"), ShouldRoute(Command, F::None, Free));
		TestFalse(Name + TEXT(" is left to InputKey in the viewport (never twice)"), ShouldRoute(Command, F::Viewport, Free));
		TestFalse(Name + TEXT(" is left to a text field"), ShouldRoute(Command, F::TextEntry, Free));

		FStageShortcutContext Dialog;
		Dialog.bDialogOpen = true;
		TestFalse(Name + TEXT(" waits while a dialog is open"), ShouldRoute(Command, F::OtherWidget, Dialog));
		FStageShortcutContext Capture;
		Capture.bCapturingKey = true;
		TestFalse(Name + TEXT(" is bound, not run, while a shortcut is being captured"), ShouldRoute(Command, F::OtherWidget, Capture));
		FStageShortcutContext Flying;
		Flying.bFlying = true;
		TestFalse(Name + TEXT(" waits while flying"), ShouldRoute(Command, F::None, Flying));
	}

	// Everything else keeps its focus-bound meaning: Delete in a panel, W in a text field, tool keys.
	for (const C Command : { C::DeleteSelection, C::SelectAll, C::Cancel, C::MoveTool, C::TogglePlaceMode, C::FocusSelection, C::ToggleGrid })
	{
		const FString Name = UEnum::GetValueAsString(Command);
		TestFalse(Name + TEXT(" is not app-wide"), StageShortcutRouting::IsAppWideCommand(Command));
		TestFalse(Name + TEXT(" is not routed from a panel"), ShouldRoute(Command, F::OtherWidget, Free));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
