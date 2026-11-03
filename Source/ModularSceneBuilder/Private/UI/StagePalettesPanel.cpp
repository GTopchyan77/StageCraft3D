// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StagePalettesPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/ShowControlSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StagePalettesPanel)

#define LOCTEXT_NAMESPACE "StagePalettesPanel"

namespace StagePalettesPanel
{
	constexpr float TileWidth = 104.f;
	constexpr float TileHeight = 52.f;
	constexpr float SwatchHeight = 8.f;
	constexpr float PaletteNameFieldHeight = 22.f;

	// Header buttons report the kind tabs as their EStagePaletteKind value; the record buttons come after.
	constexpr int32 KindCount = static_cast<int32>(EStagePaletteKind::Focus) + 1;
	constexpr int32 ChoiceRecord = 10;
	constexpr int32 ChoiceRecordPerFixture = 11;
	// Current-palette row.
	constexpr int32 ChoiceUpdate = 0;
	constexpr int32 ChoiceDelete = 1;

	FText KindLabel(EStagePaletteKind Kind)
	{
		switch (Kind)
		{
		case EStagePaletteKind::Intensity:
			return LOCTEXT("KindIntensity", "INTENSITY");
		case EStagePaletteKind::Position:
			return LOCTEXT("KindPosition", "POSITION");
		case EStagePaletteKind::Color:
			return LOCTEXT("KindColor", "COLOR");
		case EStagePaletteKind::Beam:
			return LOCTEXT("KindBeam", "BEAM");
		case EStagePaletteKind::Focus:
			return LOCTEXT("KindFocus", "FOCUS");
		default:
			return LOCTEXT("KindAll", "ALL");
		}
	}

	/** The second line of a tile: what the palette holds. */
	FText DescribeContent(const FStagePalette& Palette)
	{
		if (!Palette.FixtureValues.IsEmpty())
		{
			return FText::Format(LOCTEXT("PerFixture", "{0} {0}|plural(one=fixture,other=fixtures)"), FText::AsNumber(Palette.FixtureValues.Num()));
		}
		if (Palette.bHasColor)
		{
			return LOCTEXT("AnyColorFixture", "any colour fixture");
		}
		return FText::Format(LOCTEXT("GlobalValues", "{0} {0}|plural(one=value,other=values)"), FText::AsNumber(Palette.GlobalValues.Num()));
	}

	/** "Used by cues 1, 3 and 5" for the current-palette row. */
	FText DescribeUsage(TConstArrayView<float> CueNumbers)
	{
		if (CueNumbers.IsEmpty())
		{
			return LOCTEXT("NotUsed", "Not used by any cue");
		}
		FString List;
		for (int32 Index = 0; Index < CueNumbers.Num(); ++Index)
		{
			List += (Index == 0 ? TEXT("") : TEXT(", ")) + FText::AsNumber(CueNumbers[Index]).ToString();
		}
		return FText::Format(LOCTEXT("UsedBy", "Used by {0}|plural(one=cue,other=cues) {1}"), FText::AsNumber(CueNumbers.Num()), FText::FromString(List));
	}
}

const UStageCraftUITheme& UStagePalettesPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStagePalettesPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStagePalettesPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

FButtonStyle UStagePalettesPanel::MakeButtonStyle(bool bLit) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();
	const FLinearColor Fill = bLit ? WithAlpha(UITheme.AccentColor, 0.35f) : FLinearColor::Transparent;
	FButtonStyle Style = MakeFlatButton(Fill, bLit ? WithAlpha(UITheme.AccentColor, 0.5f) : UITheme.RowHover, UITheme.Divider);
	Style.SetNormal(MakeBox(Fill, CornerRadius, UITheme.Divider, 1.f));
	return Style;
}

void UStagePalettesPanel::SetButtonLit(UStageChoiceButton& Button, bool bLit)
{
	Button.SetStyle(MakeButtonStyle(bLit));
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetChildAt(0)))
	{
		Label->SetColorAndOpacity(FSlateColor(bLit ? GetTheme().TextPrimary : GetTheme().TextSecondary));
	}
}

UStageChoiceButton* UStagePalettesPanel::MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit)
{
	const UStageCraftUITheme& UITheme = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetStyle(MakeButtonStyle(bLit));
	Button->SetToolTipText(ToolTip);
	Button->SetChoiceIndex(ChoiceIndex);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), bLit ? UITheme.TextPrimary : UITheme.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(6.f, 4.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return Button;
}

void UStagePalettesPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStagePalettesPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(UITheme.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildHeader());
	CurrentPaletteRow = BuildCurrentPaletteRow();
	Column->AddChildToVerticalBox(CurrentPaletteRow);

	EmptyText = MakeText(LOCTEXT("Empty", "No palettes of this kind yet. Set a look on some fixtures, select them, then RECORD it."), 8, TEXT("Italic"), UITheme.TextSecondary);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("PoolScroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(UITheme));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(8.f, 6.f));
	}
	Pool = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass(), TEXT("Pool"));
	Pool->SetInnerSlotPadding(FVector2D(4.f, 4.f));
	Scroll->AddChild(Pool);
}

UWidget* UStagePalettesPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePalettesPanel;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(UITheme.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UWrapBox* Line = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	Line->SetInnerSlotPadding(FVector2D(2.f, 4.f));
	Header->SetContent(Line);

	UTextBlock* Title = MakeText(LOCTEXT("Title", "PALETTES"), 9, TEXT("Bold"), UITheme.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Named looks for one feature. Click a palette to apply it to the selected fixtures; cues stored with it follow it when it changes."));
	Line->AddChildToWrapBox(Title);

	KindTabs.Reset();
	for (int32 Kind = 0; Kind < KindCount; ++Kind)
	{
		UStageChoiceButton* Tab = MakeButton(KindLabel(static_cast<EStagePaletteKind>(Kind)),
			LOCTEXT("KindTip", "Show this kind of palette; RECORD records this kind of attribute (ALL records any)."), Kind, false);
		Tab->OnPicked.BindUObject(this, &ThisClass::HandleHeaderPicked);
		Line->AddChildToWrapBox(Tab);
		KindTabs.Add(Tab);
	}

	UStageChoiceButton* Record = MakeButton(LOCTEXT("Record", "RECORD"),
		LOCTEXT("RecordTip", "Record what the selected fixtures show as a new palette of this kind, for any fixture: a colour palette keeps the primary fixture's colour, which every fixture mixes its own way."),
		ChoiceRecord, true);
	Record->OnPicked.BindUObject(this, &ThisClass::HandleHeaderPicked);
	Line->AddChildToWrapBox(Record);
	UStageChoiceButton* RecordPerFixture = MakeButton(LOCTEXT("RecordPerFixture", "RECORD PER FIXTURE"),
		LOCTEXT("RecordPerFixtureTip", "Record each selected fixture's own values exactly, as a new palette of this kind (positions aimed one by one, for example)."),
		ChoiceRecordPerFixture, false);
	RecordPerFixture->OnPicked.BindUObject(this, &ThisClass::HandleHeaderPicked);
	Line->AddChildToWrapBox(RecordPerFixture);
	return Header;
}

UWidget* UStagePalettesPanel::BuildCurrentPaletteRow()
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePalettesPanel;
	const UStageCraftUITheme& UITheme = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CurrentPalette"));
	Row->SetBrush(MakeBox(UITheme.PanelBackground));
	Row->SetPadding(FMargin(10.f, 4.f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Line);

	NameField = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("PaletteName"));
	FEditableTextBoxStyle TextStyle = MakeInputField(UITheme);
	TextStyle.SetPadding(FMargin(6.f, 2.f));
	NameField->SetWidgetStyle(TextStyle);
	NameField->SetRevertTextOnEscape(true);
	NameField->SetToolTipText(LOCTEXT("NameTip", "The palette's name. Press Enter to rename it."));
	NameField->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleNameCommitted);
	USizeBox* NameBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	NameBox->SetHeightOverride(PaletteNameFieldHeight);
	NameBox->SetWidthOverride(150.f);
	NameBox->SetContent(NameField);
	if (UHorizontalBoxSlot* NameSlot = Line->AddChildToHorizontalBox(NameBox))
	{
		NameSlot->SetVerticalAlignment(VAlign_Center);
	}

	UsageText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), UITheme.TextSecondary);
	UsageText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UHorizontalBoxSlot* UsageSlot = Line->AddChildToHorizontalBox(UsageText))
	{
		UsageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UsageSlot->SetVerticalAlignment(VAlign_Center);
		UsageSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}

	UStageChoiceButton* Update = MakeButton(LOCTEXT("Update", "UPDATE"),
		LOCTEXT("UpdateTip", "Re-record this palette from the selected fixtures. Every cue that uses it changes with it."), ChoiceUpdate, false);
	Update->OnPicked.BindUObject(this, &ThisClass::HandleCurrentPalettePicked);
	DeleteButton = MakeButton(LOCTEXT("Delete", "DELETE"), FText::GetEmpty(), ChoiceDelete, false);
	DeleteButton->OnPicked.BindUObject(this, &ThisClass::HandleCurrentPalettePicked);
	for (UWidget* Button : { static_cast<UWidget*>(Update), static_cast<UWidget*>(DeleteButton) })
	{
		if (UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
	return Row;
}

UWidget* UStagePalettesPanel::MakeTile(const FStagePalette& Palette, bool bCurrent)
{
	using namespace StageCraftWidgetStyle;
	using namespace StagePalettesPanel;
	const UStageCraftUITheme& UITheme = GetTheme();

	UStageChoiceButton* Tile = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Tile->SetStyle(MakeButtonStyle(bCurrent));
	Tile->SetChoiceIndex(TilePaletteIds.Num());
	Tile->SetToolTipText(FText::Format(LOCTEXT("TileTip", "Apply {0} to the selected fixtures."), Palette.Name));
	Tile->OnPicked.BindUObject(this, &ThisClass::HandlePalettePicked);

	UVerticalBox* Face = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	// The swatch: the palette's colour, or a quiet bar for palettes that hold no single colour.
	UBorder* Swatch = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Swatch->SetBrush(MakeBox(Palette.bHasColor ? Palette.Color : UITheme.Divider, CornerRadius));
	USizeBox* SwatchBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SwatchBox->SetHeightOverride(SwatchHeight);
	SwatchBox->SetContent(Swatch);
	if (UVerticalBoxSlot* SwatchSlot = Face->AddChildToVerticalBox(SwatchBox))
	{
		SwatchSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 3.f));
	}
	UTextBlock* Name = MakeText(Palette.Name, 8, TEXT("Bold"), UITheme.TextPrimary);
	Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Face->AddChildToVerticalBox(Name);
	Face->AddChildToVerticalBox(MakeText(DescribeContent(Palette), 7, TEXT("Regular"), UITheme.TextSecondary));
	if (UButtonSlot* FaceSlot = Cast<UButtonSlot>(Tile->AddChild(Face)))
	{
		FaceSlot->SetPadding(FMargin(6.f, 4.f));
		FaceSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	USizeBox* Sized = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Sized->SetWidthOverride(TileWidth);
	Sized->SetHeightOverride(TileHeight);
	Sized->SetContent(Tile);
	return Sized;
}

void UStagePalettesPanel::NativeConstruct()
{
	Super::NativeConstruct();
	UWorld* World = GetWorld();
	if (UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(World))
	{
		Library->OnPalettesChanged.AddUniqueDynamic(this, &ThisClass::HandleShowDataChanged);
		BoundLibrary = Library;
	}
	if (UShowControlSubsystem* Show = UWorld::GetSubsystem<UShowControlSubsystem>(World))
	{
		Show->OnCueListChanged.AddUniqueDynamic(this, &ThisClass::HandleShowDataChanged);
		BoundShow = Show;
	}
	Rebuild();
}

void UStagePalettesPanel::NativeDestruct()
{
	if (UStageShowLibrarySubsystem* Library = BoundLibrary.Get())
	{
		Library->OnPalettesChanged.RemoveDynamic(this, &ThisClass::HandleShowDataChanged);
	}
	if (UShowControlSubsystem* Show = BoundShow.Get())
	{
		Show->OnCueListChanged.RemoveDynamic(this, &ThisClass::HandleShowDataChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	BoundLibrary.Reset();
	BoundShow.Reset();
	Super::NativeDestruct();
}

void UStagePalettesPanel::HandleShowDataChanged()
{
	RequestRebuild();
}

void UStagePalettesPanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	// Next tick: never inside the click that caused the change, and a burst of changes is one rebuild.
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStagePalettesPanel::Rebuild()
{
	using namespace StagePalettesPanel;
	RebuildTimer.Invalidate();
	if (!Pool)
	{
		return;
	}
	const UStageShowLibrarySubsystem* Library = BoundLibrary.Get();
	const UShowControlSubsystem* Show = BoundShow.Get();

	for (int32 Kind = 0; Kind < KindTabs.Num(); ++Kind)
	{
		SetButtonLit(*KindTabs[Kind], static_cast<EStagePaletteKind>(Kind) == ShownKind);
	}

	// The pool: ALL shows every palette, a kind tab only its own.
	Pool->ClearChildren();
	TilePaletteIds.Reset();
	const TArray<FStagePalette> NoPalettes;
	const TArray<FStagePalette>& Palettes = Library ? Library->GetPalettes() : NoPalettes;
	const FStagePalette* Current = nullptr;
	for (const FStagePalette& Palette : Palettes)
	{
		if (ShownKind != EStagePaletteKind::All && Palette.Kind != ShownKind)
		{
			continue;
		}
		const bool bCurrent = Palette.Id == CurrentPaletteId;
		Current = bCurrent ? &Palette : Current;
		Pool->AddChildToWrapBox(MakeTile(Palette, bCurrent));
		TilePaletteIds.Add(Palette.Id);
	}
	if (!Current)
	{
		CurrentPaletteId.Invalidate();
	}
	EmptyText->SetVisibility(TilePaletteIds.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	CurrentPaletteRow->SetVisibility(Current ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (!Current)
	{
		return;
	}
	if (!NameField->HasKeyboardFocus())
	{
		NameField->SetText(Current->Name);
	}
	const TArray<float> UsedBy = Show ? StagePaletteRules::FindCuesUsing(Show->GetCues(), Current->Id) : TArray<float>();
	UsageText->SetText(DescribeUsage(UsedBy));
	DeleteButton->SetToolTipText(UsedBy.IsEmpty()
		? LOCTEXT("DeleteTip", "Delete this palette. Undoable.")
		: LOCTEXT("DeleteUsedTip", "Delete this palette. The cues and programmer values that use it keep its current values as their own, so nothing on stage changes. Undoable."));
}

void UStagePalettesPanel::HandleHeaderPicked(int32 ChoiceIndex)
{
	using namespace StagePalettesPanel;
	if (ChoiceIndex >= 0 && ChoiceIndex < KindCount)
	{
		ShownKind = static_cast<EStagePaletteKind>(ChoiceIndex);
		RequestRebuild();
		return;
	}
	AModularPlayerController* Controller = GetStageController();
	if (!Controller || (ChoiceIndex != ChoiceRecord && ChoiceIndex != ChoiceRecordPerFixture))
	{
		return;
	}
	const EStagePaletteScope Scope = ChoiceIndex == ChoiceRecordPerFixture ? EStagePaletteScope::PerFixture : EStagePaletteScope::Global;
	if (const FGuid Recorded = Controller->RequestRecordPalette(ShownKind, FText::GetEmpty(), Scope); Recorded.IsValid())
	{
		CurrentPaletteId = Recorded;
	}
}

void UStagePalettesPanel::HandlePalettePicked(int32 TileIndex)
{
	if (!TilePaletteIds.IsValidIndex(TileIndex))
	{
		return;
	}
	CurrentPaletteId = TilePaletteIds[TileIndex];
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestApplyPalette(CurrentPaletteId);
	}
	// The tile becomes current: a view-only change no delegate announces.
	RequestRebuild();
}

void UStagePalettesPanel::HandleCurrentPalettePicked(int32 ChoiceIndex)
{
	using namespace StagePalettesPanel;
	AModularPlayerController* Controller = GetStageController();
	const UStageShowLibrarySubsystem* Library = BoundLibrary.Get();
	const FStagePalette* Palette = Library ? Library->FindPalette(CurrentPaletteId) : nullptr;
	if (!Controller || !Palette)
	{
		return;
	}
	if (ChoiceIndex == ChoiceUpdate)
	{
		// Re-recorded the way it was recorded: per fixture if it holds per-fixture values, otherwise for any fixture.
		const EStagePaletteScope Scope = Palette->FixtureValues.IsEmpty() ? EStagePaletteScope::Global : EStagePaletteScope::PerFixture;
		Controller->RequestUpdatePalette(CurrentPaletteId, Scope);
	}
	else if (ChoiceIndex == ChoiceDelete)
	{
		Controller->RequestDeletePalette(CurrentPaletteId);
	}
}

void UStagePalettesPanel::HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	// Escape reverts the text and must not write; an unchanged or empty name writes nothing either (read back on rebuild).
	AModularPlayerController* Controller = GetStageController();
	if (CommitMethod == ETextCommit::OnCleared || !Controller || !CurrentPaletteId.IsValid())
	{
		return;
	}
	if (!Controller->RequestRenamePalette(CurrentPaletteId, InText))
	{
		RequestRebuild();
	}
}

#undef LOCTEXT_NAMESPACE
