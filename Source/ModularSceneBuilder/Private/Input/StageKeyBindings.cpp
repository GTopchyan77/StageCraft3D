// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/StageKeyBindings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageKeyBindings)

#define LOCTEXT_NAMESPACE "StageKeyBindings"

namespace StageKeyBindings
{
	namespace
	{
		int32 Index(EStageInputCommand Command)
		{
			return static_cast<int32>(Command);
		}

		int32 Index(EStageKeySlot Slot)
		{
			return static_cast<int32>(Slot);
		}

		constexpr EStageKeySlot Slots[] = { EStageKeySlot::Primary, EStageKeySlot::Secondary };

		FStageKeyChord Ctrl(const FKey& Key) { return FStageKeyChord(Key, /*Ctrl*/ true); }
		FStageKeyChord CtrlShift(const FKey& Key) { return FStageKeyChord(Key, /*Ctrl*/ true, /*Shift*/ true); }
		FStageKeyChord Alt(const FKey& Key) { return FStageKeyChord(Key, false, false, /*Alt*/ true); }
		FStageKeyChord AltShift(const FKey& Key) { return FStageKeyChord(Key, false, /*Shift*/ true, /*Alt*/ true); }
		FStageKeyChord Plain(const FKey& Key) { return FStageKeyChord(Key); }

		TArray<FStageInputCommandInfo> BuildCommands()
		{
			using C = EStageInputCommand;
			using G = EStageInputCategory;
			const auto Make = [](C Command, G Category, const FText& Label, const FText& Description, const FStageKeyChord& Primary, const FStageKeyChord& Secondary = FStageKeyChord())
			{
				FStageInputCommandInfo Info;
				Info.Command = Command;
				Info.Category = Category;
				Info.Label = Label;
				Info.Description = Description;
				Info.DefaultPrimary = Primary;
				Info.DefaultSecondary = Secondary;
				return Info;
			};

			TArray<FStageInputCommandInfo> Commands =
			{
				Make(C::Undo, G::Editing, LOCTEXT("Undo", "Undo"), LOCTEXT("UndoTip", "Revert the last placement, deletion, transform or cue edit."), Ctrl(EKeys::Z)),
				Make(C::Redo, G::Editing, LOCTEXT("Redo", "Redo"), LOCTEXT("RedoTip", "Re-apply the last undone step."), Ctrl(EKeys::Y), CtrlShift(EKeys::Z)),
				Make(C::DeleteSelection, G::Editing, LOCTEXT("Delete", "Delete Selection"), LOCTEXT("DeleteTip", "Remove the selected items as one step."), Plain(EKeys::Delete)),
				Make(C::SelectAll, G::Editing, LOCTEXT("SelectAll", "Select All"), LOCTEXT("SelectAllTip", "Select every placed item."), Ctrl(EKeys::A)),
				Make(C::Cancel, G::Editing, LOCTEXT("Cancel", "Cancel / Deselect"), LOCTEXT("CancelTip", "Back out one level: drop the path being drawn, leave Place mode, then clear the selection."), Plain(EKeys::Escape)),

				Make(C::CycleTransformTool, G::Tools, LOCTEXT("CycleTool", "Cycle Transform Tool"), LOCTEXT("CycleToolTip", "Move, then Rotate, then Scale."), Plain(EKeys::SpaceBar)),
				Make(C::MoveTool, G::Tools, LOCTEXT("MoveTool", "Move Tool"), LOCTEXT("MoveToolTip", "Show the move gizmo."), Plain(EKeys::W)),
				Make(C::RotateTool, G::Tools, LOCTEXT("RotateTool", "Rotate Tool"), LOCTEXT("RotateToolTip", "Show the rotate gizmo."), Plain(EKeys::E)),
				Make(C::ScaleTool, G::Tools, LOCTEXT("ScaleTool", "Scale Tool"), LOCTEXT("ScaleToolTip", "Show the scale gizmo."), Plain(EKeys::R)),
				Make(C::TogglePlaceMode, G::Tools, LOCTEXT("PlaceMode", "Place Mode"), LOCTEXT("PlaceModeTip", "Switch between placing the armed Library item and selecting."), Plain(EKeys::P)),
				Make(C::FinishPath, G::Tools, LOCTEXT("FinishPath", "Finish Path"), LOCTEXT("FinishPathTip", "Place the truss or cable path being drawn."), Plain(EKeys::Enter)),
				Make(C::RemovePathPoint, G::Tools, LOCTEXT("RemovePoint", "Remove Last Path Point"), LOCTEXT("RemovePointTip", "Take back the last point of the path being drawn."), Plain(EKeys::BackSpace)),

				Make(C::ViewPerspective, G::View, LOCTEXT("ViewPerspective", "Perspective View"), LOCTEXT("ViewPerspectiveTip", "Back to the free 3D camera."), Alt(EKeys::G)),
				Make(C::ViewTop, G::View, LOCTEXT("ViewTop", "Top View"), LOCTEXT("ViewTopTip", "Orthographic plan view."), Alt(EKeys::J)),
				Make(C::ViewFront, G::View, LOCTEXT("ViewFront", "Front View"), LOCTEXT("ViewFrontTip", "Orthographic view from the audience."), Alt(EKeys::H)),
				Make(C::ViewBack, G::View, LOCTEXT("ViewBack", "Back View"), LOCTEXT("ViewBackTip", "Orthographic view from upstage."), AltShift(EKeys::H)),
				Make(C::ViewLeft, G::View, LOCTEXT("ViewLeft", "Left View"), LOCTEXT("ViewLeftTip", "Orthographic side view from the left."), Alt(EKeys::K)),
				Make(C::ViewRight, G::View, LOCTEXT("ViewRight", "Right View"), LOCTEXT("ViewRightTip", "Orthographic side view from the right."), AltShift(EKeys::K)),
				Make(C::ToggleGrid, G::View, LOCTEXT("ToggleGrid", "Show / Hide Grid"), LOCTEXT("ToggleGridTip", "Show or hide the viewport's reference grid."), Plain(EKeys::G)),

				Make(C::FlyForward, G::Navigation, LOCTEXT("FlyForward", "Fly Forward"), LOCTEXT("FlyTip", "While the right mouse button is held."), Plain(EKeys::W)),
				Make(C::FlyBackward, G::Navigation, LOCTEXT("FlyBackward", "Fly Back"), LOCTEXT("FlyTip", "While the right mouse button is held."), Plain(EKeys::S)),
				Make(C::FlyLeft, G::Navigation, LOCTEXT("FlyLeft", "Fly Left"), LOCTEXT("FlyTip", "While the right mouse button is held."), Plain(EKeys::A)),
				Make(C::FlyRight, G::Navigation, LOCTEXT("FlyRight", "Fly Right"), LOCTEXT("FlyTip", "While the right mouse button is held."), Plain(EKeys::D)),
				Make(C::FlyUp, G::Navigation, LOCTEXT("FlyUp", "Fly Up"), LOCTEXT("FlyTip", "While the right mouse button is held."), Plain(EKeys::E)),
				Make(C::FlyDown, G::Navigation, LOCTEXT("FlyDown", "Fly Down"), LOCTEXT("FlyTip", "While the right mouse button is held."), Plain(EKeys::Q)),

				// Appended because the order must follow the enum; the Preferences panel still lists them under VIEW.
				Make(C::FocusSelection, G::View, LOCTEXT("FocusSelection", "Focus Selection"), LOCTEXT("FocusSelectionTip", "Frame the selected items in the view, as F does in the Unreal Editor."), Plain(EKeys::F)),
				Make(C::ToggleViewportToolbar, G::View, LOCTEXT("ToggleToolbar", "Show / Hide Viewport Toolbar"), LOCTEXT("ToggleToolbarTip", "Collapse the toolbar above the viewport to a small button, or bring it back."), CtrlShift(EKeys::T)),
			};
			check(Commands.Num() == Index(C::Count));
			for (int32 Position = 0; Position < Commands.Num(); ++Position)
			{
				check(Index(Commands[Position].Command) == Position);
			}
			return Commands;
		}
	}

	TConstArrayView<FStageInputCommandInfo> GetCommands()
	{
		// Built on first use: FKeys are only valid once the engine has registered its keys.
		static const TArray<FStageInputCommandInfo> Commands = BuildCommands();
		return Commands;
	}

	const FStageInputCommandInfo& GetInfo(EStageInputCommand Command)
	{
		check(Command < EStageInputCommand::Count);
		return GetCommands()[Index(Command)];
	}

	bool IsNavigationCommand(EStageInputCommand Command)
	{
		return Command < EStageInputCommand::Count && GetInfo(Command).Category == EStageInputCategory::Navigation;
	}

	bool IsBindableKey(const FKey& Key)
	{
		return Key.IsValid()
			&& !Key.IsModifierKey()
			&& !Key.IsMouseButton()
			&& !Key.IsAnalog()
			&& !Key.IsGamepadKey()
			&& !Key.IsTouch()
			&& !Key.IsGesture()
			&& !Key.IsVirtual()
			&& !Key.IsDeprecated()
			&& Key != EKeys::AnyKey
			// The development console opens on this key before the game sees it, so a binding to it would never fire there.
			&& Key != EKeys::Tilde;
	}

	FName GetCommandName(EStageInputCommand Command)
	{
		return FName(StaticEnum<EStageInputCommand>()->GetNameStringByValue(static_cast<int64>(Command)));
	}

	TOptional<EStageInputCommand> FindCommandByName(FName Name)
	{
		for (const FStageInputCommandInfo& Info : GetCommands())
		{
			if (GetCommandName(Info.Command).ToString().Equals(Name.ToString(), ESearchCase::IgnoreCase))
			{
				return Info.Command;
			}
		}
		return {};
	}

	bool ParseChord(const FString& Text, FStageKeyChord& OutChord)
	{
		OutChord = FStageKeyChord();
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT("+"));
		if (Parts.IsEmpty())
		{
			return false;
		}
		FStageKeyChord Chord;
		for (int32 Position = 0; Position < Parts.Num() - 1; ++Position)
		{
			const FString Part = Parts[Position].TrimStartAndEnd();
			if (Part.Equals(TEXT("Ctrl"), ESearchCase::IgnoreCase) || Part.Equals(TEXT("Control"), ESearchCase::IgnoreCase))
			{
				Chord.bCtrl = true;
			}
			else if (Part.Equals(TEXT("Shift"), ESearchCase::IgnoreCase))
			{
				Chord.bShift = true;
			}
			else if (Part.Equals(TEXT("Alt"), ESearchCase::IgnoreCase))
			{
				Chord.bAlt = true;
			}
			else
			{
				return false;
			}
		}
		Chord.Key = FKey(FName(*Parts.Last().TrimStartAndEnd()));
		if (!Chord.Key.IsValid())
		{
			return false;
		}
		OutChord = Chord;
		return true;
	}

	FText DescribeCommand(const FStageKeyBindings& Bindings, EStageInputCommand Command)
	{
		const FText Primary = Bindings.Get(Command, EStageKeySlot::Primary).ToText();
		const FText Secondary = Bindings.Get(Command, EStageKeySlot::Secondary).ToText();
		if (Primary.IsEmpty() || Secondary.IsEmpty())
		{
			return Primary.IsEmpty() ? Secondary : Primary;
		}
		return FText::Format(LOCTEXT("EitherChord", "{0} or {1}"), Primary, Secondary);
	}

	FText DescribeFlyKeys(const FStageKeyBindings& Bindings)
	{
		// In the order people say it: WASD.
		const EStageInputCommand Order[] = { EStageInputCommand::FlyForward, EStageInputCommand::FlyLeft, EStageInputCommand::FlyBackward, EStageInputCommand::FlyRight };
		TArray<FString> Keys;
		bool bAllSingleCharacters = true;
		for (EStageInputCommand Command : Order)
		{
			const FStageKeyChord& Chord = Bindings.Get(Command, EStageKeySlot::Primary);
			if (!Chord.IsSet())
			{
				return FText::GetEmpty();
			}
			Keys.Add(Chord.ToText().ToString());
			bAllSingleCharacters &= Keys.Last().Len() == 1;
		}
		return FText::FromString(FString::Join(Keys, bAllSingleCharacters ? TEXT("") : TEXT("/")));
	}
}

// --- FStageKeyChord ---

FText FStageKeyChord::ToText() const
{
	if (!IsSet())
	{
		return FText::GetEmpty();
	}
	FString Text;
	if (bCtrl)
	{
		Text += LOCTEXT("CtrlPrefix", "Ctrl+").ToString();
	}
	if (bShift)
	{
		Text += LOCTEXT("ShiftPrefix", "Shift+").ToString();
	}
	if (bAlt)
	{
		Text += LOCTEXT("AltPrefix", "Alt+").ToString();
	}
	return FText::FromString(Text + Key.GetDisplayName(/*bLongDisplayName*/ false).ToString());
}

FString FStageKeyChord::ToString() const
{
	if (!IsSet())
	{
		return FString();
	}
	return FString::Printf(TEXT("%s%s%s%s"), bCtrl ? TEXT("Ctrl+") : TEXT(""), bShift ? TEXT("Shift+") : TEXT(""), bAlt ? TEXT("Alt+") : TEXT(""), *Key.ToString());
}

// --- FStageKeyBindings ---

FStageKeyBindings FStageKeyBindings::MakeDefaults()
{
	FStageKeyBindings Bindings;
	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		Bindings.At(Info.Command, EStageKeySlot::Primary) = Info.DefaultPrimary;
		Bindings.At(Info.Command, EStageKeySlot::Secondary) = Info.DefaultSecondary;
	}
	return Bindings;
}

const FStageKeyChord& FStageKeyBindings::Get(EStageInputCommand Command, EStageKeySlot Slot) const
{
	check(Command < EStageInputCommand::Count);
	return Chords[StageKeyBindings::Index(Command)][StageKeyBindings::Index(Slot)];
}

FStageKeyChord& FStageKeyBindings::At(EStageInputCommand Command, EStageKeySlot Slot)
{
	check(Command < EStageInputCommand::Count);
	return Chords[StageKeyBindings::Index(Command)][StageKeyBindings::Index(Slot)];
}

TOptional<EStageInputCommand> FStageKeyBindings::FindConflict(EStageInputCommand Command, EStageKeySlot Slot, const FStageKeyChord& Chord) const
{
	const bool bNavigation = StageKeyBindings::IsNavigationCommand(Command);
	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		// Navigation keys and command chords are never live together (commands are suspended while flying), so they may overlap.
		if (StageKeyBindings::IsNavigationCommand(Info.Command) != bNavigation)
		{
			continue;
		}
		for (EStageKeySlot OtherSlot : StageKeyBindings::Slots)
		{
			if (Info.Command == Command && OtherSlot == Slot)
			{
				continue;
			}
			const FStageKeyChord& Other = Get(Info.Command, OtherSlot);
			// Navigation keys carry no modifiers, so the key alone decides.
			if (Other.IsSet() && (bNavigation ? Other.Key == Chord.Key : Other == Chord))
			{
				return Info.Command;
			}
		}
	}
	return {};
}

EStageKeyBindingResult FStageKeyBindings::Assign(EStageInputCommand Command, EStageKeySlot Slot, const FStageKeyChord& Chord, EStageInputCommand* OutConflict)
{
	if (Command >= EStageInputCommand::Count)
	{
		return EStageKeyBindingResult::UnsupportedKey;
	}
	if (!Chord.IsSet())
	{
		return Clear(Command, Slot) ? EStageKeyBindingResult::Success : EStageKeyBindingResult::Unchanged;
	}
	if (!StageKeyBindings::IsBindableKey(Chord.Key))
	{
		return EStageKeyBindingResult::UnsupportedKey;
	}
	if (StageKeyBindings::IsNavigationCommand(Command) && Chord.HasModifiers())
	{
		return EStageKeyBindingResult::ModifiersNotAllowed;
	}
	if (Get(Command, Slot) == Chord)
	{
		return EStageKeyBindingResult::Unchanged;
	}
	if (const TOptional<EStageInputCommand> Conflict = FindConflict(Command, Slot, Chord))
	{
		if (OutConflict)
		{
			*OutConflict = Conflict.GetValue();
		}
		return EStageKeyBindingResult::Conflict;
	}
	At(Command, Slot) = Chord;
	return EStageKeyBindingResult::Success;
}

bool FStageKeyBindings::Clear(EStageInputCommand Command, EStageKeySlot Slot)
{
	FStageKeyChord& Chord = At(Command, Slot);
	if (!Chord.IsSet())
	{
		return false;
	}
	Chord = FStageKeyChord();
	return true;
}

bool FStageKeyBindings::Reset(EStageInputCommand Command)
{
	const FStageInputCommandInfo& Info = StageKeyBindings::GetInfo(Command);
	Clear(Command, EStageKeySlot::Primary);
	Clear(Command, EStageKeySlot::Secondary);
	bool bRestored = true;
	for (EStageKeySlot Slot : StageKeyBindings::Slots)
	{
		const FStageKeyChord& Default = Slot == EStageKeySlot::Primary ? Info.DefaultPrimary : Info.DefaultSecondary;
		const EStageKeyBindingResult Result = Assign(Command, Slot, Default);
		bRestored &= Result == EStageKeyBindingResult::Success || Result == EStageKeyBindingResult::Unchanged;
	}
	return bRestored;
}

TOptional<EStageInputCommand> FStageKeyBindings::FindCommand(const FStageKeyChord& Chord) const
{
	if (!Chord.IsSet())
	{
		return {};
	}
	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		if (Info.Category == EStageInputCategory::Navigation)
		{
			continue;
		}
		if (Get(Info.Command, EStageKeySlot::Primary) == Chord || Get(Info.Command, EStageKeySlot::Secondary) == Chord)
		{
			return Info.Command;
		}
	}
	return {};
}

bool FStageKeyBindings::operator==(const FStageKeyBindings& Other) const
{
	for (int32 Command = 0; Command < CommandCount; ++Command)
	{
		if (Chords[Command][0] != Other.Chords[Command][0] || Chords[Command][1] != Other.Chords[Command][1])
		{
			return false;
		}
	}
	return true;
}

TArray<FStageKeyBindingOverride> FStageKeyBindings::ToOverrides() const
{
	TArray<FStageKeyBindingOverride> Overrides;
	for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
	{
		const FStageKeyChord& Primary = Get(Info.Command, EStageKeySlot::Primary);
		const FStageKeyChord& Secondary = Get(Info.Command, EStageKeySlot::Secondary);
		if (Primary != Info.DefaultPrimary || Secondary != Info.DefaultSecondary)
		{
			FStageKeyBindingOverride& Override = Overrides.AddDefaulted_GetRef();
			Override.Command = StageKeyBindings::GetCommandName(Info.Command);
			Override.Primary = Primary;
			Override.Secondary = Secondary;
		}
	}
	return Overrides;
}

FStageKeyBindings FStageKeyBindings::FromOverrides(TConstArrayView<FStageKeyBindingOverride> Overrides, int32* OutDropped)
{
	int32 Dropped = 0;
	FStageKeyBindings Bindings = MakeDefaults();

	// First pass: which overrides are usable. The first entry for a command wins; later repeats are dropped.
	TArray<TPair<EStageInputCommand, const FStageKeyBindingOverride*>> Usable;
	TSet<EStageInputCommand> Seen;
	for (const FStageKeyBindingOverride& Override : Overrides)
	{
		const TOptional<EStageInputCommand> Command = StageKeyBindings::FindCommandByName(Override.Command);
		bool bAlreadySeen = false;
		if (Command.IsSet())
		{
			Seen.Add(Command.GetValue(), &bAlreadySeen);
		}
		if (!Command.IsSet() || bAlreadySeen)
		{
			++Dropped;
			continue;
		}
		Usable.Emplace(Command.GetValue(), &Override);
	}

	// Cleared before any is assigned, so a swap (Undo on Ctrl+Y, Redo on Ctrl+Z) does not trip over the defaults it replaces.
	for (const TPair<EStageInputCommand, const FStageKeyBindingOverride*>& Entry : Usable)
	{
		Bindings.Clear(Entry.Key, EStageKeySlot::Primary);
		Bindings.Clear(Entry.Key, EStageKeySlot::Secondary);
	}
	for (const TPair<EStageInputCommand, const FStageKeyBindingOverride*>& Entry : Usable)
	{
		for (EStageKeySlot Slot : StageKeyBindings::Slots)
		{
			const FStageKeyChord& Chord = Slot == EStageKeySlot::Primary ? Entry.Value->Primary : Entry.Value->Secondary;
			const EStageKeyBindingResult Result = Bindings.Assign(Entry.Key, Slot, Chord);
			if (Chord.IsSet() && Result != EStageKeyBindingResult::Success && Result != EStageKeyBindingResult::Unchanged)
			{
				++Dropped;
			}
		}
	}

	if (OutDropped)
	{
		*OutDropped = Dropped;
	}
	return Bindings;
}

#undef LOCTEXT_NAMESPACE
