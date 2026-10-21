// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Input/StageKeyBindings.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "StageInputPreferencesSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnStageInputPreferencesChanged);

/**
 * The local user's input preferences: the key map and how the camera looks around
 * (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2.2).
 *
 * Ownership:
 *  - The source of truth is UStageCraftUserSettings (GameUserSettings.ini, per machine). This subsystem is the only writer of
 *    its input fields. It keeps the validated key map it read at start-up, so a key press never rebuilds it from the ini.
 *  - Every setter validates through FStageKeyBindings (or clamps), stores, saves the ini and broadcasts
 *    OnInputPreferencesChanged once. A refused or unchanged request stores nothing and broadcasts nothing.
 *  - Readers (the controller's key dispatch and fly mapping, the status bar hints, the menus, the Preferences panel) read once
 *    and bind to OnInputPreferencesChanged. No tick.
 *
 * Game thread only. Without UStageCraftUserSettings (GameUserSettingsClassName not set) the defaults apply and nothing is saved.
 */
UCLASS()
class MODULARSCENEBUILDER_API UStageInputPreferencesSubsystem : public ULocalPlayerSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	//~ End USubsystem Interface

	const FStageKeyBindings& GetKeyBindings() const { return KeyBindings; }

	/**
	 * Binds Chord to one slot of Command (an unset chord unbinds it). Refused for an unsupported key, a modified navigation key
	 * or a chord another binding uses; OutConflict then names that binding's command.
	 */
	EStageKeyBindingResult SetKeyBinding(EStageInputCommand Command, EStageKeySlot Slot, const FStageKeyChord& Chord, EStageInputCommand& OutConflict);

	/** Unbinds one slot. */
	void ClearKeyBinding(EStageInputCommand Command, EStageKeySlot Slot);

	/** Command's default keys again; a default another command has taken stays unbound (returns false then). */
	bool ResetKeyBinding(EStageInputCommand Command);

	void ResetAllKeyBindings();

	/** "Ctrl+Z", "Ctrl+Y or Ctrl+Shift+Z", or empty when unbound. */
	FText DescribeCommand(EStageInputCommand Command) const { return StageKeyBindings::DescribeCommand(KeyBindings, Command); }

	/** Multiplier on the camera's look speed (StageInputPreferences::MinLookSensitivity..MaxLookSensitivity). */
	float GetLookSensitivity() const;

	/** Clamped; a non-finite value is ignored. */
	void SetLookSensitivity(float Sensitivity);

	/** Moving the mouse up looks down. */
	bool IsLookInverted() const;
	void SetLookInverted(bool bInverted);

	/** Why a SetKeyBinding was refused, for the Preferences panel and the console. Empty for Success and Unchanged. */
	static FText DescribeRefusal(EStageKeyBindingResult Result, const FStageKeyChord& Chord, EStageInputCommand Conflict);

	UPROPERTY(BlueprintAssignable, Category = "StageCraft|Preferences")
	FOnStageInputPreferencesChanged OnInputPreferencesChanged;

private:
	/** Stores the key map in the settings, saves and broadcasts. */
	void CommitKeyBindings();

	/** Saves the ini (if a setting changed) and broadcasts. */
	void Commit(bool bChanged);

	FStageKeyBindings KeyBindings;
};
