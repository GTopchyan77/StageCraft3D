// Copyright Epic Games, Inc. All Rights Reserved.

// Development console commands for the input preferences (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md §2): list,
// bind, clear and reset keys and set the look preferences through UStageInputPreferencesSubsystem, the same single write
// path the Preferences panel uses. With StageCraft.Edit.Key they let a scripted run remap a key and then press it.
// Compiled out of Shipping.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "ModularSceneBuilder.h"

namespace StageInputCommands
{
	UStageInputPreferencesSubsystem* GetPreferences(UWorld* World)
	{
		const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		return Controller ? ULocalPlayer::GetSubsystem<UStageInputPreferencesSubsystem>(Controller->GetLocalPlayer()) : nullptr;
	}

	void List(const TArray<FString>& Args, UWorld* World)
	{
		const UStageInputPreferencesSubsystem* Preferences = GetPreferences(World);
		if (!Preferences)
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input: no local player preferences in this world."));
			return;
		}
		for (const FStageInputCommandInfo& Info : StageKeyBindings::GetCommands())
		{
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input: %-20s %-16s %s"), *StageKeyBindings::GetCommandName(Info.Command).ToString(),
				*Preferences->GetKeyBindings().Get(Info.Command, EStageKeySlot::Primary).ToString(),
				*Preferences->GetKeyBindings().Get(Info.Command, EStageKeySlot::Secondary).ToString());
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input: look sensitivity %.2f, inverted %s; %d custom command(s)."), Preferences->GetLookSensitivity(),
			Preferences->IsLookInverted() ? TEXT("yes") : TEXT("no"), Preferences->GetKeyBindings().ToOverrides().Num());
	}

	/** StageCraft.Input.Bind <Command> <Chord|None> [1|2] */
	void Bind(const TArray<FString>& Args, UWorld* World)
	{
		UStageInputPreferencesSubsystem* Preferences = GetPreferences(World);
		const TOptional<EStageInputCommand> Command = Args.Num() >= 2 ? StageKeyBindings::FindCommandByName(FName(*Args[0])) : TOptional<EStageInputCommand>();
		FStageKeyChord Chord;
		if (!Preferences || !Command.IsSet() || !StageKeyBindings::ParseChord(Args[1], Chord))
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Input.Bind <Command> <Chord|None> [1|2]  (e.g. Undo Ctrl+U, FlyForward Up 2; see StageCraft.Input.List)"));
			return;
		}
		const EStageKeySlot Slot = Args.IsValidIndex(2) && Args[2] == TEXT("2") ? EStageKeySlot::Secondary : EStageKeySlot::Primary;
		EStageInputCommand Conflict = EStageInputCommand::Count;
		const EStageKeyBindingResult Result = Preferences->SetKeyBinding(Command.GetValue(), Slot, Chord, Conflict);
		const FText Refusal = UStageInputPreferencesSubsystem::DescribeRefusal(Result, Chord, Conflict);
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input.Bind %s %s: %s"), *Args[0], *Args[1],
			Refusal.IsEmpty() ? (Result == EStageKeyBindingResult::Success ? TEXT("bound") : TEXT("unchanged")) : *Refusal.ToString());
	}

	/** StageCraft.Input.Reset [Command|All] */
	void Reset(const TArray<FString>& Args, UWorld* World)
	{
		UStageInputPreferencesSubsystem* Preferences = GetPreferences(World);
		if (!Preferences)
		{
			return;
		}
		if (Args.IsEmpty() || Args[0].Equals(TEXT("All"), ESearchCase::IgnoreCase))
		{
			Preferences->ResetAllKeyBindings();
			UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input.Reset: every command uses its default keys."));
			return;
		}
		const TOptional<EStageInputCommand> Command = StageKeyBindings::FindCommandByName(FName(*Args[0]));
		if (!Command.IsSet())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Input.Reset [Command|All]"));
			return;
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input.Reset %s: %s"), *Args[0],
			Preferences->ResetKeyBinding(Command.GetValue()) ? TEXT("defaults restored") : TEXT("a default is taken by another command; that slot stays unbound"));
	}

	/** StageCraft.Input.Look <Sensitivity> [Invert 0|1] */
	void Look(const TArray<FString>& Args, UWorld* World)
	{
		UStageInputPreferencesSubsystem* Preferences = GetPreferences(World);
		if (!Preferences || Args.IsEmpty())
		{
			UE_LOG(LogStageCraft, Display, TEXT("Usage: StageCraft.Input.Look <Sensitivity 0.1-5> [Invert 0|1]"));
			return;
		}
		Preferences->SetLookSensitivity(FCString::Atof(*Args[0]));
		if (Args.IsValidIndex(1))
		{
			Preferences->SetLookInverted(Args[1] == TEXT("1"));
		}
		UE_LOG(LogStageCraft, Display, TEXT("StageCraft.Input.Look: sensitivity %.2f, inverted %s."), Preferences->GetLookSensitivity(),
			Preferences->IsLookInverted() ? TEXT("yes") : TEXT("no"));
	}

	FAutoConsoleCommandWithWorldAndArgs ListCommand(TEXT("StageCraft.Input.List"), TEXT("Logs every command's keys and the look preferences."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&List));
	FAutoConsoleCommandWithWorldAndArgs BindCommand(TEXT("StageCraft.Input.Bind"), TEXT("<Command> <Chord|None> [1|2]: binds a key, like the Preferences panel (refusals are logged)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Bind));
	FAutoConsoleCommandWithWorldAndArgs ResetCommand(TEXT("StageCraft.Input.Reset"), TEXT("[Command|All]: default keys again."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Reset));
	FAutoConsoleCommandWithWorldAndArgs LookCommand(TEXT("StageCraft.Input.Look"), TEXT("<Sensitivity> [Invert 0|1]: the camera look preferences."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Look));
}

#endif // !UE_BUILD_SHIPPING
