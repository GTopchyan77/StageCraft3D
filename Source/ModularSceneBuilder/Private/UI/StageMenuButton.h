// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "Containers/Ticker.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateTypes.h"
#include "UI/StageMenuModel.h"
#include "StageMenuButton.generated.h"

/** Describes the menu each time it opens, from the owner's state at that moment. */
DECLARE_DELEGATE_RetVal(FStageMenu, FOnStageMenuButtonGetMenu);

/**
 * A dropdown button that opens a popup menu, drawn with the StageCraft theme: the UMG counterpart of the Unreal Editor's
 * viewport "Lit" menu button. It shows a label and an arrow; clicking it asks the owner for the menu content.
 *
 * - The owner describes the menu fresh on every open (OnGetMenu, an FStageMenu), so the popup shows the state at that moment
 *   and needs no bindings of its own. Picking an entry closes the menu.
 * - The button is never keyboard-focusable, like UStageToolButton, so viewport shortcuts keep working after a click. The open
 *   menu is focused instead, so typing searches it (submenus included) and Escape closes it.
 * - Call ApplyTheme before the Slate widget is built (the style is copied into it then).
 */
UCLASS(MinimalAPI)
class UStageMenuButton : public UWidget
{
	GENERATED_BODY()

public:
	void ApplyTheme(const class UStageCraftUITheme& Theme);

	/** Updates the shown label; safe before and after the Slate widget exists. */
	void SetLabel(const FText& InLabel);

	const FText& GetLabel() const { return Label; }

	/** Bound by the owning panel; it owns this widget, so the binding cannot outlive its target. */
	FOnStageMenuButtonGetMenu OnGetMenu;

	/** The menu as it would open now; empty when unbound. Scripted checks read and pick it (StageCraft.Workspace.Menu). */
	FStageMenu GetMenu() const;

	/** Opens the popup as a click would; false (nothing opens) before the Slate widget exists or while disabled. */
	bool OpenMenu();

protected:
	//~ Begin UWidget Interface
	virtual TSharedRef<class SWidget> RebuildWidget() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	//~ End UWidget Interface

private:
	TSharedRef<class SWidget> HandleGetMenuContent();

	FComboButtonStyle ComboStyle;
	FSlateFontInfo LabelFont;
	FSlateColor LabelColor;
	FText Label;

	TSharedPtr<class SComboButton> MyComboButton;
	TSharedPtr<class STextBlock> MyLabel;
	void HandleMenuOpenChanged(bool bOpen);

	/** The open menu's widget (owned by the popup), focused so typing searches it; null when the menu was empty. */
	TWeakPtr<class SWidget> MenuWidget;
	/** What had keyboard focus when the menu opened (the viewport, usually), given back when the menu closes. */
	TWeakPtr<class SWidget> FocusBeforeMenu;
	/** The one-shot restore after a close; at most one pending. */
	FTSTicker::FDelegateHandle RestoreFocusTicker;
};
