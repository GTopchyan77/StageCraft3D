// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateTypes.h"
#include "StageMenuButton.generated.h"

/** Builds the popup each time the menu opens. Must return a valid widget. */
DECLARE_DELEGATE_RetVal(TSharedRef<class SWidget>, FOnStageMenuButtonGetContent);

/**
 * A dropdown button that opens a popup menu, drawn with the StageCraft theme: the UMG counterpart of the Unreal Editor's
 * viewport "Lit" menu button. It shows a label and an arrow; clicking it asks the owner for the menu content.
 *
 * - The owner builds the menu fresh on every open (OnGetMenuContent), normally with FMenuBuilder, so the popup shows the
 *   state at that moment and needs no bindings of its own. Picking an entry closes the menu.
 * - Never keyboard-focusable, like UStageToolButton, so viewport shortcuts keep working after a click.
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

	/** Bound by the owning panel; it owns this widget, so the binding cannot outlive its target. */
	FOnStageMenuButtonGetContent OnGetMenuContent;

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
};
