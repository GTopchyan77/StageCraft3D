// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageRenderPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Player/ModularPlayerController.h"
#include "Render/StageRenderSubsystem.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageRenderPanel)

#define LOCTEXT_NAMESPACE "StageRenderPanel"

namespace StageRenderPanel
{
	constexpr float SectionGap = 10.f;
	constexpr float ChoiceGap = 3.f;

	FText DescribeState(EStageRenderState State)
	{
		switch (State)
		{
		case EStageRenderState::Capturing:
			return LOCTEXT("Capturing", "Rendering frames...");
		case EStageRenderState::Reading:
			return LOCTEXT("Reading", "Reading the image from the GPU...");
		case EStageRenderState::Encoding:
			return LOCTEXT("Encoding", "Writing PNG...");
		case EStageRenderState::Idle:
		default:
			return FText::GetEmpty();
		}
	}
}

const UStageCraftUITheme& UStageRenderPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

void UStageRenderPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	if (RenderButton)
	{
		RenderButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleRenderClicked);
	}
	if (OpenFolderButton)
	{
		OpenFolderButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleOpenFolderClicked);
	}
}

UTextBlock* UStageRenderPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void UStageRenderPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildHeader());

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("HeaderDivider"));
	Divider->SetBrush(MakeBox(Palette.Divider));
	Divider->SetDesiredSizeOverride(FVector2D(1.f, 1.f));
	Column->AddChildToVerticalBox(Divider);

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Body"));
	UBorder* BodyPadding = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BodyPadding"));
	BodyPadding->SetBrush(MakeBox(FLinearColor::Transparent));
	BodyPadding->SetPadding(FMargin(10.f, 8.f));
	BodyPadding->SetContent(Body);
	Scroll->AddChild(BodyPadding);

	const auto AddSection = [Body](UWidget* Section)
	{
		if (UVerticalBoxSlot* SectionSlot = Body->AddChildToVerticalBox(Section))
		{
			SectionSlot->SetPadding(FMargin(0.f, 0.f, 0.f, StageRenderPanel::SectionGap));
		}
	};

	AddSection(BuildChoiceRow(LOCTEXT("Resolution", "RESOLUTION"),
		{ LOCTEXT("Res4K", "4K UHD"), LOCTEXT("ResHD", "1080p"), LOCTEXT("ResSquare", "Square 1:1") },
		{ LOCTEXT("Res4KTip", "3840 x 2160, for print and large screens."), LOCTEXT("ResHDTip", "1920 x 1080 Full HD."),
		  LOCTEXT("ResSquareTip", "2160 x 2160, for social posts and slides.") },
		ResolutionButtons, &ThisClass::HandleResolutionPicked));
	AddSection(BuildChoiceRow(LOCTEXT("AntiAliasing", "ANTI-ALIASING"),
		{ LOCTEXT("AAOff", "Off"), LOCTEXT("AAFXAA", "FXAA"), LOCTEXT("AATemporal", "Temporal"), LOCTEXT("AASuper", "Supersampled") },
		{ LOCTEXT("AAOffTip", "No smoothing: hard edges, fastest."), LOCTEXT("AAFXAATip", "Fast single-frame smoothing."),
		  LOCTEXT("AATemporalTip", "The viewport's temporal anti-aliasing, settled over 16 frames. Recommended."),
		  LOCTEXT("AASuperTip", "Temporal, rendered larger and filtered down: the cleanest edges. Up to one 4K frame of pixels: 2x for 1080p, 1.33x for Square; 4K renders like Temporal.") },
		AntiAliasingButtons, &ThisClass::HandleAntiAliasingPicked));
	AddSection(BuildChoiceRow(LOCTEXT("PostProcess", "POST-PROCESSING"),
		{ LOCTEXT("PPClean", "Clean"), LOCTEXT("PPStandard", "Standard"), LOCTEXT("PPCinematic", "Cinematic") },
		{ LOCTEXT("PPCleanTip", "Neutral technical image: no vignette, grain, fringe, lens flares or bloom."),
		  LOCTEXT("PPStandardTip", "The level's look, exactly as in the viewport."),
		  LOCTEXT("PPCinematicTip", "Standard with higher-quality lighting, reflections and ambient occlusion, and a longer warm-up.") },
		PostProcessButtons, &ThisClass::HandlePostProcessPicked));
	AddSection(BuildChoiceRow(LOCTEXT("Watermark", "WATERMARK"),
		{ LOCTEXT("WatermarkOff", "Off"), LOCTEXT("WatermarkOn", "Scene info footer") },
		{ LOCTEXT("WatermarkOffTip", "The image only."),
		  LOCTEXT("WatermarkOnTip", "Adds a footer with the scene name, date, resolution and item count.") },
		WatermarkButtons, &ThisClass::HandleWatermarkPicked));

	SummaryText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextSecondary);
	SummaryText->SetAutoWrapText(true);
	AddSection(SummaryText);

	RenderButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("RenderButton"));
	RenderButton->SetStyle(MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f));
	RenderButton->SetToolTipText(LOCTEXT("RenderTip", "Render the current view to a PNG in the Renders folder. You can keep working while it renders."));
	RenderButtonText = MakeText(LOCTEXT("RenderLabel", "RENDER IMAGE"), 9, TEXT("Bold"), Palette.WindowBackground);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(RenderButton->AddChild(RenderButtonText)))
	{
		LabelSlot->SetPadding(FMargin(12.f, 7.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	AddSection(RenderButton);

	StatusText = MakeText(FText::GetEmpty(), 8, TEXT("Regular"), Palette.TextPrimary);
	StatusText->SetAutoWrapText(true);
	AddSection(StatusText);

	UHorizontalBox* FolderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("FolderRow"));
	FolderText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	FolderText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UHorizontalBoxSlot* PathSlot = FolderRow->AddChildToHorizontalBox(FolderText))
	{
		PathSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PathSlot->SetVerticalAlignment(VAlign_Center);
	}
	OpenFolderButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("OpenFolderButton"));
	FButtonStyle FolderStyle = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	FolderStyle.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	OpenFolderButton->SetStyle(FolderStyle);
	OpenFolderButton->SetToolTipText(LOCTEXT("OpenFolderTip", "Open the Renders folder in the file browser."));
	if (UButtonSlot* FolderLabelSlot = Cast<UButtonSlot>(OpenFolderButton->AddChild(MakeText(LOCTEXT("OpenFolder", "OPEN FOLDER"), 7, TEXT("Bold"), Palette.TextSecondary))))
	{
		FolderLabelSlot->SetPadding(FMargin(8.f, 3.f));
	}
	if (UHorizontalBoxSlot* ButtonSlot = FolderRow->AddChildToHorizontalBox(OpenFolderButton))
	{
		ButtonSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
		ButtonSlot->SetVerticalAlignment(VAlign_Center);
	}
	AddSection(FolderRow);
}

UWidget* UStageRenderPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 8.f));
	UTextBlock* Title = MakeText(LOCTEXT("Title", "RENDER"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "High-resolution stills of the current view."));
	Header->SetContent(Title);
	return Header;
}

UWidget* UStageRenderPanel::BuildChoiceRow(const FText& Title, TConstArrayView<FText> Labels, TConstArrayView<FText> Tips,
	TArray<TObjectPtr<UStageChoiceButton>>& OutButtons, void (UStageRenderPanel::*Handler)(int32))
{
	const UStageCraftUITheme& Palette = GetTheme();

	UVerticalBox* Section = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UVerticalBoxSlot* TitleSlot = Section->AddChildToVerticalBox(MakeText(Title, 7, TEXT("Bold"), Palette.TextSecondary)))
	{
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Section->AddChildToVerticalBox(Row);

	OutButtons.Reset(Labels.Num());
	for (int32 Index = 0; Index < Labels.Num(); ++Index)
	{
		UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Button->SetChoiceIndex(Index);
		Button->OnPicked.BindUObject(this, Handler);
		if (Tips.IsValidIndex(Index))
		{
			Button->SetToolTipText(Tips[Index]);
		}

		UTextBlock* Label = MakeText(Labels[Index], 8, TEXT("Bold"), Palette.TextSecondary);
		if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(Label)))
		{
			LabelSlot->SetPadding(FMargin(4.f, 4.f));
			LabelSlot->SetHorizontalAlignment(HAlign_Center);
		}
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(Index == 0 ? 0.f : StageRenderPanel::ChoiceGap, 0.f, 0.f, 0.f));
		}
		OutButtons.Add(Button);
	}
	return Section;
}

void UStageRenderPanel::NativeConstruct()
{
	Super::NativeConstruct();

	UStageRenderSubsystem* Subsystem = UWorld::GetSubsystem<UStageRenderSubsystem>(GetWorld());
	RenderSubsystem = Subsystem;
	if (Subsystem)
	{
		Subsystem->OnSettingsChanged.AddUniqueDynamic(this, &ThisClass::HandleSettingsChanged);
		Subsystem->OnRenderStateChanged.AddUniqueDynamic(this, &ThisClass::HandleRenderStateChanged);
		Subsystem->OnRenderFinished.AddUniqueDynamic(this, &ThisClass::HandleRenderFinished);
	}
	RefreshSettings();
	RefreshJob();
}

void UStageRenderPanel::NativeDestruct()
{
	if (UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		Subsystem->OnSettingsChanged.RemoveDynamic(this, &ThisClass::HandleSettingsChanged);
		Subsystem->OnRenderStateChanged.RemoveDynamic(this, &ThisClass::HandleRenderStateChanged);
		Subsystem->OnRenderFinished.RemoveDynamic(this, &ThisClass::HandleRenderFinished);
	}
	RenderSubsystem.Reset();
	Super::NativeDestruct();
}

void UStageRenderPanel::StyleChoice(UStageChoiceButton& Button, bool bSelected) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	// Selected: solid accent like a lit console key; the others outlined, so they still read as buttons.
	FButtonStyle Style = bSelected
		? MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f)
		: MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	if (!bSelected)
	{
		Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	}
	Button.SetStyle(Style);
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetContent()))
	{
		Label->SetColorAndOpacity(FSlateColor(bSelected ? Palette.WindowBackground : Palette.TextSecondary));
	}
}

void UStageRenderPanel::RefreshSettings()
{
	const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get();
	const FStageRenderSettings Settings = Subsystem ? Subsystem->GetSettings() : FStageRenderSettings();

	const auto StyleGroup = [this](TArray<TObjectPtr<UStageChoiceButton>>& Buttons, int32 SelectedIndex)
	{
		for (UStageChoiceButton* Button : Buttons)
		{
			if (Button)
			{
				StyleChoice(*Button, Button->GetChoiceIndex() == SelectedIndex);
			}
		}
	};
	StyleGroup(ResolutionButtons, int32(Settings.Resolution));
	StyleGroup(AntiAliasingButtons, int32(Settings.AntiAliasing));
	StyleGroup(PostProcessButtons, int32(Settings.PostProcess));
	StyleGroup(WatermarkButtons, Settings.bWatermark ? 1 : 0);

	if (SummaryText)
	{
		const FIntPoint Output = StageRender::GetOutputSize(Settings.Resolution);
		const FIntPoint Internal = StageRender::GetInternalSize(Settings);
		FNumberFormattingOptions TwoDecimals;
		TwoDecimals.SetMaximumFractionalDigits(2);
		const FText Size = FText::Format(LOCTEXT("Size", "{0} x {1} px"), FText::AsNumber(Output.X), FText::AsNumber(Output.Y));
		const bool bSupersampleAtBudget = Settings.AntiAliasing == EStageRenderAntiAliasing::Supersampled && Internal == Output;
		SummaryText->SetText(bSupersampleAtBudget
			? FText::Format(LOCTEXT("SummaryAtBudget", "{0}  ·  already at the 4K pixel budget, so it renders like Temporal  ·  {1} warm-up frames"),
				Size, FText::AsNumber(StageRender::GetWarmupFrames(Settings)))
			: Internal == Output
			? FText::Format(LOCTEXT("Summary", "{0}  ·  {1} warm-up frames"), Size, FText::AsNumber(StageRender::GetWarmupFrames(Settings)))
			: FText::Format(LOCTEXT("SummarySS", "{0}  ·  rendered at {1} x {2} ({3}x) and filtered down  ·  {4} warm-up frames"), Size,
				FText::AsNumber(Internal.X), FText::AsNumber(Internal.Y), FText::AsNumber(StageRender::GetSupersampleFactor(Settings), &TwoDecimals),
				FText::AsNumber(StageRender::GetWarmupFrames(Settings))));
	}
	if (FolderText && Subsystem)
	{
		FolderText->SetText(FText::FromString(Subsystem->GetOutputDirectory()));
		FolderText->SetToolTipText(FText::FromString(Subsystem->GetOutputDirectory()));
	}
}

void UStageRenderPanel::RefreshJob()
{
	const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get();
	const bool bRendering = Subsystem && Subsystem->IsRendering();
	if (RenderButton)
	{
		RenderButton->SetIsEnabled(Subsystem && !bRendering);
	}
	if (RenderButtonText)
	{
		RenderButtonText->SetText(bRendering ? LOCTEXT("RenderBusy", "RENDERING...") : LOCTEXT("RenderLabel", "RENDER IMAGE"));
	}
	if (!StatusText)
	{
		return;
	}

	const UStageCraftUITheme& Palette = GetTheme();
	if (!Subsystem)
	{
		StatusText->SetText(LOCTEXT("Unavailable", "Rendering is available while a stage is open."));
		StatusText->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		return;
	}
	if (bRendering)
	{
		StatusText->SetText(StageRenderPanel::DescribeState(Subsystem->GetState()));
		StatusText->SetColorAndOpacity(FSlateColor(Palette.TextPrimary));
		return;
	}

	const FStageRenderResult& Last = Subsystem->GetLastResult();
	FNumberFormattingOptions OneDecimal;
	OneDecimal.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
	StatusText->SetText(Last.Message.IsEmpty() ? FText::GetEmpty()
		: (Last.bSucceeded ? FText::Format(LOCTEXT("LastOk", "{0}  ({1} s)"), Last.Message, FText::AsNumber(Last.Seconds, &OneDecimal)) : Last.Message));
	StatusText->SetColorAndOpacity(FSlateColor(Last.bSucceeded ? Palette.TextPrimary : Palette.WarningColor));
}

void UStageRenderPanel::ApplySettings(const FStageRenderSettings& Settings)
{
	// The single write path; the panel updates from OnSettingsChanged, not from what it asked for.
	if (UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		Subsystem->SetSettings(Settings);
	}
}

void UStageRenderPanel::HandleResolutionPicked(int32 Index)
{
	if (const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		FStageRenderSettings Settings = Subsystem->GetSettings();
		Settings.Resolution = static_cast<EStageRenderResolution>(Index);
		ApplySettings(Settings);
	}
}

void UStageRenderPanel::HandleAntiAliasingPicked(int32 Index)
{
	if (const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		FStageRenderSettings Settings = Subsystem->GetSettings();
		Settings.AntiAliasing = static_cast<EStageRenderAntiAliasing>(Index);
		ApplySettings(Settings);
	}
}

void UStageRenderPanel::HandlePostProcessPicked(int32 Index)
{
	if (const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		FStageRenderSettings Settings = Subsystem->GetSettings();
		Settings.PostProcess = static_cast<EStageRenderPostProcess>(Index);
		ApplySettings(Settings);
	}
}

void UStageRenderPanel::HandleWatermarkPicked(int32 Index)
{
	if (const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		FStageRenderSettings Settings = Subsystem->GetSettings();
		Settings.bWatermark = Index == 1;
		ApplySettings(Settings);
	}
}

void UStageRenderPanel::HandleRenderClicked()
{
	// The request bridge reports a refusal itself (status bar warning + error cue).
	if (AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer()))
	{
		Controller->RequestRender();
	}
}

void UStageRenderPanel::HandleOpenFolderClicked()
{
	if (const UStageRenderSubsystem* Subsystem = RenderSubsystem.Get())
	{
		const FString Directory = Subsystem->GetOutputDirectory();
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		FPlatformProcess::ExploreFolder(*Directory);
	}
}

void UStageRenderPanel::HandleSettingsChanged(const FStageRenderSettings& Settings)
{
	RefreshSettings();
}

void UStageRenderPanel::HandleRenderStateChanged(EStageRenderState NewState)
{
	RefreshJob();
}

void UStageRenderPanel::HandleRenderFinished(const FStageRenderResult& Result)
{
	RefreshJob();
}

#undef LOCTEXT_NAMESPACE
