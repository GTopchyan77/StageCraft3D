// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FOnStageWelcomeTemplatePicked, const FString& /*TemplateName*/);
DECLARE_DELEGATE_OneParam(FOnStageWelcomeShowAtStartupChanged, bool /*bShow*/);

/**
 * The welcome screen after startup (Docs/ADR/0024-startup-atmospheres-starter-stages.md §6): one card per starter stage,
 * "Continue" to keep the stage that is open, and a "Show at startup" switch.
 *
 * Shown by UStageWorkspaceSubsystem over the workspace (FStageWorkspaceShell::ShowDialog) once the startup loader has gone.
 * Pure view: it reports the choice, and the owner opens the starter stage through the controller and closes it.
 * Escape is Continue.
 */
class SStageWelcomeDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStageWelcomeDialog) : _bShowAtStartup(true) {}
		SLATE_ARGUMENT(TArray<FString>, TemplateNames)
		SLATE_ARGUMENT(bool, bShowAtStartup)
		SLATE_EVENT(FOnStageWelcomeTemplatePicked, OnTemplatePicked)
		SLATE_EVENT(FSimpleDelegate, OnContinue)
		SLATE_EVENT(FOnStageWelcomeShowAtStartupChanged, OnShowAtStartupChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The Continue button: where keyboard focus goes when the dialog opens. */
	TSharedPtr<SWidget> GetInitialFocus() const { return ContinueButton; }

	//~ Begin SWidget Interface
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	//~ End SWidget Interface

private:
	TSharedRef<SWidget> MakeCard(const FString& TemplateName);

	FOnStageWelcomeTemplatePicked OnTemplatePicked;
	FSimpleDelegate OnContinue;
	FOnStageWelcomeShowAtStartupChanged OnShowAtStartupChanged;
	TSharedPtr<class SButton> ContinueButton;
};
