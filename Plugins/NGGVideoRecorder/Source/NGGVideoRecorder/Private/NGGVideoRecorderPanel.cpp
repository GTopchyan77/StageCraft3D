// Copyright NGG. All Rights Reserved.

#include "NGGVideoRecorderPanel.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Misc/Paths.h"
#include "NGGVideoRecorderSettings.h"
#include "NGGVideoRecorderSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NGGVideoRecorder"

namespace NGGRecorderPanelStyle
{
	const FLinearColor PanelFill(0.008f, 0.010f, 0.016f, 0.92f);
	const FLinearColor PanelOutline(0.035f, 0.045f, 0.065f, 1.0f);

	const FLinearColor DotIdle(0.25f, 0.26f, 0.28f, 1.0f);
	const FLinearColor DotRecording(0.87f, 0.12f, 0.10f, 1.0f);

	const FLinearColor RecordNormal(0.55f, 0.06f, 0.05f, 1.0f);
	const FLinearColor RecordHovered(0.72f, 0.10f, 0.08f, 1.0f);
	const FLinearColor RecordPressed(0.40f, 0.05f, 0.04f, 1.0f);

	const FLinearColor ExportNormal(0.055f, 0.07f, 0.10f, 1.0f);
	const FLinearColor ExportHovered(0.10f, 0.13f, 0.18f, 1.0f);
	const FLinearColor ExportPressed(0.035f, 0.045f, 0.065f, 1.0f);

	const FLinearColor TextPrimary(0.85f, 0.87f, 0.90f, 1.0f);
	const FLinearColor TextSecondary(0.33f, 0.36f, 0.40f, 1.0f);

	FButtonStyle MakeButtonStyle(const FLinearColor& Normal, const FLinearColor& Hovered, const FLinearColor& Pressed)
	{
		return FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(Normal, 6.0f))
			.SetHovered(FSlateRoundedBoxBrush(Hovered, 6.0f))
			.SetPressed(FSlateRoundedBoxBrush(Pressed, 6.0f))
			.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(Normal.R, Normal.G, Normal.B, 0.35f), 6.0f))
			.SetNormalPadding(FMargin(14.0f, 8.0f))
			.SetPressedPadding(FMargin(14.0f, 9.0f, 14.0f, 7.0f));
	}

	TSharedRef<SWidget> MakePanelButton(const FButtonStyle* Style, TAttribute<FText> Label,
		const FLinearColor& LabelColor, FText Tooltip, FOnClicked OnClicked,
		TAttribute<bool> Enabled = true)
	{
		return SNew(SBox)
			.WidthOverride(124.0f)
			.HeightOverride(34.0f)
			[
				SNew(SButton)
				.ButtonStyle(Style)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.ToolTipText(MoveTemp(Tooltip))
				.IsEnabled(MoveTemp(Enabled))
				.OnClicked(MoveTemp(OnClicked))
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					.ColorAndOpacity(FSlateColor(LabelColor))
					.Text(MoveTemp(Label))
				]
			];
	}
}

void SNGGRecorderPanel::Construct(const FArguments& /*InArgs*/, UNGGVideoRecorderSubsystem* InSubsystem)
{
	using namespace NGGRecorderPanelStyle;

	Subsystem = InSubsystem;

	PanelBrush = FSlateRoundedBoxBrush(PanelFill, 12.0f, PanelOutline, 1.0f);
	DotBrush = FSlateRoundedBoxBrush(FLinearColor::White, 6.0f, FVector2f(12.0f, 12.0f));
	RecordButtonStyle = MakeButtonStyle(RecordNormal, RecordHovered, RecordPressed);
	ExportButtonStyle = MakeButtonStyle(ExportNormal, ExportHovered, ExportPressed);

	SetVisibility(MakeAttributeSP(this, &SNGGRecorderPanel::GetPanelVisibility));

	// The widget is given the whole viewport by AddViewportWidgetContent; the slot alignment parks
	// the panel bottom-right, and SelfHitTestInvisible keeps the empty rest of the screen click-through.
	ChildSlot
	.HAlign(HAlign_Right)
	.VAlign(VAlign_Bottom)
	.Padding(FMargin(0.0f, 0.0f, 24.0f, 24.0f))
	[
		SNew(SBorder)
		.BorderImage(&PanelBrush)
		.Padding(FMargin(16.0f, 12.0f, 16.0f, 14.0f))
		[
			SNew(SVerticalBox)

			// Status row: recording dot + "Ready" / "REC 00:12"
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SImage)
					.Image(&DotBrush)
					.ColorAndOpacity(this, &SNGGRecorderPanel::GetDotColor)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
					.ColorAndOpacity(FSlateColor(TextPrimary))
					.Text(this, &SNGGRecorderPanel::GetStatusText)
				]
			]

			// Last recording file name
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 2.0f)
			[
				SNew(SBox)
				.MaxDesiredWidth(280.0f)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(TextSecondary))
					.WrapTextAt(272.0f)
					.Text(this, &SNGGRecorderPanel::GetLastFileText)
					.ToolTipText(this, &SNGGRecorderPanel::GetLastFileTooltip)
				]
			]

			// Buttons row
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					MakePanelButton(&RecordButtonStyle,
						MakeAttributeSP(this, &SNGGRecorderPanel::GetRecordLabel),
						FLinearColor::White,
						LOCTEXT("RecordTooltip", "Start / stop recording the viewport to an MP4 (same as the recording hotkey)"),
						FOnClicked::CreateSP(this, &SNGGRecorderPanel::OnRecordClicked))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakePanelButton(&ExportButtonStyle,
						LOCTEXT("ExportLabel", "Export"),
						TextPrimary,
						LOCTEXT("ExportTooltip", "Show the exported MP4 in Windows Explorer"),
						FOnClicked::CreateSP(this, &SNGGRecorderPanel::OnExportClicked),
						MakeAttributeSP(this, &SNGGRecorderPanel::IsExportEnabled))
				]
			]
		]
	];
}

FReply SNGGRecorderPanel::OnRecordClicked()
{
	if (UNGGVideoRecorderSubsystem* Recorder = Subsystem.Get())
	{
		Recorder->ToggleRecording();
	}
	return FReply::Handled();
}

FReply SNGGRecorderPanel::OnExportClicked()
{
	if (UNGGVideoRecorderSubsystem* Recorder = Subsystem.Get())
	{
		Recorder->RevealLastRecordingInExplorer();
	}
	return FReply::Handled();
}

bool SNGGRecorderPanel::IsRecording() const
{
	const UNGGVideoRecorderSubsystem* Recorder = Subsystem.Get();
	return Recorder && Recorder->IsRecording();
}

const FString& SNGGRecorderPanel::GetLastPath() const
{
	static const FString Empty;
	const UNGGVideoRecorderSubsystem* Recorder = Subsystem.Get();
	return Recorder ? Recorder->GetLastRecordingPathRef() : Empty;
}

void SNGGRecorderPanel::RefreshFileTexts() const
{
	const FString& Path = GetLastPath();
	if (bFileTextsCached && Path == CachedPath)
	{
		return;
	}

	CachedPath = Path;
	bFileTextsCached = true;

	if (Path.IsEmpty())
	{
		CachedFileText = LOCTEXT("NoRecordingYet", "No recording yet");
		CachedFileTooltip = FText::GetEmpty();
	}
	else
	{
		CachedFileText = FText::FromString(FPaths::GetCleanFilename(Path));
		CachedFileTooltip = FText::FromString(Path);
	}
}

FText SNGGRecorderPanel::GetStatusText() const
{
	const UNGGVideoRecorderSubsystem* Recorder = Subsystem.Get();
	if (!Recorder || !Recorder->IsRecording())
	{
		return LOCTEXT("StatusReady", "Ready");
	}

	const int32 TotalSeconds = (int32)Recorder->GetRecordingDuration();
	if (TotalSeconds != CachedStatusSeconds)
	{
		CachedStatusSeconds = TotalSeconds;
		CachedStatusText = FText::FromString(TEXT("REC  ") + Recorder->GetRecordingTimecode());
	}
	return CachedStatusText;
}

FText SNGGRecorderPanel::GetRecordLabel() const
{
	return IsRecording() ? LOCTEXT("RecordLabelStop", "Stop") : LOCTEXT("RecordLabelRecord", "Record");
}

FText SNGGRecorderPanel::GetLastFileText() const
{
	RefreshFileTexts();
	return CachedFileText;
}

FText SNGGRecorderPanel::GetLastFileTooltip() const
{
	RefreshFileTexts();
	return CachedFileTooltip;
}

FSlateColor SNGGRecorderPanel::GetDotColor() const
{
	using namespace NGGRecorderPanelStyle;
	return FSlateColor(IsRecording() ? DotRecording : DotIdle);
}

bool SNGGRecorderPanel::IsExportEnabled() const
{
#if UE_BUILD_SHIPPING
	// RevealLastRecordingInExplorer is a no-op in shipping builds.
	return false;
#else
	return !IsRecording() && !GetLastPath().IsEmpty();
#endif
}

EVisibility SNGGRecorderPanel::GetPanelVisibility() const
{
	if (UNGGVideoRecorderSettings::Get().bHidePanelWhileRecording && IsRecording())
	{
		return EVisibility::Collapsed;
	}
	// The widget spans the viewport; only its children may take clicks, the empty area must not.
	return EVisibility::SelfHitTestInvisible;
}

#undef LOCTEXT_NAMESPACE
