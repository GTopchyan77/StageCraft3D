// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StagePatchPanel.h"

#include "Actors/LightingFixtureActor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/SelectionComponent.h"
#include "Components/SizeBox.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/LightingFixtureData.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Import/StageFileDialog.h"
#include "Interaction/StageParameterInterface.h"
#include "Misc/Paths.h"
#include "Player/ModularPlayerController.h"
#include "Report/StageReportFiles.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageDMXNetworkSubsystem.h"
#include "Subsystems/StageDMXPatchSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePatchPanel)

#define LOCTEXT_NAMESPACE "StagePatchPanel"

namespace StagePatchPanel
{
	constexpr float FixtureIdWidth = 52.f;
	constexpr float UniverseWidth = 48.f;
	constexpr float AddressWidth = 52.f;
	constexpr float FootprintWidth = 30.f;
	constexpr float ConflictWidth = 14.f;
	constexpr float ChannelAddressWidth = 52.f;
	constexpr float ChannelWidthWidth = 34.f;
	constexpr float ChannelValueWidth = 54.f;
	constexpr int32 MaxFixtureId = 99999;
	constexpr int32 MaxUniverse = 63999;
	constexpr int32 MaxAddress = 512;
	constexpr int32 UniverseSize = 512;
	constexpr float OptionsWidth = 66.f;

	/** The per-row pan / tilt option toggles; a toggle's choice index is Row * PatchOptionCount + its option. */
	enum class EPatchOption : int32
	{
		InvertPan,
		InvertTilt,
		Swap,
	};
	constexpr int32 PatchOptionCount = 3;

	/** Patch edits that change what the table shows; attribute changes only touch channel values. */
	bool IsTableParameter(const FGameplayTag& ParameterId)
	{
		return StageCraftTags::IsPatchParameter(ParameterId) || ParameterId == StageCraftTags::Param_Info_Label
			|| ParameterId == StageCraftTags::Param_Info_Type;
	}
}

namespace StagePatchFormat
{
	FText FormatPatch(int32 Universe, int32 Address)
	{
		if (Universe <= 0 || Address <= 0)
		{
			return LOCTEXT("Unpatched", "—");
		}
		return FText::FromString(FString::Printf(TEXT("%d.%03d"), Universe, Address));
	}

	int32 ReadChannelValue(TConstArrayView<uint8> Universe, int32 Address, bool b16Bit)
	{
		const int32 Index = Address - 1;
		if (Index < 0 || Index + (b16Bit ? 2 : 1) > Universe.Num())
		{
			return 0;
		}
		return b16Bit ? (Universe[Index] << 8) | Universe[Index + 1] : Universe[Index];
	}
}

// --- UStagePatchField ---

void UStagePatchField::Configure(ALightingFixtureActor& InFixture, const FGameplayTag& InParameterId)
{
	Fixture = &InFixture;
	ParameterId = InParameterId;
	OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleValueCommitted);
	OnEndSliderMovement.AddUniqueDynamic(this, &ThisClass::HandleEndSliderMovement);
}

void UStagePatchField::HandleValueCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	OnFieldCommitted.ExecuteIfBound(*this, FMath::RoundToInt32(InValue));
}

void UStagePatchField::HandleEndSliderMovement(float InValue)
{
	OnFieldCommitted.ExecuteIfBound(*this, FMath::RoundToInt32(InValue));
}

// --- UStagePatchPanel ---

const UStageCraftUITheme& UStagePatchPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStagePatchPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

ALightingFixtureActor* UStagePatchPanel::GetSelectedFixture() const
{
	const USelectionComponent* SelectionComponent = Selection.Get();
	return SelectionComponent ? Cast<ALightingFixtureActor>(SelectionComponent->GetSelectedActor()) : nullptr;
}

UTextBlock* UStagePatchPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void UStagePatchPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	if (PatchUnpatchedButton)
	{
		PatchUnpatchedButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandlePatchUnpatchedClicked);
	}
	if (ExportButton)
	{
		ExportButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleExportClicked);
	}
	if (FixConflictsButton)
	{
		FixConflictsButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleFixConflictsClicked);
	}
	if (PatchSelectionButton)
	{
		PatchSelectionButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandlePatchSelectionClicked);
	}
	if (InputButton)
	{
		InputButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleInputClicked);
	}
	if (OutputButton)
	{
		OutputButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleOutputClicked);
	}
	if (ImportButton)
	{
		ImportButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleImportClicked);
	}
	if (ImportApplyButton)
	{
		ImportApplyButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleImportApplyClicked);
	}
	if (ImportCancelButton)
	{
		ImportCancelButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleImportCancelClicked);
	}
	if (ArtNetModeButton)
	{
		ArtNetModeButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleArtNetModeClicked);
	}
	if (SACNModeButton)
	{
		SACNModeButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleSACNModeClicked);
	}
	if (UnicastField)
	{
		UnicastField->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleUnicastCommitted);
	}
	if (PriorityField)
	{
		PriorityField->OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandlePriorityCommitted);
	}
	if (RateField)
	{
		RateField->OnValueCommitted.AddUniqueDynamic(this, &ThisClass::HandleRateCommitted);
	}
}

void UStagePatchPanel::BuildTree()
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
	Column->AddChildToVerticalBox(BuildToolsBar());
	Column->AddChildToVerticalBox(BuildImportBar());

	// The patch usually docks low and wide (next to the faders), so the fixture table and the selected fixture's channels
	// sit side by side, like a console's patch window with its fixture sheet.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Body"));
	if (UVerticalBoxSlot* BodySlot = Column->AddChildToVerticalBox(Body))
	{
		BodySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UVerticalBox* TableColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("TableColumn"));
	if (UHorizontalBoxSlot* TableColumnSlot = Body->AddChildToHorizontalBox(TableColumn))
	{
		FSlateChildSize TableSize(ESlateSizeRule::Fill);
		TableSize.Value = 0.62f;
		TableColumnSlot->SetSize(TableSize);
	}
	TableColumn->AddChildToVerticalBox(MakeColumnTitles());

	EmptyText = MakeText(LOCTEXT("Empty", "No fixtures on the stage. Placed lights appear here with their fixture ID and DMX patch."), 8, TEXT("Italic"), Palette.TextSecondary);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = TableColumn->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* TableScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("TableScroll"));
	TableScroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	TableScroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* TableSlot = TableColumn->AddChildToVerticalBox(TableScroll))
	{
		TableSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	TableBody = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("TableBody"));
	TableScroll->AddChild(TableBody);

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ColumnDivider"));
	Divider->SetBrush(MakeBox(Palette.Divider));
	Divider->SetDesiredSizeOverride(FVector2D(1.f, 1.f));
	Body->AddChildToHorizontalBox(Divider);

	UVerticalBox* ChannelColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ChannelColumn"));
	if (UHorizontalBoxSlot* ChannelColumnSlot = Body->AddChildToHorizontalBox(ChannelColumn))
	{
		FSlateChildSize ChannelSize(ESlateSizeRule::Fill);
		ChannelSize.Value = 0.38f;
		ChannelColumnSlot->SetSize(ChannelSize);
	}
	UBorder* ChannelHeader = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ChannelHeader"));
	ChannelHeader->SetBrush(MakeBox(Palette.PanelHeader));
	ChannelHeader->SetPadding(FMargin(10.f, 3.f));
	ChannelTitleText = MakeText(FText::GetEmpty(), 6, TEXT("Bold"), Palette.TextSecondary);
	ChannelTitleText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	ChannelHeader->SetContent(ChannelTitleText);
	ChannelColumn->AddChildToVerticalBox(ChannelHeader);

	UScrollBox* ChannelScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("ChannelScroll"));
	ChannelScroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	ChannelScroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ChannelSlot = ChannelColumn->AddChildToVerticalBox(ChannelScroll))
	{
		ChannelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	ChannelBody = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ChannelBody"));
	ChannelScroll->AddChild(ChannelBody);
}

UWidget* UStagePatchPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));

	// Title and counts on the left, the patch action on the right: one line of chrome, so the table keeps its height.
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(Line);
	UTextBlock* Title = MakeText(LOCTEXT("Title", "DMX PATCH"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Every fixture with its fixture ID and DMX address. Overlapping addresses are marked in red."));
	if (UHorizontalBoxSlot* TitleSlot = Line->AddChildToHorizontalBox(Title))
	{
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}
	SubtitleText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	SubtitleText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UHorizontalBoxSlot* SubtitleSlot = Line->AddChildToHorizontalBox(SubtitleText))
	{
		SubtitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SubtitleSlot->SetVerticalAlignment(VAlign_Center);
		SubtitleSlot->SetPadding(FMargin(10.f, 0.f));
	}
	PatchUnpatchedButton = MakeHeaderButton(TEXT("PatchUnpatchedButton"), LOCTEXT("PatchUnpatched", "AUTO PATCH"),
		LOCTEXT("PatchUnpatchedTip", "Patch every unpatched fixture into the first free addresses that fit it, in fixture ID order. One undo step."));
	if (UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(PatchUnpatchedButton))
	{
		ButtonSlot->SetVerticalAlignment(VAlign_Center);
	}

	ExportButton = MakeHeaderButton(TEXT("ExportButton"), LOCTEXT("Export", "EXPORT PDF"),
		LOCTEXT("ExportTip", "Export the patch sheet as a print-ready PDF and a CSV to the Reports folder (File > Production > Open Reports Folder)."));
	if (UHorizontalBoxSlot* ExportSlot = Line->AddChildToHorizontalBox(ExportButton))
	{
		ExportSlot->SetVerticalAlignment(VAlign_Center);
		ExportSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
	}

	ImportButton = MakeHeaderButton(TEXT("ImportButton"), LOCTEXT("Import", "IMPORT CSV"),
		LOCTEXT("ImportTip", "Patch from a fixture schedule: a CSV with Fixture ID, Universe and Address columns (and optionally Invert Pan, Invert Tilt, Swap Pan/Tilt), such as the CSV EXPORT PDF writes. You see what would change before anything does; applying is one undo step."));
	if (UHorizontalBoxSlot* ImportSlot = Line->AddChildToHorizontalBox(ImportButton))
	{
		ImportSlot->SetVerticalAlignment(VAlign_Center);
		ImportSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
	}
	return Header;
}

UWidget* UStagePatchPanel::MakeCell(UWidget& Content, float Width)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Width);
	Box->SetContent(&Content);
	return Box;
}

UWidget* UStagePatchPanel::MakeColumnTitles()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePatchPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ColumnTitles"));
	Row->SetBrush(MakeBox(Palette.PanelHeader));
	Row->SetPadding(FMargin(10.f, 3.f));
	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Cells);

	const auto AddTitle = [this, Cells, &Palette](const FText& Title, float Width, float LeftGap)
	{
		UTextBlock* Text = MakeText(Title, 6, TEXT("Bold"), Palette.TextSecondary);
		UHorizontalBoxSlot* TitleSlot = Width > 0.f ? Cells->AddChildToHorizontalBox(MakeCell(*Text, Width)) : Cells->AddChildToHorizontalBox(Text);
		if (TitleSlot)
		{
			TitleSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			TitleSlot->SetVerticalAlignment(VAlign_Center);
			if (Width <= 0.f)
			{
				TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};
	AddTitle(FText::GetEmpty(), ConflictWidth, 0.f);
	AddTitle(LOCTEXT("FidColumn", "FID"), FixtureIdWidth, 0.f);
	AddTitle(LOCTEXT("NameColumn", "NAME / TYPE"), 0.f, 6.f);
	AddTitle(LOCTEXT("UniverseColumn", "UNIV"), UniverseWidth, 4.f);
	AddTitle(LOCTEXT("AddressColumn", "ADDR"), AddressWidth, 4.f);
	AddTitle(LOCTEXT("FootprintColumn", "CH"), FootprintWidth, 4.f);
	AddTitle(LOCTEXT("OptionsColumn", "P/T OUT"), OptionsWidth, 6.f);
	return Row;
}

UStagePatchField* UStagePatchPanel::MakePatchField(ALightingFixtureActor& Fixture, const FGameplayTag& ParameterId, int32 Value, int32 Min, int32 Max, const FText& ToolTip)
{
	const UStageCraftUITheme& Palette = GetTheme();
	UStagePatchField* Field = WidgetTree->ConstructWidget<UStagePatchField>(UStagePatchField::StaticClass());
	// Styled before it joins the live row: a widget added to a built panel builds its Slate widget at that moment.
	Field->SetWidgetStyle(StageCraftWidgetStyle::MakeNumericField(Palette));
	Field->SetFont(StageCraftWidgetStyle::MakeFont(8));
	Field->SetForegroundColor(Palette.TextPrimary);
	Field->SetMinDesiredWidth(0.f);
	Field->SetMinFractionalDigits(0);
	Field->SetMaxFractionalDigits(0);
	Field->SetDelta(1.f);
	Field->SetAlwaysUsesDeltaSnap(true);
	Field->SetMinValue(static_cast<float>(Min));
	Field->SetMaxValue(static_cast<float>(Max));
	Field->SetValue(static_cast<float>(Value));
	Field->SetToolTipText(ToolTip);
	Field->Configure(Fixture, ParameterId);
	Field->OnFieldCommitted.BindUObject(this, &ThisClass::HandleFieldCommitted);
	return Field;
}

UWidget* UStagePatchPanel::MakeFixtureRow(ALightingFixtureActor& Fixture, bool bConflict, bool bSelected)
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePatchPanel;
	const UStageCraftUITheme& Palette = GetTheme();
	const ULightingFixtureData* Data = Fixture.GetFixtureData();
	const FStageDMXPatch Patch = Fixture.GetPatch();

	// The whole row is a button (select the fixture); the fields inside take their own clicks.
	UStageChoiceButton* RowButton = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	const FLinearColor Normal = bSelected ? WithAlpha(Palette.SelectionColor, 0.25f) : FLinearColor::Transparent;
	RowButton->SetStyle(MakeFlatButton(Normal, bSelected ? WithAlpha(Palette.SelectionColor, 0.32f) : Palette.RowHover, Palette.Divider));
	RowButton->SetChoiceIndex(RowFixtures.Num());
	RowButton->OnPicked.BindUObject(this, &ThisClass::HandleRowPicked);
	RowFixtures.Add(&Fixture);

	UHorizontalBox* Cells = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UButtonSlot* CellsSlot = Cast<UButtonSlot>(RowButton->AddChild(Cells)))
	{
		CellsSlot->SetPadding(FMargin(10.f, 2.f));
		CellsSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	const auto AddCell = [Cells](UWidget* Widget, float LeftGap, bool bFill)
	{
		if (UHorizontalBoxSlot* CellSlot = Cells->AddChildToHorizontalBox(Widget))
		{
			CellSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			CellSlot->SetVerticalAlignment(VAlign_Center);
			if (bFill)
			{
				CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};

	UTextBlock* Marker = MakeText(bConflict ? LOCTEXT("ConflictMarker", "!") : FText::GetEmpty(), 9, TEXT("Bold"), Palette.WarningColor);
	if (bConflict)
	{
		Marker->SetToolTipText(LOCTEXT("ConflictTip", "This fixture's DMX channels overlap another fixture's. Change the universe or address."));
	}
	AddCell(MakeCell(*Marker, ConflictWidth), 0.f, false);

	USizeBox* FidBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	FidBox->SetWidthOverride(FixtureIdWidth);
	FidBox->SetHeightOverride(FieldHeight);
	FidBox->SetContent(MakePatchField(Fixture, StageCraftTags::Param_Patch_FixtureId, Fixture.GetFixtureId(), 1, MaxFixtureId,
		LOCTEXT("FidTip", "Fixture ID: the number the fixture is called by on the console. Must be unique.")));
	AddCell(FidBox, 0.f, false);

	FStageParameterValue Label;
	const bool bHasLabel = IStageParameterInterface::Execute_GetParameterValue(&Fixture, StageCraftTags::Param_Info_Label, Label);
	UVerticalBox* NameCell = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UTextBlock* NameText = MakeText(bHasLabel ? Label.Text : FText::FromString(Fixture.GetName()), 8, TEXT("Regular"), bConflict ? Palette.WarningColor : Palette.TextPrimary);
	NameText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	NameCell->AddChildToVerticalBox(NameText);
	const FText TypeText = Data
		? FText::Format(LOCTEXT("TypeMode", "{0} · {1}"), Data->DisplayName, FText::FromName(Data->DMXModeName))
		: LOCTEXT("NoData", "No fixture data");
	UTextBlock* Detail = MakeText(TypeText, 7, TEXT("Regular"), Palette.TextSecondary);
	Detail->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	NameCell->AddChildToVerticalBox(Detail);
	// In a narrow panel the name gets less than it asks for: clipped (and ellipsised), never drawn under the patch fields.
	NameCell->SetClipping(EWidgetClipping::ClipToBounds);
	AddCell(NameCell, 6.f, true);

	const auto AddField = [&](const FGameplayTag& ParameterId, int32 Value, int32 Min, int32 Max, float Width, const FText& ToolTip)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->SetHeightOverride(FieldHeight);
		Box->SetContent(MakePatchField(Fixture, ParameterId, Value, Min, Max, ToolTip));
		AddCell(Box, 4.f, false);
	};
	AddField(StageCraftTags::Param_Patch_Universe, Patch.Universe, 0, MaxUniverse, UniverseWidth,
		LOCTEXT("UniverseTip", "DMX universe. 0 = unpatched."));
	AddField(StageCraftTags::Param_Patch_Address, Patch.Address, 0, MaxAddress, AddressWidth,
		FText::Format(LOCTEXT("AddressTip", "DMX start address 1-512. Occupies {0} channels; patched as {1}."),
			Fixture.GetDMXFootprint(), StagePatchFormat::FormatPatch(Patch.Universe, Patch.Address)));

	UTextBlock* Footprint = MakeText(FText::AsNumber(Fixture.GetDMXFootprint()), 8, TEXT("Bold"), Palette.TextSecondary);
	Footprint->SetJustification(ETextJustify::Right);
	AddCell(MakeCell(*Footprint, FootprintWidth), 4.f, false);

	// Pan / tilt options on the wire (Docs/ADR/0019 §10.1): lit when on, greyed out for an axis the DMX mode does not have.
	bool bHasPan = false;
	bool bHasTilt = false;
	Fixture.GetPanTiltChannels(bHasPan, bHasTilt);
	const FStageDMXPatchOptions Options = Fixture.GetPatchOptions();
	UHorizontalBox* OptionCell = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	const int32 Row = RowFixtures.Num() - 1;
	const struct { EPatchOption Option; FText Label; FText ToolTip; bool bOn; bool bAvailable; } OptionButtons[] = {
		{ EPatchOption::InvertPan, LOCTEXT("InvertPanShort", "-P"),
			LOCTEXT("InvertPanTip", "Invert Pan on the wire: the pan channel runs the other way (for a fixture hung the other way round). The beam on the stage does not change. Undoable."),
			Options.bInvertPan, bHasPan },
		{ EPatchOption::InvertTilt, LOCTEXT("InvertTiltShort", "-T"),
			LOCTEXT("InvertTiltTip", "Invert Tilt on the wire: the tilt channel runs the other way. The beam on the stage does not change. Undoable."),
			Options.bInvertTilt, bHasTilt },
		{ EPatchOption::Swap, LOCTEXT("SwapShort", "SW"),
			LOCTEXT("SwapTip", "Swap Pan/Tilt on the wire: the pan channel carries the tilt and the other way round (for a fixture hung sideways). The beam on the stage does not change. Undoable."),
			Options.bSwapPanTilt, bHasPan && bHasTilt },
	};
	for (const auto& Entry : OptionButtons)
	{
		UStageChoiceButton* Toggle = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		const bool bLit = Entry.bOn && Entry.bAvailable;
		Toggle->SetStyle(MakeFlatButton(bLit ? WithAlpha(Palette.AccentColor, 0.28f) : FLinearColor::Transparent,
			bLit ? WithAlpha(Palette.AccentColor, 0.38f) : Palette.RowHover, Palette.Divider));
		Toggle->SetChoiceIndex(Row * PatchOptionCount + static_cast<int32>(Entry.Option));
		Toggle->OnPicked.BindUObject(this, &ThisClass::HandlePatchOptionPicked);
		Toggle->SetIsEnabled(Entry.bAvailable);
		Toggle->SetToolTipText(Entry.bAvailable ? Entry.ToolTip : LOCTEXT("OptionUnavailable", "This fixture's DMX mode has no channel for it."));
		UTextBlock* ToggleLabel = MakeText(Entry.Label, 6, TEXT("Bold"), bLit ? Palette.TextPrimary : Palette.TextSecondary);
		if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Toggle->AddChild(ToggleLabel)))
		{
			LabelSlot->SetPadding(FMargin(4.f, 3.f));
			LabelSlot->SetHorizontalAlignment(HAlign_Center);
		}
		if (UHorizontalBoxSlot* ToggleSlot = OptionCell->AddChildToHorizontalBox(Toggle))
		{
			ToggleSlot->SetPadding(FMargin(1.f, 0.f));
			ToggleSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
	AddCell(MakeCell(*OptionCell, OptionsWidth), 6.f, false);
	return RowButton;
}

UButton* UStagePatchPanel::MakeHeaderButton(FName Name, const FText& Label, const FText& ToolTip, UTextBlock** OutLabel)
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UButton* Button = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), Name);
	FButtonStyle Style = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	Button->SetStyle(Style);
	Button->SetToolTipText(ToolTip);
	UTextBlock* LabelText = MakeText(Label, 7, TEXT("Bold"), Palette.TextSecondary);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(LabelText)))
	{
		LabelSlot->SetPadding(FMargin(8.f, 6.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	if (OutLabel)
	{
		*OutLabel = LabelText;
	}
	return Button;
}

USpinBox* UStagePatchPanel::MakeNumberField(int32 Value, int32 Min, int32 Max, float Width, const FText& ToolTip)
{
	const UStageCraftUITheme& Palette = GetTheme();
	USpinBox* Field = WidgetTree->ConstructWidget<USpinBox>(USpinBox::StaticClass());
	Field->SetWidgetStyle(StageCraftWidgetStyle::MakeNumericField(Palette));
	Field->SetFont(StageCraftWidgetStyle::MakeFont(8));
	Field->SetForegroundColor(Palette.TextPrimary);
	Field->SetMinDesiredWidth(Width);
	Field->SetMinFractionalDigits(0);
	Field->SetMaxFractionalDigits(0);
	Field->SetDelta(1.f);
	Field->SetAlwaysUsesDeltaSnap(true);
	Field->SetMinValue(static_cast<float>(Min));
	Field->SetMaxValue(static_cast<float>(Max));
	Field->SetValue(static_cast<float>(Value));
	Field->SetToolTipText(ToolTip);
	return Field;
}

UWidget* UStagePatchPanel::BuildToolsBar()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePatchPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Bar = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ToolsBar"));
	Bar->SetBrush(MakeBox(Palette.PanelHeader));
	Bar->SetPadding(FMargin(10.f, 0.f, 10.f, 6.f));
	UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Bar->SetContent(Lines);

	const auto AddToLine = [](UHorizontalBox* Line, UWidget* Widget, float LeftGap, bool bFill)
	{
		if (UHorizontalBoxSlot* WidgetSlot = Line->AddChildToHorizontalBox(Widget))
		{
			WidgetSlot->SetVerticalAlignment(VAlign_Center);
			WidgetSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			if (bFill)
			{
				WidgetSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};

	// Network input: the switch and what it is doing.
	UHorizontalBox* InputLine = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InputLine"));
	Lines->AddChildToVerticalBox(InputLine);
	UTextBlock* InputLabel = nullptr;
	InputButton = MakeHeaderButton(TEXT("InputButton"), LOCTEXT("InputOff", "DMX IN: OFF"),
		LOCTEXT("InputTip", "Receive Art-Net (port 6454) and sACN (port 5568) from a console or visualiser on the network, and let it drive the fixtures (the show switches to External). Remembered on this PC. Windows may ask to allow StageCraft through the firewall."),
		&InputLabel);
	InputButtonLabel = InputLabel;
	AddToLine(InputLine, InputButton, 0.f, false);
	InputStatusText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	InputStatusText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	AddToLine(InputLine, InputStatusText, 10.f, true);

	// Network output: the switch, where it sends, and which universes.
	if (UVerticalBoxSlot* OutputSlot = Lines->AddChildToVerticalBox(BuildOutputLine()))
	{
		OutputSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}

	// Patch tools: repair conflicts, patch the selection from an address; how full each universe is.
	UHorizontalBox* ToolsLine = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ToolsLine"));
	if (UVerticalBoxSlot* ToolsSlot = Lines->AddChildToVerticalBox(ToolsLine))
	{
		ToolsSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	}
	FixConflictsButton = MakeHeaderButton(TEXT("FixConflictsButton"), LOCTEXT("FixConflicts", "FIX CONFLICTS"),
		LOCTEXT("FixConflictsTip", "Move every fixture that overlaps another (or runs past channel 512) to the first free addresses. In each overlap the fixture that starts first stays. One undo step."));
	AddToLine(ToolsLine, FixConflictsButton, 0.f, false);
	PatchSelectionButton = MakeHeaderButton(TEXT("PatchSelectionButton"), LOCTEXT("PatchSelection", "PATCH SELECTION AT"),
		LOCTEXT("PatchSelectionTip", "Patch the selected fixtures one after another, in fixture ID order, from the universe and address on the right, around the fixtures already there. One undo step."));
	AddToLine(ToolsLine, PatchSelectionButton, 4.f, false);

	const auto AddField = [this, ToolsLine, &AddToLine](USpinBox* Field, float Width)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->SetHeightOverride(FieldHeight);
		Box->SetContent(Field);
		AddToLine(ToolsLine, Box, 4.f, false);
	};
	StartUniverseField = MakeNumberField(1, 1, MaxUniverse, UniverseWidth, LOCTEXT("StartUniverseTip", "Universe to patch the selection into."));
	AddField(StartUniverseField, UniverseWidth);
	StartAddressField = MakeNumberField(1, 1, MaxAddress, AddressWidth, LOCTEXT("StartAddressTip", "First address for the selection; occupied channels are skipped."));
	AddField(StartAddressField, AddressWidth);

	UsageText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	UsageText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	UsageText->SetToolTipText(LOCTEXT("UsageTip", "Channels used per universe (overlapping channels count once)."));
	AddToLine(ToolsLine, UsageText, 10.f, true);
	return Bar;
}

void UStagePatchPanel::NativeConstruct()
{
	Super::NativeConstruct();

	UShowControlSubsystem* Subsystem = UWorld::GetSubsystem<UShowControlSubsystem>(GetWorld());
	ShowControl = Subsystem;
	if (Subsystem)
	{
		Subsystem->OnFixturesChanged.AddUniqueDynamic(this, &ThisClass::HandleFixturesChanged);
	}
	UStageDMXPatchSubsystem* PatchSubsystem = UWorld::GetSubsystem<UStageDMXPatchSubsystem>(GetWorld());
	Patching = PatchSubsystem;
	if (PatchSubsystem)
	{
		PatchSubsystem->OnPatchChanged.AddUniqueDynamic(this, &ThisClass::HandlePatchChanged);
	}
	UStageDMXNetworkSubsystem* NetworkSubsystem = UWorld::GetSubsystem<UStageDMXNetworkSubsystem>(GetWorld());
	Network = NetworkSubsystem;
	if (NetworkSubsystem && !InputChangedHandle.IsValid())
	{
		InputChangedHandle = NetworkSubsystem->OnInputChanged.AddUObject(this, &ThisClass::RefreshInputStatus);
	}
	if (NetworkSubsystem && !OutputChangedHandle.IsValid())
	{
		OutputChangedHandle = NetworkSubsystem->OnOutputChanged.AddUObject(this, &ThisClass::RefreshOutputStatus);
	}

	const AModularPlayerController* Controller = GetStageController();
	USelectionComponent* SelectionComponent = Controller ? Controller->GetSelection() : nullptr;
	Selection = SelectionComponent;
	if (SelectionComponent)
	{
		SelectionComponent->OnSelectionSetChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionSetChanged);
	}
	Rebuild();
	RefreshInputStatus();
	RefreshOutputStatus();
}

void UStagePatchPanel::NativeDestruct()
{
	if (UShowControlSubsystem* Subsystem = ShowControl.Get())
	{
		Subsystem->OnFixturesChanged.RemoveDynamic(this, &ThisClass::HandleFixturesChanged);
	}
	if (UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get())
	{
		PatchSubsystem->OnPatchChanged.RemoveDynamic(this, &ThisClass::HandlePatchChanged);
	}
	if (UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get())
	{
		NetworkSubsystem->OnInputChanged.Remove(InputChangedHandle);
		NetworkSubsystem->OnOutputChanged.Remove(OutputChangedHandle);
	}
	InputChangedHandle.Reset();
	OutputChangedHandle.Reset();
	Patching.Reset();
	Network.Reset();
	if (USelectionComponent* SelectionComponent = Selection.Get())
	{
		SelectionComponent->OnSelectionSetChanged.RemoveDynamic(this, &ThisClass::HandleSelectionSetChanged);
	}
	UnbindFixtures();
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	ShowControl.Reset();
	Selection.Reset();
	ChannelFixture.Reset();
	Super::NativeDestruct();
}

void UStagePatchPanel::BindFixture(ALightingFixtureActor& Fixture)
{
	Fixture.OnParameterChanged.AddUniqueDynamic(this, &ThisClass::HandleFixtureParameterChanged);
	BoundFixtures.Add(&Fixture);
}

void UStagePatchPanel::UnbindFixtures()
{
	for (const TWeakObjectPtr<ALightingFixtureActor>& Bound : BoundFixtures)
	{
		if (ALightingFixtureActor* Fixture = Bound.Get())
		{
			Fixture->OnParameterChanged.RemoveDynamic(this, &ThisClass::HandleFixtureParameterChanged);
		}
	}
	BoundFixtures.Reset();
}

void UStagePatchPanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	// Next tick: a burst of edits (Patch Unpatched, a scene load) is one rebuild, and never inside the callback that caused it.
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStagePatchPanel::Rebuild()
{
	RebuildTimer.Invalidate();
	if (!TableBody)
	{
		return;
	}
	TableBody->ClearChildren();
	RowFixtures.Reset();
	UnbindFixtures();

	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	TArray<ALightingFixtureActor*> Fixtures = Subsystem ? Subsystem->GetFixtures() : TArray<ALightingFixtureActor*>();
	Fixtures.Sort([](const ALightingFixtureActor& A, const ALightingFixtureActor& B)
	{
		return A.GetFixtureId() != B.GetFixtureId() ? A.GetFixtureId() < B.GetFixtureId() : A.GetName() < B.GetName();
	});

	const UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get();
	const TArray<int32> Conflicts = PatchSubsystem ? PatchSubsystem->FindPatchConflicts() : TArray<int32>();
	const ALightingFixtureActor* Selected = GetSelectedFixture();
	int32 Unpatched = 0;
	for (ALightingFixtureActor* Fixture : Fixtures)
	{
		TableBody->AddChildToVerticalBox(MakeFixtureRow(*Fixture, Conflicts.Contains(Fixture->GetFixtureId()), Fixture == Selected));
		BindFixture(*Fixture);
		Unpatched += Fixture->GetPatch().IsPatched() ? 0 : 1;
	}

	EmptyText->SetVisibility(Fixtures.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	PatchUnpatchedButton->SetIsEnabled(Unpatched > 0);
	ExportButton->SetIsEnabled(!Fixtures.IsEmpty());
	SubtitleText->SetText(FText::Format(LOCTEXT("Subtitle", "{0} {0}|plural(one=fixture,other=fixtures)   ·   {1} unpatched   ·   {2} {2}|plural(one=conflict,other=conflicts)"),
		Fixtures.Num(), Unpatched, Conflicts.Num()));
	SubtitleText->SetColorAndOpacity(FSlateColor(Conflicts.IsEmpty() ? GetTheme().TextSecondary : GetTheme().WarningColor));

	FixConflictsButton->SetIsEnabled(!Conflicts.IsEmpty());
	const USelectionComponent* SelectionComponent = Selection.Get();
	PatchSelectionButton->SetIsEnabled(SelectionComponent && SelectionComponent->GetSelectedActors().ContainsByPredicate(
		[](const AActor* Actor) { return Actor && Actor->IsA<ALightingFixtureActor>(); }));

	TArray<FString> Usage;
	for (const FStageDMXUniverseUsage& Universe : PatchSubsystem ? PatchSubsystem->GetUniverseUsage() : TArray<FStageDMXUniverseUsage>())
	{
		Usage.Add(FString::Printf(TEXT("U%d %d/%d%s"), Universe.Universe, Universe.UsedChannels, StagePatchPanel::UniverseSize,
			Universe.Conflicts > 0 ? TEXT(" !") : TEXT("")));
	}
	UsageText->SetText(Usage.IsEmpty() ? LOCTEXT("NoUniverses", "No universe in use") : FText::FromString(FString::Join(Usage, TEXT("   "))));
	RebuildChannelMap();
	RebuildOutputUniverses();
}

void UStagePatchPanel::RefreshInputStatus()
{
	if (!InputStatusText || !InputButtonLabel)
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get();
	const FStageDMXInputStatus Status = NetworkSubsystem ? NetworkSubsystem->GetStatus() : FStageDMXInputStatus();
	InputButton->SetIsEnabled(NetworkSubsystem != nullptr);

	switch (Status.State)
	{
	case EStageDMXInputState::Listening:
	{
		InputButtonLabel->SetText(LOCTEXT("InputOn", "DMX IN: ON"));
		InputButtonLabel->SetColorAndOpacity(FSlateColor(Palette.AccentColor));
		const int64 Frames = Status.ArtNetPackets + Status.SACNPackets;
		InputStatusText->SetText(Frames == 0
			? FText::Format(LOCTEXT("InputWaiting", "Listening · {0} · no DMX received yet"), FText::FromString(Status.Endpoint))
			: FText::Format(LOCTEXT("InputLive", "Listening · {0} · {1} {1}|plural(one=universe,other=universes) live from {2} {2}|plural(one=source,other=sources) · {3} Art-Net, {4} sACN frames{5}"),
				FText::FromString(Status.Endpoint), Status.LiveUniverses.Num(), Status.LiveSources,
				FText::AsNumber(Status.ArtNetPackets), FText::AsNumber(Status.SACNPackets),
				Status.InvalidPackets > 0 ? FText::Format(LOCTEXT("InputInvalid", " · {0} invalid"), FText::AsNumber(Status.InvalidPackets)) : FText::GetEmpty()));
		InputStatusText->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		break;
	}
	case EStageDMXInputState::Failed:
		InputButtonLabel->SetText(LOCTEXT("InputOffAgain", "DMX IN: OFF"));
		InputButtonLabel->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		InputStatusText->SetText(Status.Error);
		InputStatusText->SetColorAndOpacity(FSlateColor(Palette.WarningColor));
		break;
	default:
		InputButtonLabel->SetText(LOCTEXT("InputOffIdle", "DMX IN: OFF"));
		InputButtonLabel->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		InputStatusText->SetText(LOCTEXT("InputIdle", "Network input off: the cue list drives the fixtures. Switch on to follow a console over Art-Net or sACN."));
		InputStatusText->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		break;
	}
	InputStatusText->SetToolTipText(InputStatusText->GetText());
}

void UStagePatchPanel::RebuildChannelMap()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePatchPanel;
	if (!ChannelBody)
	{
		return;
	}
	ChannelBody->ClearChildren();
	ChannelRows.Reset();

	const UStageCraftUITheme& Palette = GetTheme();
	ALightingFixtureActor* Fixture = GetSelectedFixture();
	ChannelFixture = Fixture;
	const ULightingFixtureData* Data = Fixture ? Fixture->GetFixtureData() : nullptr;
	if (!Fixture || !Data)
	{
		ChannelTitleText->SetText(LOCTEXT("ChannelNone", "CHANNELS   ·   select a fixture to see its DMX channels"));
		return;
	}

	const FStageDMXPatch Patch = Fixture->GetPatch();
	ChannelTitleText->SetText(FText::Format(LOCTEXT("ChannelTitle", "CHANNELS   ·   FID {0}   ·   {1}   ·   {2}"),
		Fixture->GetFixtureId(), FText::FromName(Data->DMXModeName), StagePatchFormat::FormatPatch(Patch.Universe, Patch.Address)));

	for (const FStageFixtureDMXChannel& Channel : Data->DMXChannels)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		const auto AddCell = [this, Row](UWidget& Content, float Width, float LeftGap)
		{
			UHorizontalBoxSlot* CellSlot = Width > 0.f ? Row->AddChildToHorizontalBox(MakeCell(Content, Width)) : Row->AddChildToHorizontalBox(&Content);
			if (CellSlot)
			{
				CellSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
				if (Width <= 0.f)
				{
					CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
				}
			}
		};

		// Absolute address when patched ("1.017"), else the channel within the mode ("ch 17").
		const int32 Absolute = Patch.IsPatched() ? Patch.Address + Channel.Channel - 1 : 0;
		const FText Address = Patch.IsPatched()
			? StagePatchFormat::FormatPatch(Patch.Universe, Absolute)
			: FText::Format(LOCTEXT("ModeChannel", "ch {0}"), Channel.Channel);
		AddCell(*MakeText(Address, 8, TEXT("Bold"), Palette.TextSecondary), ChannelAddressWidth, 0.f);

		const FString AttributeName = Channel.Attribute.GetTagName().ToString().RightChop(FString(TEXT("StageCraft.Attribute.")).Len());
		UTextBlock* Name = MakeText(FText::FromString(AttributeName), 8, TEXT("Regular"),
			Data->SupportsAttribute(Channel.Attribute) ? Palette.TextPrimary : Palette.TextDisabled);
		Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		AddCell(*Name, 0.f, 6.f);

		AddCell(*MakeText(Channel.b16Bit ? LOCTEXT("SixteenBit", "16-bit") : LOCTEXT("EightBit", "8-bit"), 7, TEXT("Regular"), Palette.TextSecondary), ChannelWidthWidth, 4.f);

		UTextBlock* Value = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.AccentColor);
		Value->SetJustification(ETextJustify::Right);
		AddCell(*Value, ChannelValueWidth, 4.f);

		if (UVerticalBoxSlot* RowSlot = ChannelBody->AddChildToVerticalBox(Row))
		{
			RowSlot->SetPadding(FMargin(10.f, 1.f));
		}

		FChannelRow& Entry = ChannelRows.AddDefaulted_GetRef();
		Entry.Address = Absolute;
		Entry.b16Bit = Channel.b16Bit;
		Entry.Value = Value;
	}
	RefreshChannelValues();
}

void UStagePatchPanel::RefreshChannelValues()
{
	const ALightingFixtureActor* Fixture = ChannelFixture.Get();
	if (!Fixture || !Fixture->GetPatch().IsPatched())
	{
		for (const FChannelRow& Row : ChannelRows)
		{
			if (UTextBlock* Value = Row.Value.Get())
			{
				Value->SetText(LOCTEXT("NoValue", "—"));
			}
		}
		return;
	}

	// Encoded by the fixture itself, so the sheet shows exactly what would go out on the wire.
	TArray<uint8> Universe;
	Universe.SetNumZeroed(StagePatchPanel::UniverseSize);
	Fixture->WriteDMX(Universe);
	for (const FChannelRow& Row : ChannelRows)
	{
		if (UTextBlock* Value = Row.Value.Get())
		{
			Value->SetText(FText::AsNumber(StagePatchFormat::ReadChannelValue(Universe, Row.Address, Row.b16Bit), &FNumberFormattingOptions::DefaultNoGrouping()));
		}
	}
}

void UStagePatchPanel::HandleFieldCommitted(UStagePatchField& Field, int32 Value)
{
	ALightingFixtureActor* Fixture = Field.GetFixture();
	AModularPlayerController* Controller = GetStageController();
	if (Fixture && Controller)
	{
		// Refusals (duplicate fixture ID, locked parameter) are reported by the controller on OnRequestRejected.
		Controller->RequestParameterChange(Fixture, Field.GetParameterId(), FStageParameterValue::MakeInteger(Value));
	}

	// Read back: a refused or clamped edit shows the fixture's real value.
	FStageParameterValue Current;
	if (Fixture && IStageParameterInterface::Execute_GetParameterValue(Fixture, Field.GetParameterId(), Current))
	{
		Field.SetValue(static_cast<float>(Current.Integer));
	}
}

void UStagePatchPanel::HandleFixturesChanged()
{
	RequestRebuild();
}

void UStagePatchPanel::HandleFixtureParameterChanged(AModularBaseActor* Actor, FGameplayTag ParameterId)
{
	if (StagePatchPanel::IsTableParameter(ParameterId))
	{
		RequestRebuild();
	}
	else if (Actor && Actor == ChannelFixture.Get() && ParameterId.MatchesTag(StageCraftTags::Attribute))
	{
		RefreshChannelValues();
	}
}

void UStagePatchPanel::HandleSelectionSetChanged(int32 Count)
{
	// Row highlight and channel map follow the primary selection.
	RequestRebuild();
}

void UStagePatchPanel::HandleRowPicked(int32 RowIndex)
{
	ALightingFixtureActor* Fixture = RowFixtures.IsValidIndex(RowIndex) ? RowFixtures[RowIndex].Get() : nullptr;
	AModularPlayerController* Controller = GetStageController();
	if (Fixture && Controller)
	{
		// Ctrl+Click adds or removes, like a click in the viewport or the Outliner.
		const bool bToggle = FSlateApplication::IsInitialized() && FSlateApplication::Get().GetModifierKeys().IsControlDown();
		Controller->RequestSelectItems({ Fixture }, bToggle ? EStageSelectionClick::Toggle : EStageSelectionClick::Replace);
	}
}

void UStagePatchPanel::ApplyPlan(TConstArrayView<FStageDMXPatchAssignment> Plan, const FText& Empty)
{
	// One undo step; a refusal (an empty plan included, with Empty as its reason) reaches the status bar through the
	// controller. The table follows through OnPatchChanged.
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestPatchFixtures(Plan, Empty);
	}
}

void UStagePatchPanel::HandlePatchUnpatchedClicked()
{
	if (const UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get())
	{
		ApplyPlan(PatchSubsystem->PlanUnpatched(), LOCTEXT("NothingToPatch", "Every fixture is patched, or there is no room left for the unpatched ones."));
	}
}

void UStagePatchPanel::HandleFixConflictsClicked()
{
	if (const UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get())
	{
		ApplyPlan(PatchSubsystem->PlanConflictRepair(), LOCTEXT("NoConflictsToFix", "There are no conflicts to fix, or no room to move the overlapping fixtures to."));
	}
}

void UStagePatchPanel::HandlePatchSelectionClicked()
{
	const UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get();
	const USelectionComponent* SelectionComponent = Selection.Get();
	if (!PatchSubsystem || !SelectionComponent)
	{
		return;
	}

	TArray<ALightingFixtureActor*> Selected;
	for (AActor* Actor : SelectionComponent->GetSelectedActors())
	{
		if (ALightingFixtureActor* Fixture = Cast<ALightingFixtureActor>(Actor))
		{
			Selected.Add(Fixture);
		}
	}

	FStageDMXPatch Start;
	Start.Universe = FMath::RoundToInt32(StartUniverseField->GetValue());
	Start.Address = FMath::RoundToInt32(StartAddressField->GetValue());
	ApplyPlan(PatchSubsystem->PlanConsecutive(Selected, Start),
		FText::Format(LOCTEXT("SelectionDoesNotFit", "The selected fixtures do not fit from {0}."), StagePatchFormat::FormatPatch(Start.Universe, Start.Address)));
}

void UStagePatchPanel::HandleInputClicked()
{
	AModularPlayerController* Controller = GetStageController();
	const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get();
	if (Controller && NetworkSubsystem)
	{
		// The label follows through OnInputChanged; a port that cannot be opened is reported on OnRequestRejected.
		Controller->RequestSetDMXInput(NetworkSubsystem->GetStatus().State != EStageDMXInputState::Listening);
	}
}

void UStagePatchPanel::HandlePatchChanged()
{
	RequestRebuild();
}

void UStagePatchPanel::HandleExportClicked()
{
	// The result (or a refusal) reaches the status bar through OnExportFinished / OnRequestRejected.
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestExportPatchSheet();
	}
}

// --- DMX OUT (Docs/ADR/0019 §9.6) ---

UWidget* UStagePatchPanel::BuildOutputLine()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePatchPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("OutputLine"));
	const auto AddToLine = [Line](UWidget* Widget, float LeftGap, bool bFill)
	{
		if (UHorizontalBoxSlot* WidgetSlot = Line->AddChildToHorizontalBox(Widget))
		{
			WidgetSlot->SetVerticalAlignment(VAlign_Center);
			WidgetSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
			if (bFill)
			{
				WidgetSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	};
	const auto Sized = [this](UWidget* Content, float Width)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->SetHeightOverride(FieldHeight);
		Box->SetContent(Content);
		return Box;
	};

	UTextBlock* Label = nullptr;
	OutputButton = MakeHeaderButton(TEXT("OutputButton"), LOCTEXT("OutputOff", "DMX OUT: OFF"),
		LOCTEXT("OutputTip", "Send the show as Art-Net and/or sACN to real fixtures, a DMX node or a visualiser. Only universes with patched fixtures are sent, and only while the cue list drives the show (it pauses while DMX IN follows a console). Remembered on this PC."),
		&Label);
	OutputButtonLabel = Label;
	AddToLine(OutputButton, 0.f, false);

	Label = nullptr;
	ArtNetModeButton = MakeHeaderButton(TEXT("ArtNetModeButton"), FText::GetEmpty(),
		LOCTEXT("ArtNetModeTip", "Art-Net: off, broadcast to every node on the network (255.255.255.255 on port 6454), or unicast to the address on the right. Click to change."),
		&Label);
	ArtNetModeLabel = Label;
	AddToLine(ArtNetModeButton, 4.f, false);

	Label = nullptr;
	SACNModeButton = MakeHeaderButton(TEXT("SACNModeButton"), FText::GetEmpty(),
		LOCTEXT("SACNModeTip", "sACN (E1.31): off, multicast to each universe's group (239.255.x.y, which every sACN receiver listens to), or unicast to the address on the right. Click to change."),
		&Label);
	SACNModeLabel = Label;
	AddToLine(SACNModeButton, 4.f, false);

	UnicastField = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("UnicastField"));
	FEditableTextBoxStyle TextStyle = MakeInputField(Palette);
	TextStyle.SetPadding(FMargin(6.f, 2.f));
	TextStyle.SetFont(MakeFont(8));
	UnicastField->SetWidgetStyle(TextStyle);
	UnicastField->SetRevertTextOnEscape(true);
	UnicastField->SetHintText(LOCTEXT("UnicastHint", "unicast IP"));
	UnicastField->SetToolTipText(LOCTEXT("UnicastTip", "The IPv4 address the protocols set to UNICAST send to (a DMX node or a visualiser PC). Press Enter to apply."));
	AddToLine(Sized(UnicastField, 104.f), 4.f, false);

	PriorityField = MakeNumberField(100, 0, StageDMXOutput::MaxSACNPriority, 34.f,
		LOCTEXT("PriorityTip", "sACN priority, 0-200 (default 100). A receiver shows the source with the highest priority."));
	AddToLine(MakeText(LOCTEXT("PriorityLabel", "PRI"), 6, TEXT("Bold"), Palette.TextSecondary), 6.f, false);
	AddToLine(Sized(PriorityField, 40.f), 3.f, false);

	RateField = MakeNumberField(StageDMXOutput::MaxRateHz, StageDMXOutput::MinRateHz, StageDMXOutput::MaxRateHz, 30.f,
		LOCTEXT("RateTip", "Frames per second while the look changes, 1-44. Unchanged universes are repeated once a second."));
	AddToLine(MakeText(LOCTEXT("RateLabel", "HZ"), 6, TEXT("Bold"), Palette.TextSecondary), 6.f, false);
	AddToLine(Sized(RateField, 36.f), 3.f, false);

	OutputUniverseBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("OutputUniverses"));
	AddToLine(OutputUniverseBox, 6.f, false);

	// The status gets a line of its own: the controls fill the first one, and the packet counts must stay readable.
	UVerticalBox* Block = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("OutputBlock"));
	Block->AddChildToVerticalBox(Line);
	OutputStatusText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	OutputStatusText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UVerticalBoxSlot* StatusSlot = Block->AddChildToVerticalBox(OutputStatusText))
	{
		StatusSlot->SetPadding(FMargin(2.f, 3.f, 0.f, 0.f));
	}
	return Block;
}

void UStagePatchPanel::RefreshOutputStatus()
{
	if (!OutputStatusText || !OutputButtonLabel)
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get();
	const FStageDMXOutputStatus Status = NetworkSubsystem ? NetworkSubsystem->GetOutputStatus() : FStageDMXOutputStatus();
	const FStageDMXOutputSettings Settings = NetworkSubsystem ? NetworkSubsystem->GetOutputSettings() : FStageDMXOutputSettings();
	const bool bAvailable = NetworkSubsystem != nullptr;
	for (UWidget* Control : TArray<UWidget*>{ OutputButton, ArtNetModeButton, SACNModeButton, UnicastField, PriorityField, RateField })
	{
		if (Control)
		{
			Control->SetIsEnabled(bAvailable);
		}
	}

	const auto ModeText = [](EStageArtNetOutputMode Mode)
	{
		switch (Mode)
		{
		case EStageArtNetOutputMode::Broadcast: return LOCTEXT("ArtNetBroadcast", "ART-NET: BCAST");
		case EStageArtNetOutputMode::Unicast: return LOCTEXT("ArtNetUnicast", "ART-NET: UNICAST");
		default: return LOCTEXT("ArtNetOff", "ART-NET: OFF");
		}
	};
	const auto SACNText = [](EStageSACNOutputMode Mode)
	{
		switch (Mode)
		{
		case EStageSACNOutputMode::Multicast: return LOCTEXT("SACNMulticast", "sACN: MCAST");
		case EStageSACNOutputMode::Unicast: return LOCTEXT("SACNUnicast", "sACN: UNICAST");
		default: return LOCTEXT("SACNOff", "sACN: OFF");
		}
	};
	ArtNetModeLabel->SetText(ModeText(Settings.ArtNet));
	ArtNetModeLabel->SetColorAndOpacity(FSlateColor(Settings.ArtNet == EStageArtNetOutputMode::Off ? Palette.TextSecondary : Palette.TextPrimary));
	SACNModeLabel->SetText(SACNText(Settings.SACN));
	SACNModeLabel->SetColorAndOpacity(FSlateColor(Settings.SACN == EStageSACNOutputMode::Off ? Palette.TextSecondary : Palette.TextPrimary));
	// The fields read the saved settings back, so a refused or sanitized edit snaps back; one being typed in is left alone.
	if (!UnicastField->HasKeyboardFocus())
	{
		UnicastField->SetText(FText::FromString(Settings.UnicastAddress));
	}
	if (!PriorityField->HasKeyboardFocus())
	{
		PriorityField->SetValue(static_cast<float>(Settings.SACNPriority));
	}
	if (!RateField->HasKeyboardFocus())
	{
		RateField->SetValue(static_cast<float>(Settings.RateHz));
	}

	const UShowControlSubsystem* Show = ShowControl.Get();
	const bool bPaused = Show && Show->GetControlSource() == EStageControlSource::External;
	switch (Status.State)
	{
	case EStageDMXOutputState::Sending:
		OutputButtonLabel->SetText(LOCTEXT("OutputOn", "DMX OUT: ON"));
		OutputButtonLabel->SetColorAndOpacity(FSlateColor(Palette.AccentColor));
		OutputStatusText->SetText(bPaused
			? FText::Format(LOCTEXT("OutputPaused", "Paused while DMX IN drives the show · {0}"), FText::FromString(Status.Target))
			: FText::Format(LOCTEXT("OutputLive", "Sending {0} {0}|plural(one=universe,other=universes) · {1} · {2} Art-Net, {3} sACN packets{4}"),
				Status.Universes.Num(), FText::FromString(Status.Target), FText::AsNumber(Status.ArtNetPackets), FText::AsNumber(Status.SACNPackets),
				Status.SendErrors > 0 ? FText::Format(LOCTEXT("OutputErrors", " · {0} not sent"), FText::AsNumber(Status.SendErrors)) : FText::GetEmpty()));
		OutputStatusText->SetColorAndOpacity(FSlateColor(Status.SendErrors > 0 ? Palette.WarningColor : Palette.TextSecondary));
		break;
	case EStageDMXOutputState::Failed:
		OutputButtonLabel->SetText(LOCTEXT("OutputOffFailed", "DMX OUT: OFF"));
		OutputButtonLabel->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		OutputStatusText->SetText(Status.Error);
		OutputStatusText->SetColorAndOpacity(FSlateColor(Palette.WarningColor));
		break;
	default:
		OutputButtonLabel->SetText(LOCTEXT("OutputOffIdle", "DMX OUT: OFF"));
		OutputButtonLabel->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		OutputStatusText->SetText(LOCTEXT("OutputIdle", "Network output off. Switch on to drive fixtures, a node or a visualiser over Art-Net or sACN."));
		OutputStatusText->SetColorAndOpacity(FSlateColor(Palette.TextSecondary));
		break;
	}
	OutputStatusText->SetToolTipText(OutputStatusText->GetText());

	// The chips: lit while their universe is sent (muted ones stay dark).
	for (int32 Index = 0; Index < OutputUniverseButtons.Num() && Index < OutputUniverses.Num(); ++Index)
	{
		if (UStageChoiceButton* Chip = OutputUniverseButtons[Index])
		{
			const bool bSent = !StageDMXOutput::IsMuted(Settings, OutputUniverses[Index]);
			Chip->SetStyle(StageCraftWidgetStyle::MakeFlatButton(bSent ? StageCraftWidgetStyle::WithAlpha(Palette.AccentColor, 0.28f) : FLinearColor::Transparent,
				bSent ? StageCraftWidgetStyle::WithAlpha(Palette.AccentColor, 0.38f) : Palette.RowHover, Palette.Divider));
			if (UTextBlock* ChipLabel = Cast<UTextBlock>(Chip->GetChildAt(0)))
			{
				ChipLabel->SetColorAndOpacity(FSlateColor(bSent ? Palette.TextPrimary : Palette.TextSecondary));
			}
		}
	}
}

void UStagePatchPanel::RebuildOutputUniverses()
{
	if (!OutputUniverseBox)
	{
		return;
	}
	OutputUniverseBox->ClearChildren();
	OutputUniverseButtons.Reset();
	const UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get();
	OutputUniverses = PatchSubsystem ? PatchSubsystem->GetPatchedUniverses() : TArray<int32>();
	for (int32 Index = 0; Index < OutputUniverses.Num(); ++Index)
	{
		UStageChoiceButton* Chip = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Chip->SetChoiceIndex(Index);
		Chip->SetToolTipText(FText::Format(LOCTEXT("ChipTip", "Universe {0}: lit while DMX OUT sends it. Click to mute or unmute it on this PC."), OutputUniverses[Index]));
		Chip->OnPicked.BindUObject(this, &ThisClass::HandleOutputUniversePicked);
		UTextBlock* ChipLabel = MakeText(FText::Format(LOCTEXT("ChipLabel", "U{0}"), OutputUniverses[Index]), 6, TEXT("Bold"), GetTheme().TextSecondary);
		if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Chip->AddChild(ChipLabel)))
		{
			LabelSlot->SetPadding(FMargin(5.f, 3.f));
			LabelSlot->SetHorizontalAlignment(HAlign_Center);
		}
		if (UHorizontalBoxSlot* ChipSlot = OutputUniverseBox->AddChildToHorizontalBox(Chip))
		{
			ChipSlot->SetPadding(FMargin(Index == 0 ? 0.f : 2.f, 0.f, 0.f, 0.f));
			ChipSlot->SetVerticalAlignment(VAlign_Center);
		}
		OutputUniverseButtons.Add(Chip);
	}
	RefreshOutputStatus();
}

void UStagePatchPanel::ApplyOutputSettings(const FStageDMXOutputSettings& Settings)
{
	if (AModularPlayerController* Controller = GetStageController())
	{
		// A refusal reaches the status bar through OnRequestRejected; the line follows through OnOutputChanged.
		Controller->RequestApplyDMXOutputSettings(Settings);
	}
	RefreshOutputStatus(); // Snaps the fields back when nothing changed (no broadcast then).
}

void UStagePatchPanel::HandleOutputClicked()
{
	AModularPlayerController* Controller = GetStageController();
	const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get();
	if (Controller && NetworkSubsystem)
	{
		Controller->RequestSetDMXOutput(NetworkSubsystem->GetOutputStatus().State != EStageDMXOutputState::Sending);
	}
}

void UStagePatchPanel::HandleArtNetModeClicked()
{
	if (const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get())
	{
		FStageDMXOutputSettings Settings = NetworkSubsystem->GetOutputSettings();
		Settings.ArtNet = static_cast<EStageArtNetOutputMode>((static_cast<uint8>(Settings.ArtNet) + 1) % 3);
		ApplyOutputSettings(Settings);
	}
}

void UStagePatchPanel::HandleSACNModeClicked()
{
	if (const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get())
	{
		FStageDMXOutputSettings Settings = NetworkSubsystem->GetOutputSettings();
		Settings.SACN = static_cast<EStageSACNOutputMode>((static_cast<uint8>(Settings.SACN) + 1) % 3);
		ApplyOutputSettings(Settings);
	}
}

void UStagePatchPanel::HandleUnicastCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get();
	if (!NetworkSubsystem || CommitMethod == ETextCommit::OnCleared)
	{
		RefreshOutputStatus();
		return;
	}
	FStageDMXOutputSettings Settings = NetworkSubsystem->GetOutputSettings();
	if (Settings.UnicastAddress == Text.ToString().TrimStartAndEnd())
	{
		return;
	}
	Settings.UnicastAddress = Text.ToString();
	ApplyOutputSettings(Settings);
}

void UStagePatchPanel::HandlePriorityCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	if (const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get())
	{
		FStageDMXOutputSettings Settings = NetworkSubsystem->GetOutputSettings();
		if (Settings.SACNPriority != FMath::RoundToInt32(InValue))
		{
			Settings.SACNPriority = FMath::RoundToInt32(InValue);
			ApplyOutputSettings(Settings);
		}
	}
}

void UStagePatchPanel::HandleRateCommitted(float InValue, ETextCommit::Type CommitMethod)
{
	if (const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get())
	{
		FStageDMXOutputSettings Settings = NetworkSubsystem->GetOutputSettings();
		if (Settings.RateHz != FMath::RoundToInt32(InValue))
		{
			Settings.RateHz = FMath::RoundToInt32(InValue);
			ApplyOutputSettings(Settings);
		}
	}
}

void UStagePatchPanel::HandleOutputUniversePicked(int32 Index)
{
	const UStageDMXNetworkSubsystem* NetworkSubsystem = Network.Get();
	if (!NetworkSubsystem || !OutputUniverses.IsValidIndex(Index))
	{
		return;
	}
	FStageDMXOutputSettings Settings = NetworkSubsystem->GetOutputSettings();
	const int32 Universe = OutputUniverses[Index];
	if (Settings.MutedUniverses.Remove(Universe) == 0)
	{
		Settings.MutedUniverses.Add(Universe);
	}
	ApplyOutputSettings(Settings);
}

// --- Patch options and fixture schedule (Docs/ADR/0019 §10) ---

void UStagePatchPanel::HandlePatchOptionPicked(int32 Index)
{
	using namespace StagePatchPanel;
	const int32 Row = Index / PatchOptionCount;
	ALightingFixtureActor* Fixture = RowFixtures.IsValidIndex(Row) ? RowFixtures[Row].Get() : nullptr;
	AModularPlayerController* Controller = GetStageController();
	if (!Fixture || !Controller)
	{
		return;
	}
	const FStageDMXPatchOptions Options = Fixture->GetPatchOptions();
	const EPatchOption Option = static_cast<EPatchOption>(Index % PatchOptionCount);
	const FGameplayTag ParameterId = Option == EPatchOption::InvertPan ? StageCraftTags::Param_Patch_InvertPan
		: Option == EPatchOption::InvertTilt ? StageCraftTags::Param_Patch_InvertTilt : StageCraftTags::Param_Patch_SwapPanTilt;
	const bool bOn = Option == EPatchOption::InvertPan ? Options.bInvertPan : Option == EPatchOption::InvertTilt ? Options.bInvertTilt : Options.bSwapPanTilt;
	// One undo step (a patch parameter); the row follows through the fixture's OnParameterChanged.
	Controller->RequestParameterChange(Fixture, ParameterId, FStageParameterValue::MakeBool(!bOn));
}

UWidget* UStagePatchPanel::BuildImportBar()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	ImportBar = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ImportBar"));
	ImportBar->SetBrush(MakeBox(WithAlpha(Palette.AccentColor, 0.12f)));
	ImportBar->SetPadding(FMargin(10.f, 6.f));
	ImportBar->SetVisibility(ESlateVisibility::Collapsed);

	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	ImportBar->SetContent(Line);
	UVerticalBox* Texts = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (UHorizontalBoxSlot* TextsSlot = Line->AddChildToHorizontalBox(Texts))
	{
		TextsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextsSlot->SetVerticalAlignment(VAlign_Center);
	}
	ImportSummaryText = MakeText(FText::GetEmpty(), 8, TEXT("Bold"), Palette.TextPrimary);
	ImportSummaryText->SetAutoWrapText(true);
	Texts->AddChildToVerticalBox(ImportSummaryText);
	ImportDetailsText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	ImportDetailsText->SetAutoWrapText(true);
	Texts->AddChildToVerticalBox(ImportDetailsText);

	ImportApplyButton = MakeHeaderButton(TEXT("ImportApplyButton"), LOCTEXT("ImportApply", "APPLY"),
		LOCTEXT("ImportApplyTip", "Re-patch the fixtures as shown, in one undo step. Skipped rows change nothing."));
	if (UHorizontalBoxSlot* ApplySlot = Line->AddChildToHorizontalBox(ImportApplyButton))
	{
		ApplySlot->SetVerticalAlignment(VAlign_Center);
		ApplySlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}
	ImportCancelButton = MakeHeaderButton(TEXT("ImportCancelButton"), LOCTEXT("ImportCancel", "CANCEL"),
		LOCTEXT("ImportCancelTip", "Close without changing the patch."));
	if (UHorizontalBoxSlot* CancelSlot = Line->AddChildToHorizontalBox(ImportCancelButton))
	{
		CancelSlot->SetVerticalAlignment(VAlign_Center);
		CancelSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
	}
	return ImportBar;
}

void UStagePatchPanel::RefreshImportBar()
{
	if (!ImportBar)
	{
		return;
	}
	if (!PendingImport.IsSet())
	{
		ImportBar->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	const FStagePatchScheduleImport& Import = PendingImport.GetValue();
	ImportBar->SetVisibility(ESlateVisibility::Visible);
	ImportSummaryText->SetText(FText::Format(LOCTEXT("ImportSummary", "Fixture schedule: {0}"), Import.Summary));
	ImportDetailsText->SetText(Import.Details);
	ImportDetailsText->SetVisibility(Import.Details.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	ImportApplyButton->SetIsEnabled(!Import.Assignments.IsEmpty());
}

void UStagePatchPanel::HandleImportClicked()
{
	const UStageDMXPatchSubsystem* PatchSubsystem = Patching.Get();
	AModularPlayerController* Controller = GetStageController();
	if (!PatchSubsystem || !Controller)
	{
		return;
	}
	if (LastImportDirectory.IsEmpty())
	{
		LastImportDirectory = StageReportFiles::GetOutputDirectory();
	}
	TArray<FString> Files;
	if (!StageFileDialog::OpenFiles(LOCTEXT("ImportTitle", "Import a Fixture Schedule"), LastImportDirectory,
		TEXT("CSV files (*.csv)|*.csv|Text files (*.txt)|*.txt"), Files) || Files.IsEmpty())
	{
		return;
	}
	LastImportDirectory = FPaths::GetPath(Files[0]);

	// A file that cannot be read or is not a schedule is refused on the status bar; otherwise the plan waits for APPLY.
	FStagePatchScheduleImport Import;
	if (Controller->RequestPlanPatchSchedule(Files[0], Import).IsSuccess())
	{
		PendingImport = MoveTemp(Import);
	}
	RefreshImportBar();
}

void UStagePatchPanel::HandleImportApplyClicked()
{
	AModularPlayerController* Controller = GetStageController();
	if (Controller && PendingImport.IsSet())
	{
		// A refusal reaches the status bar; the table follows through OnPatchChanged.
		Controller->RequestPatchFixtures(PendingImport->Assignments, LOCTEXT("ImportNothing", "The schedule changes nothing on this stage."));
	}
	PendingImport.Reset();
	RefreshImportBar();
}

void UStagePatchPanel::HandleImportCancelClicked()
{
	PendingImport.Reset();
	RefreshImportBar();
}

#undef LOCTEXT_NAMESPACE
