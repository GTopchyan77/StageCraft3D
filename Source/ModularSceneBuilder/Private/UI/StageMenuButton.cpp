// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageMenuButton.h"

#include "Framework/Application/SlateApplication.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Text/STextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageMenuButton)

void UStageMenuButton::ApplyTheme(const UStageCraftUITheme& Theme)
{
	// The same button as the theme's dropdown fields (the projection selector), so the toolbar's two dropdowns match.
	ComboStyle = StageCraftWidgetStyle::MakeComboBox(Theme).ComboButtonStyle;
	LabelFont = StageCraftWidgetStyle::MakeFont(9);
	LabelColor = FSlateColor(Theme.TextPrimary);
}

void UStageMenuButton::SetLabel(const FText& InLabel)
{
	// Views refresh labels on every pushed change (a zoom step changes the view); the same text must not invalidate the layout.
	if (Label.IdenticalTo(InLabel) || Label.EqualTo(InLabel))
	{
		return;
	}
	Label = InLabel;
	if (MyLabel)
	{
		MyLabel->SetText(Label);
	}
}

TSharedRef<SWidget> UStageMenuButton::RebuildWidget()
{
	MyComboButton = SNew(SComboButton)
		.ComboButtonStyle(&ComboStyle)
		.IsFocusable(false)
		.HasDownArrow(true)
		.ForegroundColor(LabelColor)
		.ContentPadding(FMargin(6.f, 1.f))
		.OnGetMenuContent(FOnGetContent::CreateUObject(this, &ThisClass::HandleGetMenuContent))
		.OnMenuOpenChanged(FOnIsOpenChanged::CreateUObject(this, &ThisClass::HandleMenuOpenChanged))
		.ButtonContent()
		[
			SAssignNew(MyLabel, STextBlock)
			.Text(Label)
			.Font(LabelFont)
			.ColorAndOpacity(LabelColor)
		];
	return MyComboButton.ToSharedRef();
}

void UStageMenuButton::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	MyComboButton.Reset();
	MyLabel.Reset();
	MenuWidget.Reset();
	FocusBeforeMenu.Reset();
	if (RestoreFocusTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RestoreFocusTicker);
		RestoreFocusTicker.Reset();
	}
}

FStageMenu UStageMenuButton::GetMenu() const
{
	// Unbound only if the owner was torn down while the button was still clickable; an empty popup is harmless.
	return OnGetMenu.IsBound() ? OnGetMenu.Execute() : FStageMenu();
}

bool UStageMenuButton::OpenMenu()
{
	if (!MyComboButton || !GetIsEnabled())
	{
		return false;
	}
	MyComboButton->SetIsOpen(true);
	// Opened this way the menu stack focuses its own wrapper, a parent of the menu widget; typed characters bubble up from the
	// focused widget and would never reach the menu's search. Focus the menu itself, as a click does (HandleGetMenuContent).
	if (const TSharedPtr<SWidget> Content = MenuWidget.Pin(); Content && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetUserFocus(FSlateApplication::Get().GetUserIndexForKeyboard(), Content, EFocusCause::SetDirectly);
	}
	return true;
}

TSharedRef<SWidget> UStageMenuButton::HandleGetMenuContent()
{
	const FStageMenu Menu = GetMenu();
	const TSharedRef<SWidget> Content = Menu.IsEmpty() ? StaticCastSharedRef<SWidget>(SNew(SSpacer)) : StageMenu::MakeWidget(Menu);
	// "Start typing to search" (it also searches inside submenus) only works when the menu widget has keyboard focus. The button
	// itself stays non-focusable, so viewport shortcuts keep working after a click; the combo button focuses this widget instead
	// when a click opens the menu, and HandleMenuOpenChanged gives focus back when it closes.
	if (FSlateApplication::IsInitialized())
	{
		FocusBeforeMenu = FSlateApplication::Get().GetUserFocusedWidget(FSlateApplication::Get().GetUserIndexForKeyboard());
	}
	MenuWidget =Menu.IsEmpty() ? TWeakPtr<SWidget>() : TWeakPtr<SWidget>(Content);
	if (MyComboButton)
	{
		MyComboButton->SetMenuContentWidgetToFocus(MenuWidget);
	}
	return Content;
}

void UStageMenuButton::HandleMenuOpenChanged(bool bOpen)
{
	if (bOpen || !FocusBeforeMenu.IsValid())
	{
		return;
	}
	// The menu leaves focus nowhere when it closes: Escape clears it after this event, and a pick destroys the focused menu. The
	// engine restores the previous focus only when the OS re-activates the owner window, which does not always happen. Give it
	// back on the next tick, and only if nothing has taken focus meanwhile (a click into a text field keeps it).
	if (RestoreFocusTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RestoreFocusTicker);
	}
	RestoreFocusTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float)
	{
		RestoreFocusTicker.Reset();
		const TSharedPtr<SWidget> Previous = FocusBeforeMenu.Pin();
		FocusBeforeMenu.Reset();
		if (Previous && FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			const uint32 User = Slate.GetUserIndexForKeyboard();
			if (!Slate.GetUserFocusedWidget(User).IsValid())
			{
				Slate.SetUserFocus(User, Previous, EFocusCause::SetDirectly);
			}
		}
		return false;
	}));
}
