// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "StageKeyBindings.generated.h"

/**
 * Every action a user can bind keys to (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.1). The enumerator
 * names are written to GameUserSettings.ini: never rename one, only add new ones before Count.
 */
UENUM()
enum class EStageInputCommand : uint8
{
	Undo,
	Redo,
	DeleteSelection,
	SelectAll,
	Cancel,
	CycleTransformTool,
	MoveTool,
	RotateTool,
	ScaleTool,
	TogglePlaceMode,
	FinishPath,
	RemovePathPoint,
	ViewPerspective,
	ViewTop,
	ViewFront,
	ViewBack,
	ViewLeft,
	ViewRight,
	ToggleGrid,
	FlyForward,
	FlyBackward,
	FlyLeft,
	FlyRight,
	FlyUp,
	FlyDown,
	Count UMETA(Hidden)
};

/** How the Preferences panel groups the commands. Navigation commands act only while the right mouse button is held. */
enum class EStageInputCategory : uint8
{
	Editing,
	Tools,
	View,
	Navigation,
};

/** Each command has two bindings, like the Unreal Editor's keyboard shortcuts. */
enum class EStageKeySlot : uint8
{
	Primary,
	Secondary,
};

/** A key plus the modifiers that must be held with it, exactly (Ctrl+Z never matches a plain Z binding). */
USTRUCT()
struct MODULARSCENEBUILDER_API FStageKeyChord
{
	GENERATED_BODY()

	FStageKeyChord() = default;
	explicit FStageKeyChord(const FKey& InKey, bool bInCtrl = false, bool bInShift = false, bool bInAlt = false)
		: Key(InKey), bCtrl(bInCtrl), bShift(bInShift), bAlt(bInAlt)
	{
	}

	/** An unset chord means "not bound". */
	bool IsSet() const { return Key.IsValid(); }
	bool HasModifiers() const { return bCtrl || bShift || bAlt; }

	bool operator==(const FStageKeyChord& Other) const
	{
		return Key == Other.Key && bCtrl == Other.bCtrl && bShift == Other.bShift && bAlt == Other.bAlt;
	}
	bool operator!=(const FStageKeyChord& Other) const { return !(*this == Other); }

	/** "Ctrl+Shift+Z" with the keys' short display names ("Esc", "Space"); empty when unset. For menus, hints and the panel. */
	FText ToText() const;

	/** "Ctrl+Shift+Z" with engine key names ("Escape", "SpaceBar"); empty when unset. For logs and console commands. */
	FString ToString() const;

	UPROPERTY()
	FKey Key;

	UPROPERTY()
	bool bCtrl = false;

	UPROPERTY()
	bool bShift = false;

	UPROPERTY()
	bool bAlt = false;
};

/** One command's bindings as GameUserSettings.ini stores them: only commands that differ from the defaults are written. */
USTRUCT()
struct MODULARSCENEBUILDER_API FStageKeyBindingOverride
{
	GENERATED_BODY()

	/** The EStageInputCommand enumerator name. */
	UPROPERTY()
	FName Command;

	UPROPERTY()
	FStageKeyChord Primary;

	UPROPERTY()
	FStageKeyChord Secondary;

	bool operator==(const FStageKeyBindingOverride& Other) const
	{
		return Command == Other.Command && Primary == Other.Primary && Secondary == Other.Secondary;
	}
};

/** Why an assignment was refused. Nothing changes unless it is Success. */
enum class EStageKeyBindingResult : uint8
{
	Success,
	/** The slot already holds this chord. */
	Unchanged,
	/** Not a keyboard key, a modifier on its own, or a reserved key (the console key). */
	UnsupportedKey,
	/** Navigation keys are held while flying, so they take no Ctrl, Shift or Alt. */
	ModifiersNotAllowed,
	/** Another binding already uses the chord; its command is reported. */
	Conflict,
};

/** What the Preferences panel and the hints need to know about a command. */
struct FStageInputCommandInfo
{
	EStageInputCommand Command = EStageInputCommand::Count;
	EStageInputCategory Category = EStageInputCategory::Editing;
	FText Label;
	FText Description;
	FStageKeyChord DefaultPrimary;
	FStageKeyChord DefaultSecondary;
};

/**
 * A complete key map (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.1). A value type with no UObjects, safe on
 * any thread. Only Assign, Clear, Reset and ResetAll change it, and they enforce the rules, so a map never holds an unbindable
 * key, a modified navigation key or a conflict:
 *  - two editing, tool or view commands never share a chord, and two navigation commands never share a key;
 *  - a navigation key may equal a command chord (W is Move Tool and Fly Forward): commands are suspended while flying.
 */
class MODULARSCENEBUILDER_API FStageKeyBindings
{
public:
	/** The built-in map (the table in the ADR). */
	static FStageKeyBindings MakeDefaults();

	/**
	 * Rebuilds a map from saved overrides, which are untrusted ini data: unknown command names, repeated commands and invalid
	 * chords are dropped. Every overridden command is cleared before any is assigned, so swapped bindings load in any order.
	 * An override that still conflicts loses its chord. OutDropped (optional) counts everything dropped.
	 */
	static FStageKeyBindings FromOverrides(TConstArrayView<FStageKeyBindingOverride> Overrides, int32* OutDropped = nullptr);

	/** The commands whose bindings differ from the defaults, in command order. */
	TArray<FStageKeyBindingOverride> ToOverrides() const;

	const FStageKeyChord& Get(EStageInputCommand Command, EStageKeySlot Slot) const;

	/**
	 * Binds Chord to one slot of Command. Refused (map unchanged) for an unsupported key, a modified navigation key or a chord
	 * another binding uses; OutConflict then names that binding's command (it may be Command itself, for its other slot).
	 * An unset Chord is the same as Clear.
	 */
	EStageKeyBindingResult Assign(EStageInputCommand Command, EStageKeySlot Slot, const FStageKeyChord& Chord, EStageInputCommand* OutConflict = nullptr);

	/** Unbinds one slot. Returns false if it was already unbound. */
	bool Clear(EStageInputCommand Command, EStageKeySlot Slot);

	/**
	 * Puts Command's default chords back. A default that another command has taken meanwhile stays unbound in that slot, so
	 * the other binding is never silently lost. Returns false if a default could not be restored.
	 */
	bool Reset(EStageInputCommand Command);

	void ResetAll() { *this = MakeDefaults(); }

	/**
	 * The editing, tool or view command bound to Chord, if any. Navigation commands are never returned: they are not run on a
	 * key press but mapped to the fly axis while the right mouse button is held.
	 */
	TOptional<EStageInputCommand> FindCommand(const FStageKeyChord& Chord) const;

	bool operator==(const FStageKeyBindings& Other) const;
	bool operator!=(const FStageKeyBindings& Other) const { return !(*this == Other); }

private:
	/** The binding that would clash with Chord in Command's slot, other than that slot itself. */
	TOptional<EStageInputCommand> FindConflict(EStageInputCommand Command, EStageKeySlot Slot, const FStageKeyChord& Chord) const;

	FStageKeyChord& At(EStageInputCommand Command, EStageKeySlot Slot);

	static constexpr int32 CommandCount = static_cast<int32>(EStageInputCommand::Count);
	FStageKeyChord Chords[CommandCount][2];
};

namespace StageKeyBindings
{
	/** Every command, in panel order (category, then the order of the ADR table). */
	MODULARSCENEBUILDER_API TConstArrayView<FStageInputCommandInfo> GetCommands();

	MODULARSCENEBUILDER_API const FStageInputCommandInfo& GetInfo(EStageInputCommand Command);

	MODULARSCENEBUILDER_API bool IsNavigationCommand(EStageInputCommand Command);

	/** A keyboard key that can be bound: not a mouse button, axis, gamepad, touch or gesture key, not a modifier on its own, not AnyKey or the console key. */
	MODULARSCENEBUILDER_API bool IsBindableKey(const FKey& Key);

	/** The persisted name ("Undo"). */
	MODULARSCENEBUILDER_API FName GetCommandName(EStageInputCommand Command);

	/** The command for a persisted or typed name, case-insensitively; unset for an unknown name. */
	MODULARSCENEBUILDER_API TOptional<EStageInputCommand> FindCommandByName(FName Name);

	/**
	 * "Ctrl+Shift+Z" -> the chord, with modifiers Ctrl/Control, Shift and Alt and an engine key name last. "None" or an empty
	 * string is an unset chord. Returns false for anything else (unknown key, a modifier with no key).
	 */
	MODULARSCENEBUILDER_API bool ParseChord(const FString& Text, FStageKeyChord& OutChord);

	/** For hints and menus: "Ctrl+Y or Ctrl+Shift+Z", the primary alone, or empty when the command is unbound. */
	MODULARSCENEBUILDER_API FText DescribeCommand(const FStageKeyBindings& Bindings, EStageInputCommand Command);

	/** The fly keys for the status bar hint: "WASD" for single-letter keys, "Up/Left/Down/Right" otherwise; empty if any is unbound. */
	MODULARSCENEBUILDER_API FText DescribeFlyKeys(const FStageKeyBindings& Bindings);
}
