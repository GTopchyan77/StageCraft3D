// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StagePatchPanel.h"

#include "Actors/LightingFixtureActor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/SelectionComponent.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/LightingFixtureData.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Interaction/StageParameterInterface.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
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

	/** Patch edits that change what the table shows; attribute changes only touch channel values. */
	bool IsTableParameter(const FGameplayTag& ParameterId)
	{
		return ParameterId == StageCraftTags::Param_Patch_FixtureId || ParameterId == StageCraftTags::Param_Patch_Universe
			|| ParameterId == StageCraftTags::Param_Patch_Address || ParameterId == StageCraftTags::Param_Info_Label
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
	if (UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(BuildPatchButton()))
	{
		ButtonSlot->SetVerticalAlignment(VAlign_Center);
	}

	ExportButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("ExportButton"));
	FButtonStyle ExportStyle = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	ExportStyle.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	ExportButton->SetStyle(ExportStyle);
	ExportButton->SetToolTipText(LOCTEXT("ExportTip", "Export the patch sheet as a print-ready PDF and a CSV to the Reports folder (File > Production > Open Reports Folder)."));
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(ExportButton->AddChild(MakeText(LOCTEXT("Export", "EXPORT PDF"), 7, TEXT("Bold"), Palette.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 6.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	if (UHorizontalBoxSlot* ExportSlot = Line->AddChildToHorizontalBox(ExportButton))
	{
		ExportSlot->SetVerticalAlignment(VAlign_Center);
		ExportSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
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
	return RowButton;
}

UWidget* UStagePatchPanel::BuildPatchButton()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	PatchUnpatchedButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("PatchUnpatchedButton"));
	FButtonStyle Style = MakeFlatButton(FLinearColor::Transparent, Palette.RowHover, Palette.Divider);
	Style.SetNormal(MakeBox(FLinearColor::Transparent, CornerRadius, Palette.Divider, 1.f));
	PatchUnpatchedButton->SetStyle(Style);
	PatchUnpatchedButton->SetToolTipText(LOCTEXT("PatchUnpatchedTip", "Give every unpatched fixture the next free DMX address, in fixture ID order."));
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(PatchUnpatchedButton->AddChild(MakeText(LOCTEXT("PatchUnpatched", "PATCH UNPATCHED"), 7, TEXT("Bold"), Palette.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 6.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return PatchUnpatchedButton;
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

	const AModularPlayerController* Controller = GetStageController();
	USelectionComponent* SelectionComponent = Controller ? Controller->GetSelection() : nullptr;
	Selection = SelectionComponent;
	if (SelectionComponent)
	{
		SelectionComponent->OnSelectionSetChanged.AddUniqueDynamic(this, &ThisClass::HandleSelectionSetChanged);
	}
	Rebuild();
}

void UStagePatchPanel::NativeDestruct()
{
	if (UShowControlSubsystem* Subsystem = ShowControl.Get())
	{
		Subsystem->OnFixturesChanged.RemoveDynamic(this, &ThisClass::HandleFixturesChanged);
	}
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

	const TArray<int32> Conflicts = Subsystem ? Subsystem->FindPatchConflicts() : TArray<int32>();
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
	RebuildChannelMap();
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

void UStagePatchPanel::HandlePatchUnpatchedClicked()
{
	const UShowControlSubsystem* Subsystem = ShowControl.Get();
	AModularPlayerController* Controller = GetStageController();
	if (!Subsystem || !Controller)
	{
		return;
	}

	TArray<ALightingFixtureActor*> Fixtures = Subsystem->GetFixtures();
	Fixtures.Sort([](const ALightingFixtureActor& A, const ALightingFixtureActor& B) { return A.GetFixtureId() < B.GetFixtureId(); });
	for (ALightingFixtureActor* Fixture : Fixtures)
	{
		if (Fixture->GetPatch().IsPatched())
		{
			continue;
		}
		// Asked again per fixture: the previous fixture's new patch is already occupied.
		const FStageDMXPatch Free = Subsystem->FindNextFreePatch(Fixture->GetDMXFootprint());
		if (Free.IsPatched())
		{
			Controller->RequestParameterChange(Fixture, StageCraftTags::Param_Patch_Universe, FStageParameterValue::MakeInteger(Free.Universe));
			Controller->RequestParameterChange(Fixture, StageCraftTags::Param_Patch_Address, FStageParameterValue::MakeInteger(Free.Address));
		}
	}
}

void UStagePatchPanel::HandleExportClicked()
{
	// The result (or a refusal) reaches the status bar through OnExportFinished / OnRequestRejected.
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestExportPatchSheet();
	}
}

#undef LOCTEXT_NAMESPACE
