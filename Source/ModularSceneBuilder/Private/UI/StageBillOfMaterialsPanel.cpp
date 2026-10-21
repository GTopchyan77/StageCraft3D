// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageBillOfMaterialsPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageBillOfMaterialsPanel)

#define LOCTEXT_NAMESPACE "StageBillOfMaterialsPanel"

namespace StageBillOfMaterialsPanel
{
	constexpr float QuantityColumnWidth = 44.f;
	constexpr float WeightColumnWidth = 70.f;
	constexpr float PowerColumnWidth = 64.f;
	constexpr float SectionGap = 8.f;
}

namespace StageReportFormat
{
	FText FormatWeight(double Kilograms)
	{
		FNumberFormattingOptions Options;
		if (Kilograms >= 1000.0)
		{
			Options.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
			return FText::Format(LOCTEXT("Tonnes", "{0} t"), FText::AsNumber(Kilograms / 1000.0, &Options));
		}
		Options.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
		return FText::Format(LOCTEXT("Kilograms", "{0} kg"), FText::AsNumber(Kilograms, &Options));
	}

	FText FormatPower(double Watts)
	{
		FNumberFormattingOptions Options;
		if (Watts >= 1000.0)
		{
			Options.SetMinimumFractionalDigits(2).SetMaximumFractionalDigits(2);
			return FText::Format(LOCTEXT("Kilowatts", "{0} kW"), FText::AsNumber(Watts / 1000.0, &Options));
		}
		Options.SetMaximumFractionalDigits(0);
		return FText::Format(LOCTEXT("Watts", "{0} W"), FText::AsNumber(Watts, &Options));
	}

	FText MakeLineDetail(const FStageBomLine& Line)
	{
		const bool bRuns = Line.Placements != Line.Quantity;
		if (Line.Manufacturer.IsEmpty())
		{
			return bRuns ? FText::Format(LOCTEXT("InRuns", "in {0} {0}|plural(one=run,other=runs)"), Line.Placements) : FText::GetEmpty();
		}
		return bRuns ? FText::Format(LOCTEXT("MakerInRuns", "{0} · in {1} {1}|plural(one=run,other=runs)"), Line.Manufacturer, Line.Placements) : Line.Manufacturer;
	}
}

const UStageCraftUITheme& UStageBillOfMaterialsPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

UTextBlock* UStageBillOfMaterialsPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void UStageBillOfMaterialsPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	if (ExportButton)
	{
		ExportButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleExportClicked);
	}
	if (OpenFolderButton)
	{
		OpenFolderButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleOpenFolderClicked);
	}
}

void UStageBillOfMaterialsPanel::BuildTree()
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
	if (UVerticalBoxSlot* SummarySlot = Column->AddChildToVerticalBox(BuildSummary()))
	{
		SummarySlot->SetPadding(FMargin(10.f, 8.f, 10.f, StageBillOfMaterialsPanel::SectionGap));
	}

	// Column titles, styled like a group row so the table reads as one grid.
	if (UVerticalBoxSlot* TitlesSlot = Column->AddChildToVerticalBox(MakeTableRow(LOCTEXT("ItemColumn", "ITEM"), FText::GetEmpty(),
		LOCTEXT("QuantityColumn", "QTY"), LOCTEXT("WeightColumn", "WEIGHT"), LOCTEXT("PowerColumn", "POWER"), ERowStyle::Group)))
	{
		TitlesSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 1.f));
	}

	EmptyText = MakeText(LOCTEXT("Empty", "The stage is empty. Placed items are listed here with their quantities, weight and power."), 8, TEXT("Italic"), Palette.TextSecondary);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	TableBody = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("TableBody"));
	Scroll->AddChild(TableBody);

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("FooterDivider"));
	Divider->SetBrush(MakeBox(Palette.Divider));
	Divider->SetDesiredSizeOverride(FVector2D(1.f, 1.f));
	Column->AddChildToVerticalBox(Divider);
	if (UVerticalBoxSlot* FooterSlot = Column->AddChildToVerticalBox(BuildFooter()))
	{
		FooterSlot->SetPadding(FMargin(10.f, 8.f));
	}
}

UWidget* UStageBillOfMaterialsPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 8.f));

	UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Header->SetContent(Lines);
	UTextBlock* Title = MakeText(LOCTEXT("Title", "BILL OF MATERIALS"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Everything on the stage, with quantities, combined weight and power draw, for production planning."));
	Lines->AddChildToVerticalBox(Title);
	SubtitleText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	Lines->AddChildToVerticalBox(SubtitleText);
	return Header;
}

UWidget* UStageBillOfMaterialsPanel::BuildSummary()
{
	UHorizontalBox* Tiles = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Summary"));
	const auto AddTile = [this, Tiles](const FText& Title, TObjectPtr<UTextBlock>& OutValue, bool bFirst)
	{
		if (UHorizontalBoxSlot* TileSlot = Tiles->AddChildToHorizontalBox(MakeTile(Title, OutValue)))
		{
			TileSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			TileSlot->SetPadding(FMargin(bFirst ? 0.f : 4.f, 0.f, 0.f, 0.f));
		}
	};
	AddTile(LOCTEXT("ItemsTile", "ITEMS"), ItemsValue, true);
	AddTile(LOCTEXT("UnitsTile", "UNITS"), UnitsValue, false);
	AddTile(LOCTEXT("WeightTile", "WEIGHT"), WeightValue, false);
	AddTile(LOCTEXT("PowerTile", "POWER"), PowerValue, false);

	ItemsValue->SetToolTipText(LOCTEXT("ItemsTip", "Placed items (a truss run counts once)."));
	UnitsValue->SetToolTipText(LOCTEXT("UnitsTip", "Catalog units to pick: pieces, and every segment of every run."));
	WeightValue->SetToolTipText(LOCTEXT("WeightTip", "Combined weight of everything on the stage."));
	PowerValue->SetToolTipText(LOCTEXT("PowerTip", "Combined maximum power draw."));
	return Tiles;
}

UWidget* UStageBillOfMaterialsPanel::MakeTile(const FText& Title, TObjectPtr<UTextBlock>& OutValue)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Tile = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Tile->SetBrush(MakeBox(Palette.RowBackground, CornerRadius, Palette.Divider, 1.f));
	Tile->SetPadding(FMargin(8.f, 5.f));

	UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Tile->SetContent(Lines);
	Lines->AddChildToVerticalBox(MakeText(Title, 6, TEXT("Bold"), Palette.TextSecondary));
	OutValue = MakeText(FText::GetEmpty(), 11, TEXT("Bold"), Palette.TextPrimary);
	Lines->AddChildToVerticalBox(OutValue);
	return Tile;
}

UWidget* UStageBillOfMaterialsPanel::MakeTableRow(const FText& Name, const FText& Detail, const FText& Quantity, const FText& Weight, const FText& Power, ERowStyle Style)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const bool bGroup = Style == ERowStyle::Group;

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Row->SetBrush(MakeBox(bGroup ? Palette.PanelHeader : FLinearColor::Transparent));
	Row->SetPadding(FMargin(bGroup ? 10.f : 18.f, 3.f, 10.f, 3.f));

	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Cells);

	UVerticalBox* NameCell = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UTextBlock* NameText = MakeText(Name, bGroup ? 7 : 8, bGroup ? TEXT("Bold") : TEXT("Regular"), bGroup ? Palette.TextSecondary : Palette.TextPrimary);
	NameText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	NameCell->AddChildToVerticalBox(NameText);
	if (!Detail.IsEmpty())
	{
		UTextBlock* DetailText = MakeText(Detail, 7, TEXT("Regular"), Palette.TextSecondary);
		DetailText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		NameCell->AddChildToVerticalBox(DetailText);
	}
	if (UHorizontalBoxSlot* NameSlot = Cells->AddChildToHorizontalBox(NameCell))
	{
		NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		NameSlot->SetVerticalAlignment(VAlign_Center);
	}

	const auto AddNumber = [this, Cells, bGroup, &Palette](const FText& Value, float Width)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		UTextBlock* Text = MakeText(Value, bGroup ? 7 : 8, TEXT("Bold"), bGroup ? Palette.TextSecondary : Palette.TextPrimary);
		Text->SetJustification(ETextJustify::Right);
		Box->SetContent(Text);
		if (UHorizontalBoxSlot* NumberSlot = Cells->AddChildToHorizontalBox(Box))
		{
			NumberSlot->SetVerticalAlignment(VAlign_Center);
		}
	};
	AddNumber(Quantity, StageBillOfMaterialsPanel::QuantityColumnWidth);
	AddNumber(Weight, StageBillOfMaterialsPanel::WeightColumnWidth);
	AddNumber(Power, StageBillOfMaterialsPanel::PowerColumnWidth);
	return Row;
}

UWidget* UStageBillOfMaterialsPanel::BuildFooter()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UVerticalBox* Footer = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Footer"));
	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Footer->AddChildToVerticalBox(Buttons);

	ExportButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("ExportButton"));
	ExportButton->SetStyle(MakeFlatButton(Palette.AccentColor, Palette.AccentColor * 1.1f, Palette.AccentColor * 0.85f));
	ExportButton->SetToolTipText(LOCTEXT("ExportTip", "Write the bill of materials as a print-ready PDF and a CSV (opens in any spreadsheet) to the Reports folder."));
	ExportButtonText = MakeText(LOCTEXT("ExportLabel", "EXPORT PDF"), 9, TEXT("Bold"), Palette.WindowBackground);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(ExportButton->AddChild(ExportButtonText)))
	{
		LabelSlot->SetPadding(FMargin(12.f, 6.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	if (UHorizontalBoxSlot* ExportSlot = Buttons->AddChildToHorizontalBox(ExportButton))
	{
		ExportSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	OpenFolderButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("OpenFolderButton"));
	FButtonStyle FolderStyle = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	FolderStyle.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	OpenFolderButton->SetStyle(FolderStyle);
	OpenFolderButton->SetToolTipText(LOCTEXT("OpenFolderTip", "Open the Reports folder in the file browser."));
	if (UButtonSlot* FolderLabelSlot = Cast<UButtonSlot>(OpenFolderButton->AddChild(MakeText(LOCTEXT("OpenFolder", "OPEN FOLDER"), 7, TEXT("Bold"), Palette.TextSecondary))))
	{
		FolderLabelSlot->SetPadding(FMargin(8.f, 6.f));
	}
	if (UHorizontalBoxSlot* FolderSlot = Buttons->AddChildToHorizontalBox(OpenFolderButton))
	{
		FolderSlot->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
	}

	StatusText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	StatusText->SetAutoWrapText(true);
	if (UVerticalBoxSlot* StatusSlot = Footer->AddChildToVerticalBox(StatusText))
	{
		StatusSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	}
	return Footer;
}

void UStageBillOfMaterialsPanel::NativeConstruct()
{
	Super::NativeConstruct();

	UStageBillOfMaterialsSubsystem* Subsystem = UWorld::GetSubsystem<UStageBillOfMaterialsSubsystem>(GetWorld());
	Report = Subsystem;
	Session = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	if (Subsystem)
	{
		Subsystem->OnReportChanged.AddUniqueDynamic(this, &ThisClass::HandleReportChanged);
		Subsystem->OnExportFinished.AddUniqueDynamic(this, &ThisClass::HandleExportFinished);
	}
	Rebuild();
	RefreshExport();
}

void UStageBillOfMaterialsPanel::NativeDestruct()
{
	if (UStageBillOfMaterialsSubsystem* Subsystem = Report.Get())
	{
		Subsystem->OnReportChanged.RemoveDynamic(this, &ThisClass::HandleReportChanged);
		Subsystem->OnExportFinished.RemoveDynamic(this, &ThisClass::HandleExportFinished);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	Report.Reset();
	Session.Reset();
	Super::NativeDestruct();
}

void UStageBillOfMaterialsPanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStageBillOfMaterialsPanel::Rebuild()
{
	using namespace StageReportFormat;
	RebuildTimer.Invalidate();
	if (!TableBody)
	{
		return;
	}
	TableBody->ClearChildren();

	const UStageBillOfMaterialsSubsystem* Subsystem = Report.Get();
	const FStageBillOfMaterials EmptyReport;
	const FStageBillOfMaterials& Current = Subsystem ? Subsystem->GetReport() : EmptyReport;

	for (const FStageBomGroup& Group : Current.Groups)
	{
		TableBody->AddChildToVerticalBox(MakeTableRow(Group.Label.ToUpper(), FText::GetEmpty(), FText::AsNumber(Group.Quantity),
			FormatWeight(Group.TotalWeightKg), FormatPower(Group.TotalPowerWatts), ERowStyle::Group));
		for (const FStageBomLine& Line : Group.Lines)
		{
			TableBody->AddChildToVerticalBox(MakeTableRow(Line.ItemName, MakeLineDetail(Line), FText::AsNumber(Line.Quantity),
				FormatWeight(Line.TotalWeightKg), FormatPower(Line.TotalPowerWatts), ERowStyle::Line));
		}
	}

	ItemsValue->SetText(FText::AsNumber(Current.Placements));
	UnitsValue->SetText(FText::AsNumber(Current.Quantity));
	WeightValue->SetText(FormatWeight(Current.TotalWeightKg));
	PowerValue->SetText(FormatPower(Current.TotalPowerWatts));
	EmptyText->SetVisibility(Current.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

	const UStageSessionSubsystem* SessionSubsystem = Session.Get();
	const FString SceneName = SessionSubsystem ? SessionSubsystem->GetSceneName() : FString();
	SubtitleText->SetText(FText::Format(LOCTEXT("Subtitle", "{0}   ·   {1} {1}|plural(one=line,other=lines)"),
		SceneName.IsEmpty() ? LOCTEXT("Untitled", "Untitled scene") : FText::FromString(SceneName), Current.LineCount));
	RefreshExport();
}

void UStageBillOfMaterialsPanel::RefreshExport()
{
	const UStageBillOfMaterialsSubsystem* Subsystem = Report.Get();
	const bool bExporting = Subsystem && Subsystem->IsExporting();
	if (ExportButton)
	{
		// Display only: an empty stage is refused (and reported) by the request anyway.
		ExportButton->SetIsEnabled(Subsystem && !bExporting && !Subsystem->GetReport().IsEmpty());
	}
	if (ExportButtonText)
	{
		ExportButtonText->SetText(bExporting ? LOCTEXT("Exporting", "EXPORTING...") : LOCTEXT("ExportLabel", "EXPORT PDF"));
	}
	if (!StatusText)
	{
		return;
	}

	const UStageCraftUITheme& Palette = GetTheme();
	const FStageReportExportResult* Last = Subsystem ? &Subsystem->GetLastExport() : nullptr;
	const bool bHasResult = Last && !Last->Message.IsEmpty();
	StatusText->SetText(bHasResult ? Last->Message
		: (Subsystem ? FText::FromString(Subsystem->GetOutputDirectory()) : LOCTEXT("Unavailable", "The bill of materials is available while a stage is open.")));
	StatusText->SetColorAndOpacity(FSlateColor(bHasResult && !Last->bSucceeded ? Palette.WarningColor : Palette.TextSecondary));
}

void UStageBillOfMaterialsPanel::HandleReportChanged()
{
	RequestRebuild();
}

void UStageBillOfMaterialsPanel::HandleExportFinished(const FStageReportExportResult& Result)
{
	RefreshExport();
}

void UStageBillOfMaterialsPanel::HandleExportClicked()
{
	if (AModularPlayerController* Controller = Cast<AModularPlayerController>(GetOwningPlayer()))
	{
		Controller->RequestExportBillOfMaterials();
	}
	RefreshExport();
}

void UStageBillOfMaterialsPanel::HandleOpenFolderClicked()
{
	if (const UStageBillOfMaterialsSubsystem* Subsystem = Report.Get())
	{
		const FString Directory = Subsystem->GetOutputDirectory();
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		FPlatformProcess::ExploreFolder(*Directory);
	}
}

#undef LOCTEXT_NAMESPACE
