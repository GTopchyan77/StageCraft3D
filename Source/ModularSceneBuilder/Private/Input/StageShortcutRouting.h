// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "Input/StageKeyBindings.h"

/** Where the keyboard focus is when a key goes down, as far as shortcut routing cares. */
enum class EStageShortcutFocus : uint8
{
	/** The scene viewport (or a widget inside it): AModularPlayerController::InputKey already handles every shortcut there. */
	Viewport,
	/** A panel, a floating window, the outliner, a button: no shortcut would reach the controller without help. */
	OtherWidget,
	/** A text field: Ctrl+Z, Ctrl+S, Ctrl+D... belong to the text. */
	TextEntry,
	/** Nothing has keyboard focus. */
	None,
};

/** What else stops a shortcut, besides the focus. */
struct FStageShortcutContext
{
	/** A modal window or a workspace dialog (the save prompt) is open: nothing behind it may act. */
	bool bDialogOpen = false;
	/** The Preferences panel is capturing a new shortcut: the chord must be bound, not run. */
	bool bCapturingKey = false;
	/** The right mouse button is flying the camera: keys are fly keys meanwhile. */
	bool bFlying = false;
};

/**
 * Focus-independent shortcuts (Docs/ADR/0013-preferences-key-bindings-and-audit-fixes.md, addendum of STATE.md #57).
 *
 * Keys reach AModularPlayerController::InputKey only while the scene viewport is on the keyboard focus path, so with the focus in
 * a docked panel or a floating window no shortcut fired. FStageShortcutInputProcessor sees every key first (a Slate input
 * preprocessor) and runs the app-wide commands itself when the viewport would not get them; everything else is left alone.
 */
namespace StageShortcutRouting
{
	/** The commands that act on the whole stage and so work from any panel: Save, Undo, Redo, Duplicate. */
	bool IsAppWideCommand(EStageInputCommand Command);

	/**
	 * Whether the preprocessor runs Command now. False leaves the key to its normal route: the viewport's InputKey (so a command
	 * never runs twice), the text field, the dialog or the key capture.
	 */
	bool ShouldRoute(EStageInputCommand Command, EStageShortcutFocus Focus, const FStageShortcutContext& Context);
}

/**
 * The Slate input preprocessor that carries out StageShortcutRouting for one local player controller. Registered by the
 * controller in BeginPlay and unregistered in EndPlay; holds the controller weakly, so a stale registration does nothing.
 * Game thread only (Slate input).
 */
class FStageShortcutInputProcessor : public IInputProcessor
{
public:
	explicit FStageShortcutInputProcessor(class AModularPlayerController& InController);

	//~ Begin IInputProcessor Interface
	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<class ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("StageCraftShortcuts"); }
	//~ End IInputProcessor Interface

	/** Where SlateApp's keyboard focus for UserIndex is (see EStageShortcutFocus). */
	static EStageShortcutFocus ClassifyFocus(FSlateApplication& SlateApp, uint32 UserIndex);

private:
	TWeakObjectPtr<class AModularPlayerController> Controller;
};
