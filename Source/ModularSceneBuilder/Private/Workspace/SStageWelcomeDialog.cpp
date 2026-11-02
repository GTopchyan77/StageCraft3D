// Copyright Epic Games, Inc. All Rights Reserved.

#include "Workspace/SStageWelcomeDialog.h"

#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Styling/AppStyle.h"
#include "UI/StageCraftWidgetStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "StageWelcomeDialog"

namespace StageWelcomeDialog
{
	constexpr float Width = 560.f;
	constexpr float Padding = 20.f;
	constexpr int32 Columns = 2;

	/** One line under each known starter stage's name; unknown (user-added) ones show none. */
	FText Describe(const FString& TemplateName)
	{
		if (TemplateName.Contains(TEXT("Concert"))) { return LOCTEXT("ConcertTip", "Thrust stage, truss and moving lights, night sky"); }
		if (TemplateName.Contains(TEXT("Theatre"))) { return LOCTEXT("TheatreTip", "Proscenium deck, lighting bars, the venue's own lighting"); }
		if (TemplateName.Contains(TEXT("Conference"))) { return LOCTEXT("ConferenceTip", "Low riser and a front truss in the studio"); }
		if (TemplateName.Contains(TEXT("Empty"))) { return LOCTEXT("EmptyTip", "A clear floor in daylight"); }
		return FText::GetEmpty();
	}
}

void SStageWelcomeDialog::Construct(const FArguments& InArgs)
{
	using namespace StageWelcomeDialog;
	OnTemplatePicked = InArgs._OnTemplatePicked;
	OnContinue = InArgs._OnContinue;
	OnShowAtStartupChanged = InArgs._OnShowAtStartupChanged;

	const UStageCraftUITheme& Theme = StageCraftWidgetStyle::ResolveTheme(nullptr);

	TSharedRef<SUniformGridPanel> Grid = SNew(SUniformGridPanel).SlotPadding(FMargin(4.f));
	for (int32 Index = 0; Index < InArgs._TemplateNames.Num(); ++Index)
	{
		Grid->AddSlot(Index % Columns, Index / Columns)[MakeCard(InArgs._TemplateNames[Index])];
	}

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(Width)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
			.Padding(Padding)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Start a stage"))
					.Font(StageCraftWidgetStyle::MakeFont(14, TEXT("Bold")))
					.ColorAndOpacity(Theme.TextPrimary)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 12.f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Subtitle", "Pick a starter stage to build on, or carry on with the stage that is open. Starter stages open as a new, untitled stage."))
					.AutoWrapText(true)
					.ColorAndOpacity(Theme.TextSecondary)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					InArgs._TemplateNames.IsEmpty()
						? StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(LOCTEXT("NoTemplates", "No starter stages are installed.")).ColorAndOpacity(Theme.WarningColor))
						: StaticCastSharedRef<SWidget>(Grid)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 16.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.f)
					.VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsChecked(InArgs._bShowAtStartup ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
						.OnCheckStateChanged_Lambda([OnChanged = OnShowAtStartupChanged](ECheckBoxState State) { OnChanged.ExecuteIfBound(State == ECheckBoxState::Checked); })
						[
							SNew(STextBlock).Text(LOCTEXT("ShowAtStartup", "Show at startup"))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(ContinueButton, SButton)
						.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("PrimaryButton"))
						.Text(LOCTEXT("Continue", "Continue"))
						.ToolTipText(LOCTEXT("ContinueTip", "Keep the stage that is open."))
						.OnClicked_Lambda([this]() { OnContinue.ExecuteIfBound(); return FReply::Handled(); })
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SStageWelcomeDialog::MakeCard(const FString& TemplateName)
{
	const UStageCraftUITheme& Theme = StageCraftWidgetStyle::ResolveTheme(nullptr);
	const FText Description = StageWelcomeDialog::Describe(TemplateName);
	return SNew(SButton)
		.ContentPadding(FMargin(12.f, 10.f))
		.ToolTipText(FText::Format(LOCTEXT("CardTip", "Start a new stage from \"{0}\"."), FText::FromString(TemplateName)))
		.OnClicked_Lambda([this, TemplateName]() { OnTemplatePicked.ExecuteIfBound(TemplateName); return FReply::Handled(); })
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TemplateName))
				.Font(StageCraftWidgetStyle::MakeFont(11, TEXT("Bold")))
				.ColorAndOpacity(Theme.TextPrimary)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 3.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(Description)
				.Visibility(Description.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.AutoWrapText(true)
				.Font(StageCraftWidgetStyle::MakeFont(8))
				.ColorAndOpacity(Theme.TextSecondary)
			]
		];
}

FReply SStageWelcomeDialog::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		OnContinue.ExecuteIfBound();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

#undef LOCTEXT_NAMESPACE
