// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/StageShortcutRouting.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/StageInputPreferencesSubsystem.h"
#include "Player/ModularPlayerController.h"
#include "Widgets/SViewport.h"
#include "Workspace/StageWorkspaceSubsystem.h"

namespace StageShortcutRoutingImpl
{
	/** The widget types that take typed text; the focus sits on these inside an SEditableTextBox or SMultiLineEditableTextBox. */
	const FName EditableText(TEXT("SEditableText"));
	const FName MultiLineEditableText(TEXT("SMultiLineEditableText"));
}

bool StageShortcutRouting::IsAppWideCommand(EStageInputCommand Command)
{
	switch (Command)
	{
	case EStageInputCommand::Save:
	case EStageInputCommand::Undo:
	case EStageInputCommand::Redo:
	case EStageInputCommand::Duplicate:
		return true;
	default:
		return false;
	}
}

bool StageShortcutRouting::ShouldRoute(EStageInputCommand Command, EStageShortcutFocus Focus, const FStageShortcutContext& Context)
{
	if (!IsAppWideCommand(Command) || Context.bDialogOpen || Context.bCapturingKey || Context.bFlying)
	{
		return false;
	}
	switch (Focus)
	{
	case EStageShortcutFocus::OtherWidget:
	case EStageShortcutFocus::None:
		return true;
	case EStageShortcutFocus::Viewport:	// InputKey runs it; routing here too would run it twice.
	case EStageShortcutFocus::TextEntry:	// The text field's own undo, and typing.
	default:
		return false;
	}
}

FStageShortcutInputProcessor::FStageShortcutInputProcessor(AModularPlayerController& InController)
	: Controller(&InController)
{
}

EStageShortcutFocus FStageShortcutInputProcessor::ClassifyFocus(FSlateApplication& SlateApp, uint32 UserIndex)
{
	using namespace StageShortcutRoutingImpl;
	const TSharedPtr<SWidget> Focused = SlateApp.GetUserFocusedWidget(UserIndex);
	if (!Focused.IsValid())
	{
		return EStageShortcutFocus::None;
	}
	const FName Type = Focused->GetType();
	if (Type == EditableText || Type == MultiLineEditableText)
	{
		return EStageShortcutFocus::TextEntry;
	}
	const TSharedPtr<SViewport> Viewport = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
	if (Viewport.IsValid() && (Focused == Viewport || Viewport->HasUserFocusedDescendants(UserIndex)))
	{
		return EStageShortcutFocus::Viewport;
	}
	return EStageShortcutFocus::OtherWidget;
}

bool FStageShortcutInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	AModularPlayerController* Owner = Controller.Get();
	if (!Owner || InKeyEvent.IsRepeat() || !StageKeyBindings::IsBindableKey(InKeyEvent.GetKey()))
	{
		return false;
	}
	const ULocalPlayer* Player = Owner->GetLocalPlayer();
	const UStageInputPreferencesSubsystem* Preferences = Player ? Player->GetSubsystem<UStageInputPreferencesSubsystem>() : nullptr;
	if (!Preferences)
	{
		return false;
	}
	// The same key map as InputKey, so a rebound shortcut works the same from every panel.
	const FStageKeyChord Chord(InKeyEvent.GetKey(), InKeyEvent.IsControlDown(), InKeyEvent.IsShiftDown(), InKeyEvent.IsAltDown());
	const TOptional<EStageInputCommand> Command = Preferences->GetKeyBindings().FindCommand(Chord);
	if (!Command.IsSet())
	{
		return false;
	}

	FStageShortcutContext Context;
	const UGameInstance* GameInstance = Owner->GetGameInstance();
	const UStageWorkspaceSubsystem* Workspace = GameInstance ? GameInstance->GetSubsystem<UStageWorkspaceSubsystem>() : nullptr;
	Context.bDialogOpen = SlateApp.GetActiveModalWindow().IsValid() || (Workspace && Workspace->IsSavePromptOpen());
	Context.bCapturingKey = Preferences->IsKeyCaptureActive();
	Context.bFlying = Owner->IsNavigatingCamera();
	if (!StageShortcutRouting::ShouldRoute(Command.GetValue(), ClassifyFocus(SlateApp, InKeyEvent.GetUserIndex()), Context))
	{
		return false;
	}
	return Owner->RequestAppWideCommand(Command.GetValue());
}
