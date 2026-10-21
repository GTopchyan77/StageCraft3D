// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/StageInputPreferencesSubsystem.h"

#include "ModularSceneBuilder.h"
#include "Settings/StageCraftUserSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageInputPreferencesSubsystem)

#define LOCTEXT_NAMESPACE "StageInputPreferences"

void UStageInputPreferencesSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	if (!Settings)
	{
		KeyBindings = FStageKeyBindings::MakeDefaults();
		UE_LOG(LogStageCraft, Warning, TEXT("Preferences: GameUserSettingsClassName is not StageCraftUserSettings; the default keys apply and changes are not saved."));
		return;
	}

	int32 Dropped = 0;
	KeyBindings = Settings->GetKeyBindings(&Dropped);
	if (Dropped > 0)
	{
		UE_LOG(LogStageCraft, Warning, TEXT("Preferences: %d saved key binding(s) were invalid or conflicting and were dropped; those slots use their default keys or stay unbound."), Dropped);
	}
	UE_LOG(LogStageCraft, Log, TEXT("Preferences: %d command(s) use custom keys."), KeyBindings.ToOverrides().Num());
}

EStageKeyBindingResult UStageInputPreferencesSubsystem::SetKeyBinding(EStageInputCommand Command, EStageKeySlot Slot, const FStageKeyChord& Chord, EStageInputCommand& OutConflict)
{
	const EStageKeyBindingResult Result = KeyBindings.Assign(Command, Slot, Chord, &OutConflict);
	if (Result == EStageKeyBindingResult::Success)
	{
		UE_LOG(LogStageCraft, Log, TEXT("Preferences: %s %s = %s."), *StageKeyBindings::GetCommandName(Command).ToString(),
			Slot == EStageKeySlot::Primary ? TEXT("primary") : TEXT("secondary"), Chord.IsSet() ? *Chord.ToString() : TEXT("(none)"));
		CommitKeyBindings();
	}
	return Result;
}

void UStageInputPreferencesSubsystem::ClearKeyBinding(EStageInputCommand Command, EStageKeySlot Slot)
{
	if (KeyBindings.Clear(Command, Slot))
	{
		CommitKeyBindings();
	}
}

bool UStageInputPreferencesSubsystem::ResetKeyBinding(EStageInputCommand Command)
{
	const FStageKeyBindings Before = KeyBindings;
	const bool bRestored = KeyBindings.Reset(Command);
	if (KeyBindings != Before)
	{
		CommitKeyBindings();
	}
	return bRestored;
}

void UStageInputPreferencesSubsystem::ResetAllKeyBindings()
{
	const FStageKeyBindings Before = KeyBindings;
	KeyBindings.ResetAll();
	if (KeyBindings != Before)
	{
		CommitKeyBindings();
	}
}

float UStageInputPreferencesSubsystem::GetLookSensitivity() const
{
	const UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	return Settings ? Settings->GetLookSensitivity() : StageInputPreferences::DefaultLookSensitivity;
}

void UStageInputPreferencesSubsystem::SetLookSensitivity(float Sensitivity)
{
	UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	Commit(Settings && Settings->SetLookSensitivity(Sensitivity));
}

bool UStageInputPreferencesSubsystem::IsLookInverted() const
{
	const UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	return Settings && Settings->IsLookInverted();
}

void UStageInputPreferencesSubsystem::SetLookInverted(bool bInverted)
{
	UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	Commit(Settings && Settings->SetLookInverted(bInverted));
}

void UStageInputPreferencesSubsystem::CommitKeyBindings()
{
	UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get();
	if (Settings)
	{
		Settings->SetKeyBindings(KeyBindings);
	}
	// Broadcast even without settings: the controller and the hints follow the map in use, saved or not.
	Commit(/*bChanged*/ true);
}

void UStageInputPreferencesSubsystem::Commit(bool bChanged)
{
	if (!bChanged)
	{
		return;
	}
	if (UStageCraftUserSettings* Settings = UStageCraftUserSettings::Get())
	{
		Settings->SaveSettings();
	}
	OnInputPreferencesChanged.Broadcast();
}

FText UStageInputPreferencesSubsystem::DescribeRefusal(EStageKeyBindingResult Result, const FStageKeyChord& Chord, EStageInputCommand Conflict)
{
	switch (Result)
	{
	case EStageKeyBindingResult::UnsupportedKey:
		return FText::Format(LOCTEXT("Unsupported", "{0} cannot be used for a shortcut. Use a keyboard key, with Ctrl, Shift or Alt if you like."), Chord.ToText());
	case EStageKeyBindingResult::ModifiersNotAllowed:
		return LOCTEXT("NoModifiers", "Fly keys are held while flying, so they cannot use Ctrl, Shift or Alt.");
	case EStageKeyBindingResult::Conflict:
		return Conflict < EStageInputCommand::Count
			? FText::Format(LOCTEXT("Conflict", "{0} is already used by {1}. Change or clear that shortcut first."), Chord.ToText(), StageKeyBindings::GetInfo(Conflict).Label)
			: FText::Format(LOCTEXT("ConflictUnknown", "{0} is already in use."), Chord.ToText());
	case EStageKeyBindingResult::Success:
	case EStageKeyBindingResult::Unchanged:
	default:
		return FText::GetEmpty();
	}
}

#undef LOCTEXT_NAMESPACE
