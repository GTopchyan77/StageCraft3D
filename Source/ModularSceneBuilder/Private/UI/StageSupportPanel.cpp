// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageSupportPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Cloud/StageCloudSubsystem.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Support/StageSupportSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageThemedComboBox.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageSupportPanel)

#define LOCTEXT_NAMESPACE "StageSupportPanel"

const UStageCraftUITheme& UStageSupportPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

UStageSupportSubsystem* UStageSupportPanel::GetSupport() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UStageSupportSubsystem>() : nullptr;
}

UTextBlock* UStageSupportPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

UEditableTextBox* UStageSupportPanel::MakeField(const FText& Hint)
{
	UEditableTextBox* Field = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	Field->SetWidgetStyle(StageCraftWidgetStyle::MakeInputField(GetTheme()));
	Field->SetHintText(Hint);
	return Field;
}

UMultiLineEditableTextBox* UStageSupportPanel::MakeMultiLine(const FText& Hint)
{
	UMultiLineEditableTextBox* Field = WidgetTree->ConstructWidget<UMultiLineEditableTextBox>(UMultiLineEditableTextBox::StaticClass());
	// Set before the Slate widget exists: UMultiLineEditableTextBox has no style setter.
	Field->WidgetStyle = StageCraftWidgetStyle::MakeInputField(GetTheme());
	Field->SetHintText(Hint);
	return Field;
}

UStageChoiceButton* UStageSupportPanel::MakeButton(const FText& Label, const FText& Tip, int32 Index, bool bAccent)
{
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetChoiceIndex(Index);
	Button->SetToolTipText(Tip);
	Button->OnPicked.BindUObject(this, &ThisClass::HandleAction);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), GetTheme().TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 3.f));
	}
	ActionButtons[Index] = Button;
	StyleToggle(static_cast<EAction>(Index), bAccent);
	return Button;
}

void UStageSupportPanel::StyleToggle(EAction Action, bool bOn)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	UStageChoiceButton* Button = ActionButtons[int32(Action)];
	FButtonStyle Style = bOn ? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bOn)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button->SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bOn ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

UWidget* UStageSupportPanel::Labeled(const FText& Label, UWidget* Field, float Height)
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Box->AddChildToVerticalBox(MakeText(Label, 7, TEXT("Bold"), GetTheme().TextSecondary))->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	if (Height > 0.f)
	{
		USizeBox* Sized = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Sized->SetHeightOverride(Height);
		Sized->SetContent(Field);
		Field = Sized;
	}
	Box->AddChildToVerticalBox(Field);
	return Box;
}

void UStageSupportPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageSupportPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	ActionButtons.SetNum(int32(EAction::Count));

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	WidgetTree->RootWidget = Root;
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Root->SetContent(Scroll);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(Column);
	const auto Add = [Column](UWidget* Widget, float Top = 8.f) { Column->AddChildToVerticalBox(Widget)->SetPadding(FMargin(12.f, Top, 12.f, 0.f)); };

	Add(MakeText(LOCTEXT("Title", "SUPPORT & BUG REPORT"), 9, TEXT("Bold"), Palette.TextPrimary), 10.f);
	UTextBlock* Intro = MakeText(LOCTEXT("Intro", "Tell us what went wrong or what you need. You decide whether the log and a screenshot go with it; API keys, e-mail addresses and your Windows user name are removed first."),
		8, TEXT("Regular"), Palette.TextSecondary);
	Intro->SetAutoWrapText(true);
	Add(Intro, 4.f);

	CategoryCombo = WidgetTree->ConstructWidget<UStageThemedComboBox>(UStageThemedComboBox::StaticClass());
	CategoryCombo->ApplyTheme(Palette);
	for (int32 Index = 0; Index < int32(EStageSupportCategory::Count); ++Index)
	{
		CategoryCombo->AddOption(StageSupportReport::GetCategoryDisplayName(EStageSupportCategory(Index)).ToString());
	}
	CategoryCombo->SetSelectedIndex(0);
	Add(Labeled(LOCTEXT("Category", "ABOUT"), CategoryCombo));
	TitleField = MakeField(LOCTEXT("TitleHint", "A short title, e.g. \"Export to Blender stops at 50 %\""));
	Add(Labeled(LOCTEXT("TitleLabel", "TITLE"), TitleField));
	DescriptionField = MakeMultiLine(LOCTEXT("DescriptionHint", "What happened? What were you doing?"));
	Add(Labeled(LOCTEXT("Description", "WHAT HAPPENED"), DescriptionField, 90.f));
	StepsField = MakeMultiLine(LOCTEXT("StepsHint", "1. Open the Concert starter stage\n2. ..."));
	Add(Labeled(LOCTEXT("Steps", "STEPS TO REPRODUCE (OPTIONAL)"), StepsField, 70.f));
	ExpectedField = MakeField(LOCTEXT("ExpectedHint", "What you expected"));
	Add(Labeled(LOCTEXT("Expected", "EXPECTED (OPTIONAL)"), ExpectedField));
	ActualField = MakeField(LOCTEXT("ActualHint", "What happened instead"));
	Add(Labeled(LOCTEXT("Actual", "ACTUAL (OPTIONAL)"), ActualField));
	EmailField = MakeField(LOCTEXT("EmailHint", "Only if you want an answer and are not signed in"));
	Add(Labeled(LOCTEXT("Email", "YOUR E-MAIL (OPTIONAL)"), EmailField));

	UHorizontalBox* Attach = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Attach->AddChildToHorizontalBox(MakeButton(LOCTEXT("IncludeLog", "INCLUDE LOG"),
		LOCTEXT("IncludeLogTip", "Attach the end of this session's log (at most 256 KB), with keys, e-mail addresses and your user name removed."), int32(EAction::IncludeLog), false));
	Attach->AddChildToHorizontalBox(MakeButton(LOCTEXT("IncludeScreenshot", "INCLUDE SCREENSHOT"),
		LOCTEXT("IncludeScreenshotTip", "Attach a picture of the StageCraft window as it is now. Close anything private first."), int32(EAction::IncludeScreenshot), false))
		->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	Add(Attach, 10.f);
	AttachText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	AttachText->SetAutoWrapText(true);
	Add(AttachText, 4.f);

	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Footer->AddChildToHorizontalBox(MakeButton(LOCTEXT("Send", "SEND"), LOCTEXT("SendTip", "Send to StageCraft support, or save on this computer when offline."), int32(EAction::Send), true));
	Footer->AddChildToHorizontalBox(MakeButton(LOCTEXT("OpenFolder", "OPEN FOLDER"), LOCTEXT("OpenFolderTip", "Show the saved report in Explorer."), int32(EAction::OpenFolder), false))
		->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	Footer->AddChildToHorizontalBox(MakeButton(LOCTEXT("New", "NEW REPORT"), LOCTEXT("NewTip", "Clear the form."), int32(EAction::New), false))
		->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	Add(Footer, 12.f);
	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	Column->AddChildToVerticalBox(StatusText)->SetPadding(FMargin(12.f, 6.f, 12.f, 12.f));
}

void UStageSupportPanel::NativeConstruct()
{
	Super::NativeConstruct();
	Refresh();
}

void UStageSupportPanel::Refresh()
{
	const UStageSupportSubsystem* Support = GetSupport();
	const UGameInstance* GameInstance = GetGameInstance();
	const UStageCloudSubsystem* Cloud = GameInstance ? GameInstance->GetSubsystem<UStageCloudSubsystem>() : nullptr;
	const bool bOnline = Cloud && Cloud->IsOnline();
	StyleToggle(EAction::IncludeLog, bIncludeLog);
	StyleToggle(EAction::IncludeScreenshot, bIncludeScreenshot);
	StyleToggle(EAction::Send, true);
	EmailField->SetVisibility(Cloud && Cloud->IsSignedIn() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	TArray<FText> Parts;
	Parts.Add(LOCTEXT("AlwaysSent", "Sent with every report: app version, Windows version, graphics card and the number of items and fixtures on stage."));
	if (bIncludeLog)
	{
		Parts.Add(FText::Format(LOCTEXT("LogSize", "Log: {0} KB, redacted."), FText::AsNumber(FMath::DivideAndRoundUp(LogTail.Len(), 1024))));
	}
	if (bIncludeScreenshot)
	{
		Parts.Add(FText::Format(LOCTEXT("ShotSize", "Screenshot: {0} KB."), FText::AsNumber(FMath::DivideAndRoundUp(ScreenshotPng.Num(), 1024))));
	}
	Parts.Add(bOnline ? LOCTEXT("ToCloud", "Goes to StageCraft support (StageCraft Cloud).") : LOCTEXT("ToDisk", "StageCraft Cloud is offline: the report is saved on this computer for you to send."));
	AttachText->SetText(FText::Join(FText::FromString(TEXT(" ")), Parts));
	ActionButtons[int32(EAction::Send)]->SetIsEnabled(Support && !Support->IsSubmitting());
	ActionButtons[int32(EAction::OpenFolder)]->SetVisibility(SavedFolder.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
}

void UStageSupportPanel::HandleAction(int32 Index)
{
	UStageSupportSubsystem* Support = GetSupport();
	if (!Support)
	{
		return;
	}
	switch (static_cast<EAction>(Index))
	{
	case EAction::IncludeLog:
		bIncludeLog = !bIncludeLog;
		LogTail = bIncludeLog ? Support->ReadLogTail() : FString();
		break;
	case EAction::IncludeScreenshot:
		bIncludeScreenshot = !bIncludeScreenshot;
		ScreenshotPng.Reset();
		if (bIncludeScreenshot)
		{
			FText Error;
			if (!Support->CaptureScreenshot(ScreenshotPng, Error))
			{
				bIncludeScreenshot = false;
				StatusText->SetText(Error);
			}
		}
		break;
	case EAction::OpenFolder:
		if (!SavedFolder.IsEmpty())
		{
			FPlatformProcess::ExploreFolder(*SavedFolder);
		}
		break;
	case EAction::New:
		ResetForm();
		break;
	case EAction::Send:
	{
		FStageSupportReport Report = Support->MakeDraft(GetWorld());
		Report.Category = EStageSupportCategory(FMath::Clamp(CategoryCombo->GetSelectedIndex(), 0, int32(EStageSupportCategory::Count) - 1));
		Report.Title = TitleField->GetText().ToString();
		Report.Description = DescriptionField->GetText().ToString();
		Report.Steps = StepsField->GetText().ToString();
		Report.Expected = ExpectedField->GetText().ToString();
		Report.Actual = ActualField->GetText().ToString();
		Report.ContactEmail = EmailField->GetVisibility() == ESlateVisibility::Collapsed ? FString() : EmailField->GetText().ToString().TrimStartAndEnd();
		Report.bIncludeLog = bIncludeLog;
		Report.LogTail = LogTail;
		Report.bIncludeScreenshot = bIncludeScreenshot;
		Report.ScreenshotPng = ScreenshotPng;
		FText Error;
		if (Support->Submit(MoveTemp(Report), FOnStageSupportSubmitted::CreateUObject(this, &ThisClass::HandleSubmitted), Error))
		{
			StatusText->SetText(LOCTEXT("Sending", "Sending..."));
			StatusText->SetColorAndOpacity(FSlateColor(GetTheme().TextSecondary));
		}
		else
		{
			StatusText->SetText(Error);
			StatusText->SetColorAndOpacity(FSlateColor(GetTheme().WarningColor));
		}
		break;
	}
	default:
		break;
	}
	Refresh();
}

void UStageSupportPanel::HandleSubmitted(const FStageSupportSubmitResult& Result)
{
	StatusText->SetText(Result.Message);
	StatusText->SetColorAndOpacity(FSlateColor(Result.bSent || Result.bSaved ? GetTheme().AccentColor : GetTheme().WarningColor));
	SavedFolder = Result.bSaved ? Result.Folder : FString();
	if (Result.bSent)
	{
		ResetForm();
		StatusText->SetText(Result.Message);
	}
	Refresh();
}

void UStageSupportPanel::ResetForm()
{
	for (UEditableTextBox* Field : { TitleField.Get(), ExpectedField.Get(), ActualField.Get() })
	{
		Field->SetText(FText::GetEmpty());
	}
	DescriptionField->SetText(FText::GetEmpty());
	StepsField->SetText(FText::GetEmpty());
	CategoryCombo->SetSelectedIndex(0);
	bIncludeLog = false;
	bIncludeScreenshot = false;
	LogTail.Reset();
	ScreenshotPng.Reset();
	StatusText->SetText(FText::GetEmpty());
}

#undef LOCTEXT_NAMESPACE
