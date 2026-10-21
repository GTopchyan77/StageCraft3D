// Copyright Epic Games, Inc. All Rights Reserved.

// Unit tests for the key map and its storage (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.1-2.2): exact chord
// lookup, the binding rules (unsupported keys, modified fly keys, conflicts, fly keys shared with commands), reset, the saved
// overrides as untrusted ini data, and the settings fields the preferences subsystem writes.
// Run: UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests StageCraft.Input; Quit" -unattended -nullrhi

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Input/StageKeyBindings.h"
#include "Misc/AutomationTest.h"
#include "Settings/StageCraftUserSettings.h"
#include "UObject/Package.h"

#include <limits>

namespace StageKeyBindingsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter;

	using C = EStageInputCommand;
	using S = EStageKeySlot;

	FStageKeyChord Ctrl(const FKey& Key) { return FStageKeyChord(Key, true); }
	FStageKeyChord CtrlShift(const FKey& Key) { return FStageKeyChord(Key, true, true); }
	FStageKeyChord Plain(const FKey& Key) { return FStageKeyChord(Key); }

	FStageKeyBindingOverride MakeOverride(const TCHAR* Command, const FStageKeyChord& Primary, const FStageKeyChord& Secondary = FStageKeyChord())
	{
		FStageKeyBindingOverride Override;
		Override.Command = FName(Command);
		Override.Primary = Primary;
		Override.Secondary = Secondary;
		return Override;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageKeyBindingRulesTest, "StageCraft.Input.KeyBindings", StageKeyBindingsTests::Flags)

bool FStageKeyBindingRulesTest::RunTest(const FString& Parameters)
{
	using namespace StageKeyBindingsTests;

	// The defaults are a valid map: rebuilding them through the rules drops nothing.
	const FStageKeyBindings Defaults = FStageKeyBindings::MakeDefaults();
	{
		TArray<FStageKeyBindingOverride> All;
		for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
		{
			All.Add(MakeOverride(*StageKeyBindings::GetCommandName(Info.Command).ToString(), Info.DefaultPrimary, Info.DefaultSecondary));
		}
		int32 Dropped = -1;
		TestTrue(TEXT("Defaults: every default passes the rules"), FStageKeyBindings::FromOverrides(All, &Dropped) == Defaults && Dropped == 0);
		TestTrue(TEXT("Defaults: nothing to save"), Defaults.ToOverrides().IsEmpty());
	}

	// Exact chords: modifiers must match exactly.
	{
		TestTrue(TEXT("Lookup: Ctrl+Z is Undo"), Defaults.FindCommand(Ctrl(EKeys::Z)) == TOptional<C>(C::Undo));
		TestTrue(TEXT("Lookup: Ctrl+Shift+Z is Redo, not Undo"), Defaults.FindCommand(CtrlShift(EKeys::Z)) == TOptional<C>(C::Redo));
		TestTrue(TEXT("Lookup: Ctrl+Y is Redo"), Defaults.FindCommand(Ctrl(EKeys::Y)) == TOptional<C>(C::Redo));
		TestFalse(TEXT("Lookup: plain Z is nothing"), Defaults.FindCommand(Plain(EKeys::Z)).IsSet());
		TestFalse(TEXT("Lookup: Shift+P is not Place mode"), Defaults.FindCommand(FStageKeyChord(EKeys::P, false, true)).IsSet());
		TestTrue(TEXT("Lookup: W is the Move tool (flying is not a key-press command)"), Defaults.FindCommand(Plain(EKeys::W)) == TOptional<C>(C::MoveTool));
		TestFalse(TEXT("Lookup: S is nothing outside flying"), Defaults.FindCommand(Plain(EKeys::S)).IsSet());
		TestFalse(TEXT("Lookup: an unset chord is nothing"), Defaults.FindCommand(FStageKeyChord()).IsSet());
		TestTrue(TEXT("Lookup: Alt+J is the top view"), Defaults.FindCommand(FStageKeyChord(EKeys::J, false, false, true)) == TOptional<C>(C::ViewTop));
	}

	// The rules.
	{
		FStageKeyBindings Map = Defaults;
		C Conflict = C::Count;
		TestEqual(TEXT("Rules: a chord another command uses is refused"), Map.Assign(C::SelectAll, S::Primary, Ctrl(EKeys::Z), &Conflict), EStageKeyBindingResult::Conflict);
		TestEqual(TEXT("Rules: and names that command"), Conflict, C::Undo);
		TestTrue(TEXT("Rules: a refusal changes nothing"), Map == Defaults);

		Conflict = C::Count;
		TestEqual(TEXT("Rules: a command's own other slot conflicts too"), Map.Assign(C::Redo, S::Primary, CtrlShift(EKeys::Z), &Conflict), EStageKeyBindingResult::Conflict);
		TestEqual(TEXT("Rules: that conflict names the command itself"), Conflict, C::Redo);

		TestEqual(TEXT("Rules: a mouse button is not a shortcut"), Map.Assign(C::Undo, S::Secondary, Plain(EKeys::LeftMouseButton)), EStageKeyBindingResult::UnsupportedKey);
		TestEqual(TEXT("Rules: a modifier alone is not a shortcut"), Map.Assign(C::Undo, S::Secondary, Plain(EKeys::LeftControl)), EStageKeyBindingResult::UnsupportedKey);
		TestEqual(TEXT("Rules: the console key is reserved"), Map.Assign(C::Undo, S::Secondary, Plain(EKeys::Tilde)), EStageKeyBindingResult::UnsupportedKey);
		TestEqual(TEXT("Rules: gamepad buttons are not shortcuts"), Map.Assign(C::Undo, S::Secondary, Plain(EKeys::Gamepad_FaceButton_Bottom)), EStageKeyBindingResult::UnsupportedKey);
		TestEqual(TEXT("Rules: axes are not shortcuts"), Map.Assign(C::Undo, S::Secondary, Plain(EKeys::MouseWheelAxis)), EStageKeyBindingResult::UnsupportedKey);
		TestEqual(TEXT("Rules: fly keys take no modifiers"), Map.Assign(C::FlyForward, S::Secondary, FStageKeyChord(EKeys::Up, false, true)), EStageKeyBindingResult::ModifiersNotAllowed);

		Conflict = C::Count;
		TestEqual(TEXT("Rules: two fly directions cannot share a key"), Map.Assign(C::FlyUp, S::Primary, Plain(EKeys::W), &Conflict), EStageKeyBindingResult::Conflict);
		TestEqual(TEXT("Rules: the fly conflict names the other direction"), Conflict, C::FlyForward);
		TestEqual(TEXT("Rules: a command may share a fly key (never live together)"), Map.Assign(C::ToggleGrid, S::Primary, Plain(EKeys::S)), EStageKeyBindingResult::Success);
		TestTrue(TEXT("Rules: and is then found by its key"), Map.FindCommand(Plain(EKeys::S)) == TOptional<C>(C::ToggleGrid));
		TestEqual(TEXT("Rules: the same chord again is unchanged"), Map.Assign(C::ToggleGrid, S::Primary, Plain(EKeys::S)), EStageKeyBindingResult::Unchanged);
		TestEqual(TEXT("Rules: arrow keys as second fly keys"), Map.Assign(C::FlyForward, S::Secondary, Plain(EKeys::Up)), EStageKeyBindingResult::Success);

		TestTrue(TEXT("Clear: an assigned slot clears"), Map.Clear(C::Undo, S::Primary));
		TestFalse(TEXT("Clear: an empty slot reports no change"), Map.Clear(C::Undo, S::Primary));
		TestFalse(TEXT("Clear: the cleared chord no longer runs anything"), Map.FindCommand(Ctrl(EKeys::Z)).IsSet());
		TestEqual(TEXT("Clear: now free for another command"), Map.Assign(C::SelectAll, S::Secondary, Ctrl(EKeys::Z)), EStageKeyBindingResult::Success);

		// Undo's default (Ctrl+Z) now belongs to Select All: a reset must not take it away silently.
		TestFalse(TEXT("Reset: a default taken by another command is reported"), Map.Reset(C::Undo));
		TestFalse(TEXT("Reset: and that slot stays unbound"), Map.Get(C::Undo, S::Primary).IsSet());
		TestTrue(TEXT("Reset: the other command keeps it"), Map.FindCommand(Ctrl(EKeys::Z)) == TOptional<C>(C::SelectAll));
		TestTrue(TEXT("Reset: a free default comes back"), Map.Reset(C::ToggleGrid) && Map.Get(C::ToggleGrid, S::Primary) == Plain(EKeys::G));

		Map.ResetAll();
		TestTrue(TEXT("ResetAll: the defaults"), Map == Defaults);
	}

	// Descriptions for menus and hints.
	{
		TestEqual(TEXT("Describe: both chords"), StageKeyBindings::DescribeCommand(Defaults, C::Redo).ToString(), FString(TEXT("Ctrl+Y or Ctrl+Shift+Z")));
		TestEqual(TEXT("Describe: short key names"), StageKeyBindings::DescribeCommand(Defaults, C::Cancel).ToString(), FString(TEXT("Esc")));
		TestEqual(TEXT("Describe: fly keys"), StageKeyBindings::DescribeFlyKeys(Defaults).ToString(), FString(TEXT("WASD")));
		FStageKeyBindings Arrows = Defaults;
		for (const TPair<C, FKey>& Pair : { TPair<C, FKey>(C::FlyForward, EKeys::Up), TPair<C, FKey>(C::FlyBackward, EKeys::Down), TPair<C, FKey>(C::FlyLeft, EKeys::Left), TPair<C, FKey>(C::FlyRight, EKeys::Right) })
		{
			Arrows.Assign(Pair.Key, S::Primary, Plain(Pair.Value));
		}
		TestEqual(TEXT("Describe: longer key names are separated"), StageKeyBindings::DescribeFlyKeys(Arrows).ToString(), FString(TEXT("Up/Left/Down/Right")));
		Arrows.Clear(C::FlyLeft, S::Primary);
		TestTrue(TEXT("Describe: an unbound direction gives no fly hint"), StageKeyBindings::DescribeFlyKeys(Arrows).IsEmpty());
		FStageKeyBindings Unbound = Defaults;
		Unbound.Clear(C::Undo, S::Primary);
		TestTrue(TEXT("Describe: an unbound command is empty"), StageKeyBindings::DescribeCommand(Unbound, C::Undo).IsEmpty());
	}

	// Typed chords (console) and names.
	{
		FStageKeyChord Chord;
		TestTrue(TEXT("Parse: Ctrl+Shift+Z"), StageKeyBindings::ParseChord(TEXT("Ctrl+Shift+Z"), Chord) && Chord == CtrlShift(EKeys::Z));
		TestTrue(TEXT("Parse: Control and spaces"), StageKeyBindings::ParseChord(TEXT(" control + Alt + G "), Chord) && Chord == FStageKeyChord(EKeys::G, true, false, true));
		TestTrue(TEXT("Parse: None is unbound"), StageKeyBindings::ParseChord(TEXT("None"), Chord) && !Chord.IsSet());
		TestFalse(TEXT("Parse: an unknown key"), StageKeyBindings::ParseChord(TEXT("Ctrl+NotAKey"), Chord));
		TestFalse(TEXT("Parse: a modifier with no key"), StageKeyBindings::ParseChord(TEXT("Ctrl"), Chord));
		TestFalse(TEXT("Parse: an unknown modifier"), StageKeyBindings::ParseChord(TEXT("Hyper+Z"), Chord));
		TestEqual(TEXT("Chord text for logs"), CtrlShift(EKeys::Z).ToString(), FString(TEXT("Ctrl+Shift+Z")));
		TestTrue(TEXT("Names: case-insensitive"), StageKeyBindings::FindCommandByName(TEXT("undo")) == TOptional<C>(C::Undo));
		TestFalse(TEXT("Names: unknown"), StageKeyBindings::FindCommandByName(TEXT("Explode")).IsSet());
		TestFalse(TEXT("Names: the Count sentinel is not a command"), StageKeyBindings::FindCommandByName(TEXT("Count")).IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStageKeyBindingStorageTest, "StageCraft.Input.SavedBindings", StageKeyBindingsTests::Flags)

bool FStageKeyBindingStorageTest::RunTest(const FString& Parameters)
{
	using namespace StageKeyBindingsTests;
	const FStageKeyBindings Defaults = FStageKeyBindings::MakeDefaults();

	// Only differences are saved, and they load back to the same map.
	FStageKeyBindings Custom = Defaults;
	Custom.Assign(C::Undo, S::Primary, Ctrl(EKeys::U));
	Custom.Clear(C::Redo, S::Secondary);
	Custom.Assign(C::FlyForward, S::Secondary, Plain(EKeys::Up));
	const TArray<FStageKeyBindingOverride> Saved = Custom.ToOverrides();
	TestEqual(TEXT("Overrides: one per changed command"), Saved.Num(), 3);
	TestTrue(TEXT("Overrides: round trip"), FStageKeyBindings::FromOverrides(Saved) == Custom);

	// A swap only works if every overridden command is cleared first; it must load in either order.
	{
		const TArray<FStageKeyBindingOverride> Swap = { MakeOverride(TEXT("Undo"), Ctrl(EKeys::Y)), MakeOverride(TEXT("Redo"), Ctrl(EKeys::Z), CtrlShift(EKeys::Z)) };
		const TArray<FStageKeyBindingOverride> SwapReversed = { Swap[1], Swap[0] };
		int32 Dropped = -1;
		const FStageKeyBindings Swapped = FStageKeyBindings::FromOverrides(Swap, &Dropped);
		TestEqual(TEXT("Swap: nothing dropped"), Dropped, 0);
		TestTrue(TEXT("Swap: Ctrl+Y undoes"), Swapped.FindCommand(Ctrl(EKeys::Y)) == TOptional<C>(C::Undo));
		TestTrue(TEXT("Swap: Ctrl+Z redoes"), Swapped.FindCommand(Ctrl(EKeys::Z)) == TOptional<C>(C::Redo));
		TestTrue(TEXT("Swap: the order of the ini does not matter"), FStageKeyBindings::FromOverrides(SwapReversed) == Swapped);
	}

	// Damaged or hand-edited entries.
	{
		int32 Dropped = -1;
		const FStageKeyBindings Loaded = FStageKeyBindings::FromOverrides({
			MakeOverride(TEXT("NoSuchCommand"), Ctrl(EKeys::K)),				// unknown command: dropped
			MakeOverride(TEXT("Delete"), Plain(EKeys::LeftMouseButton)),		// not a command name (DeleteSelection): dropped
			MakeOverride(TEXT("DeleteSelection"), Plain(EKeys::LeftMouseButton)),	// unbindable key: that slot dropped
			MakeOverride(TEXT("SelectAll"), Ctrl(EKeys::Z)),					// takes Undo's chord, Undo not overridden: dropped
			MakeOverride(TEXT("FlyUp"), FStageKeyChord(EKeys::E, true)),		// modified fly key: dropped
			MakeOverride(TEXT("ToggleGrid"), Plain(EKeys::H)),					// fine
			MakeOverride(TEXT("ToggleGrid"), Plain(EKeys::N)),					// repeated command: dropped
		}, &Dropped);
		TestEqual(TEXT("Damaged: every bad entry counted"), Dropped, 6);
		TestTrue(TEXT("Damaged: the good entry applies"), Loaded.Get(C::ToggleGrid, S::Primary) == Plain(EKeys::H));
		TestTrue(TEXT("Damaged: Undo keeps its key"), Loaded.FindCommand(Ctrl(EKeys::Z)) == TOptional<C>(C::Undo));
		TestFalse(TEXT("Damaged: an overridden command with a bad key is left unbound, not given a mouse button"), Loaded.Get(C::DeleteSelection, S::Primary).IsSet());
		TestFalse(TEXT("Damaged: the bad fly key is unbound"), Loaded.Get(C::FlyUp, S::Primary).IsSet());
	}

	// The settings fields the preferences subsystem writes.
	{
		UStageCraftUserSettings* Settings = NewObject<UStageCraftUserSettings>(GetTransientPackage());
		TestTrue(TEXT("Settings: starts with the default map"), Settings->GetKeyBindings() == Defaults);
		TestTrue(TEXT("Settings: storing a custom map changes it"), Settings->SetKeyBindings(Custom));
		TestFalse(TEXT("Settings: storing it again does not"), Settings->SetKeyBindings(Custom));
		TestTrue(TEXT("Settings: reads back"), Settings->GetKeyBindings() == Custom);

		TestTrue(TEXT("Settings: look sensitivity is clamped high"), Settings->SetLookSensitivity(50.f) && Settings->GetLookSensitivity() == StageInputPreferences::MaxLookSensitivity);
		TestTrue(TEXT("Settings: and low"), Settings->SetLookSensitivity(0.f) && Settings->GetLookSensitivity() == StageInputPreferences::MinLookSensitivity);
		TestFalse(TEXT("Settings: NaN is refused"), Settings->SetLookSensitivity(std::numeric_limits<float>::quiet_NaN()));
		TestEqual(TEXT("Settings: and leaves the value"), Settings->GetLookSensitivity(), StageInputPreferences::MinLookSensitivity);
		TestTrue(TEXT("Settings: invert look"), Settings->SetLookInverted(true) && Settings->IsLookInverted() && !Settings->SetLookInverted(true));

		Settings->SetToDefaults();
		TestTrue(TEXT("Settings: defaults restore the key map"), Settings->GetKeyBindings() == Defaults);
		TestEqual(TEXT("Settings: and the look speed"), Settings->GetLookSensitivity(), StageInputPreferences::DefaultLookSensitivity);
		TestFalse(TEXT("Settings: and the invert"), Settings->IsLookInverted());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
