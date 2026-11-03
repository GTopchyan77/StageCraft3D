// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageGroupsPanel.h"

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
#include "Components/WrapBoxSlot.h"
#include "Engine/World.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageProgrammerSubsystem.h"
#include "Subsystems/StageShowLibrarySubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageGroupsPanel)

#define LOCTEXT_NAMESPACE "StageGroupsPanel"

namespace StageGroupsPanel
{
	constexpr float TileWidth = 104.f;
	constexpr float TileHeight = 46.f;
	constexpr float NameFieldHeight = 22.f;

	// Programmer bar and current-group row buttons report these choice indices.
	constexpr int32 ChoiceRecord = 0;
	constexpr int32 ChoiceClear = 1;
	constexpr int32 ChoiceHighlight = 2;
	constexpr int32 ChoiceFan = 3;
	constexpr int32 ChoiceUpdate = 0;
	constexpr int32 ChoiceDelete = 1;

	FText FanLabel(EStageFanMode Mode)
	{
		switch (Mode)
		{
		case EStageFanMode::Linear:
			return LOCTEXT("FanLinear", "FAN: LINEAR");
		case EStageFanMode::V:
			return LOCTEXT("FanV", "FAN: V");
		case EStageFanMode::InvertedV:
			return LOCTEXT("FanInvertedV", "FAN: INV V");
		default:
			return LOCTEXT("FanOff", "FAN: OFF");
		}
	}

	EStageFanMode NextFanMode(EStageFanMode Mode)
	{
		switch (Mode)
		{
		case EStageFanMode::Off:
			return EStageFanMode::Linear;
		case EStageFanMode::Linear:
			return EStageFanMode::V;
		case EStageFanMode::V:
			return EStageFanMode::InvertedV;
		default:
			return EStageFanMode::Off;
		}
	}
}

const UStageCraftUITheme& UStageGroupsPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageGroupsPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

UTextBlock* UStageGroupsPanel::MakeText(const FText& Text, int32 Size, FName Typeface, const FLinearColor& Color)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

FButtonStyle UStageGroupsPanel::MakeButtonStyle(bool bLit) const
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();
	const FLinearColor Fill = bLit ? WithAlpha(Palette.AccentColor, 0.35f) : FLinearColor::Transparent;
	FButtonStyle Style = MakeFlatButton(Fill, bLit ? WithAlpha(Palette.AccentColor, 0.5f) : Palette.RowHover, Palette.Divider);
	Style.SetNormal(MakeBox(Fill, CornerRadius, Palette.Divider, 1.f));
	return Style;
}

void UStageGroupsPanel::SetButtonLit(UStageChoiceButton& Button, bool bLit)
{
	Button.SetStyle(MakeButtonStyle(bLit));
	if (UTextBlock* Label = Cast<UTextBlock>(Button.GetChildAt(0)))
	{
		Label->SetColorAndOpacity(FSlateColor(bLit ? GetTheme().TextPrimary : GetTheme().TextSecondary));
	}
}

UStageChoiceButton* UStageGroupsPanel::MakeButton(const FText& Label, const FText& ToolTip, int32 ChoiceIndex, bool bLit)
{
	const UStageCraftUITheme& Palette = GetTheme();
	UStageChoiceButton* Button = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
	Button->SetStyle(MakeButtonStyle(bLit));
	Button->SetToolTipText(ToolTip);
	Button->SetChoiceIndex(ChoiceIndex);
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(Button->AddChild(MakeText(Label, 7, TEXT("Bold"), bLit ? Palette.TextPrimary : Palette.TextSecondary))))
	{
		LabelSlot->SetPadding(FMargin(8.f, 4.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
	}
	return Button;
}

void UStageGroupsPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
}

void UStageGroupsPanel::BuildTree()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Root = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	Root->SetBrush(MakeBox(Palette.PanelBackground));
	Root->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Column"));
	Root->SetContent(Column);
	Column->AddChildToVerticalBox(BuildProgrammerBar());
	CurrentGroupRow = BuildCurrentGroupRow();
	Column->AddChildToVerticalBox(CurrentGroupRow);

	EmptyText = MakeText(LOCTEXT("Empty", "No groups yet. Select fixtures in the viewport or the outliner, then RECORD them as a group."), 8, TEXT("Italic"), Palette.TextSecondary);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("PoolScroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
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

UWidget* UStageGroupsPanel::BuildProgrammerBar()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageGroupsPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 6.f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Header->SetContent(Line);
	const auto Add = [Line](UWidget* Widget, float LeftGap, bool bFill)
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

	UTextBlock* Title = MakeText(LOCTEXT("Title", "GROUPS"), 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetToolTipText(LOCTEXT("TitleTip", "Saved fixture selections. Click a group to select its fixtures, in the order they were recorded."));
	Add(Title, 0.f, false);

	UStageChoiceButton* Record = MakeButton(LOCTEXT("Record", "RECORD"), LOCTEXT("RecordTip", "Record the selected fixtures as a new group."), ChoiceRecord, true);
	Record->OnPicked.BindUObject(this, &ThisClass::HandleProgrammerPicked);
	Add(Record, 12.f, false);

	ProgrammerText = MakeText(FText::GetEmpty(), 7, TEXT("Regular"), Palette.TextSecondary);
	ProgrammerText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Add(ProgrammerText, 12.f, true);

	ClearButton = MakeButton(LOCTEXT("Clear", "CLEAR"), LOCTEXT("ClearTip", "Clear the programmer: fixtures go back to the playing cue and their home look. Undoable."), ChoiceClear, false);
	ClearButton->OnPicked.BindUObject(this, &ThisClass::HandleProgrammerPicked);
	Add(ClearButton, 8.f, false);
	HighlightButton = MakeButton(LOCTEXT("Highlight", "HIGHLIGHT"), LOCTEXT("HighlightTip", "Show the selected fixtures at full, in open white, to find them on stage. Never stored in cues."), ChoiceHighlight, false);
	HighlightButton->OnPicked.BindUObject(this, &ThisClass::HandleProgrammerPicked);
	Add(HighlightButton, 2.f, false);
	FanButton = MakeButton(FanLabel(EStageFanMode::Off), LOCTEXT("FanTip", "How a fader or encoder change spreads over several selected fixtures, in selection order: off (all the same), linear, V or inverted V."), ChoiceFan, false);
	FanButton->OnPicked.BindUObject(this, &ThisClass::HandleProgrammerPicked);
	Add(FanButton, 2.f, false);
	return Header;
}

UWidget* UStageGroupsPanel::BuildCurrentGroupRow()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageGroupsPanel;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("CurrentGroup"));
	Row->SetBrush(MakeBox(Palette.PanelBackground));
	Row->SetPadding(FMargin(10.f, 4.f));
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Line);

	NameField = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("GroupName"));
	FEditableTextBoxStyle TextStyle = MakeInputField(Palette);
	TextStyle.SetPadding(FMargin(6.f, 2.f));
	NameField->SetWidgetStyle(TextStyle);
	NameField->SetRevertTextOnEscape(true);
	NameField->SetToolTipText(LOCTEXT("NameTip", "The group's name. Press Enter to rename it."));
	NameField->OnTextCommitted.AddUniqueDynamic(this, &ThisClass::HandleNameCommitted);
	USizeBox* NameBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	NameBox->SetHeightOverride(NameFieldHeight);
	NameBox->SetContent(NameField);
	if (UHorizontalBoxSlot* NameSlot = Line->AddChildToHorizontalBox(NameBox))
	{
		NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		NameSlot->SetVerticalAlignment(VAlign_Center);
	}
	UStageChoiceButton* Update = MakeButton(LOCTEXT("Update", "UPDATE"), LOCTEXT("UpdateTip", "Replace this group's fixtures with the selected ones."), ChoiceUpdate, false);
	Update->OnPicked.BindUObject(this, &ThisClass::HandleCurrentGroupPicked);
	UStageChoiceButton* Delete = MakeButton(LOCTEXT("Delete", "DELETE"), LOCTEXT("DeleteTip", "Delete this group. Its fixtures stay on stage. Undoable."), ChoiceDelete, false);
	Delete->OnPicked.BindUObject(this, &ThisClass::HandleCurrentGroupPicked);
	for (UWidget* Button : { static_cast<UWidget*>(Update), static_cast<UWidget*>(Delete) })
	{
		if (UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
	return Row;
}

void UStageGroupsPanel::NativeConstruct()
{
	Super::NativeConstruct();
	UWorld* World = GetWorld();
	if (UStageShowLibrarySubsystem* Library = UWorld::GetSubsystem<UStageShowLibrarySubsystem>(World))
	{
		Library->OnGroupsChanged.AddUniqueDynamic(this, &ThisClass::HandleShowDataChanged);
		BoundLibrary = Library;
	}
	if (UStageProgrammerSubsystem* Programmer = UWorld::GetSubsystem<UStageProgrammerSubsystem>(World))
	{
		Programmer->OnValuesChanged.AddUniqueDynamic(this, &ThisClass::HandleShowDataChanged);
		BoundProgrammer = Programmer;
	}
	Rebuild();
}

void UStageGroupsPanel::NativeDestruct()
{
	if (UStageShowLibrarySubsystem* Library = BoundLibrary.Get())
	{
		Library->OnGroupsChanged.RemoveDynamic(this, &ThisClass::HandleShowDataChanged);
	}
	if (UStageProgrammerSubsystem* Programmer = BoundProgrammer.Get())
	{
		Programmer->OnValuesChanged.RemoveDynamic(this, &ThisClass::HandleShowDataChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	BoundLibrary.Reset();
	BoundProgrammer.Reset();
	Super::NativeDestruct();
}

void UStageGroupsPanel::HandleShowDataChanged()
{
	RequestRebuild();
}

void UStageGroupsPanel::RequestRebuild()
{
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	// Next tick: never inside the click that caused the change, and a burst of changes is one rebuild.
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStageGroupsPanel::Rebuild()
{
	using namespace StageCraftWidgetStyle;
	using namespace StageGroupsPanel;
	RebuildTimer.Invalidate();
	if (!Pool)
	{
		return;
	}
	const UStageCraftUITheme& Palette = GetTheme();
	const UStageShowLibrarySubsystem* Library = BoundLibrary.Get();
	const UStageProgrammerSubsystem* Programmer = BoundProgrammer.Get();
	const AModularPlayerController* Controller = GetStageController();

	// Programmer bar.
	const int32 Held = Programmer ? Programmer->GetFixtureCount() : 0;
	ProgrammerText->SetText(Held > 0
		? FText::Format(LOCTEXT("ProgrammerHolds", "Programmer: {0} {0}|plural(one=fixture,other=fixtures)"), FText::AsNumber(Held))
		: LOCTEXT("ProgrammerEmpty", "Programmer empty"));
	ProgrammerText->SetColorAndOpacity(FSlateColor(Held > 0 ? Palette.AccentColor : Palette.TextSecondary));
	SetButtonLit(*ClearButton, Held > 0);
	SetButtonLit(*HighlightButton, Controller && Controller->IsHighlightOn());
	const EStageFanMode Fan = Controller ? Controller->GetFanMode() : EStageFanMode::Off;
	SetButtonLit(*FanButton, Fan != EStageFanMode::Off);
	if (UTextBlock* FanText = Cast<UTextBlock>(FanButton->GetChildAt(0)))
	{
		FanText->SetText(FanLabel(Fan));
	}

	// The pool.
	Pool->ClearChildren();
	TileGroupIds.Reset();
	const TArray<FStageFixtureGroup> NoGroups;
	const TArray<FStageFixtureGroup>& Groups = Library ? Library->GetGroups() : NoGroups;
	const FStageFixtureGroup* Current = nullptr;
	for (const FStageFixtureGroup& Group : Groups)
	{
		const bool bCurrent = Group.Id == CurrentGroupId;
		Current = bCurrent ? &Group : Current;
		UStageChoiceButton* Tile = WidgetTree->ConstructWidget<UStageChoiceButton>(UStageChoiceButton::StaticClass());
		Tile->SetStyle(MakeButtonStyle(bCurrent));
		Tile->SetChoiceIndex(TileGroupIds.Num());
		Tile->SetToolTipText(FText::Format(LOCTEXT("TileTip", "Select the fixtures of {0}."), Group.Name));
		Tile->OnPicked.BindUObject(this, &ThisClass::HandleGroupPicked);
		UVerticalBox* Face = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		UTextBlock* Name = MakeText(Group.Name, 8, TEXT("Bold"), Palette.TextPrimary);
		Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Face->AddChildToVerticalBox(Name);
		Face->AddChildToVerticalBox(MakeText(FText::Format(LOCTEXT("TileCount", "{0} {0}|plural(one=fixture,other=fixtures)"), FText::AsNumber(Group.FixtureIds.Num())), 7, TEXT("Regular"), Palette.TextSecondary));
		if (UButtonSlot* FaceSlot = Cast<UButtonSlot>(Tile->AddChild(Face)))
		{
			FaceSlot->SetPadding(FMargin(8.f, 4.f));
			FaceSlot->SetHorizontalAlignment(HAlign_Left);
		}
		USizeBox* Sized = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Sized->SetWidthOverride(TileWidth);
		Sized->SetHeightOverride(TileHeight);
		Sized->SetContent(Tile);
		Pool->AddChildToWrapBox(Sized);
		TileGroupIds.Add(Group.Id);
	}
	if (!Current)
	{
		CurrentGroupId.Invalidate();
	}
	EmptyText->SetVisibility(Groups.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	CurrentGroupRow->SetVisibility(Current ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (Current && !NameField->HasKeyboardFocus())
	{
		NameField->SetText(Current->Name);
	}
}

void UStageGroupsPanel::HandleProgrammerPicked(int32 ChoiceIndex)
{
	using namespace StageGroupsPanel;
	AModularPlayerController* Controller = GetStageController();
	if (!Controller)
	{
		return;
	}
	switch (ChoiceIndex)
	{
	case ChoiceRecord:
		if (const FGuid Recorded = Controller->RequestRecordGroup(FText::GetEmpty()); Recorded.IsValid())
		{
			CurrentGroupId = Recorded;
		}
		break;
	case ChoiceClear:
		Controller->RequestClearProgrammer();
		break;
	case ChoiceHighlight:
		Controller->RequestSetHighlight(!Controller->IsHighlightOn());
		break;
	case ChoiceFan:
		Controller->RequestSetFanMode(NextFanMode(Controller->GetFanMode()));
		break;
	default:
		break;
	}
	// Highlight and fan are this player's tool state, not show data: no delegate announces them, so the bar refreshes itself.
	RequestRebuild();
}

void UStageGroupsPanel::HandleGroupPicked(int32 TileIndex)
{
	if (!TileGroupIds.IsValidIndex(TileIndex))
	{
		return;
	}
	CurrentGroupId = TileGroupIds[TileIndex];
	if (AModularPlayerController* Controller = GetStageController())
	{
		Controller->RequestSelectGroup(CurrentGroupId);
	}
	RequestRebuild();
}

void UStageGroupsPanel::HandleCurrentGroupPicked(int32 ChoiceIndex)
{
	using namespace StageGroupsPanel;
	AModularPlayerController* Controller = GetStageController();
	if (!Controller || !CurrentGroupId.IsValid())
	{
		return;
	}
	if (ChoiceIndex == ChoiceUpdate)
	{
		Controller->RequestUpdateGroup(CurrentGroupId);
	}
	else if (ChoiceIndex == ChoiceDelete)
	{
		Controller->RequestDeleteGroup(CurrentGroupId);
	}
}

void UStageGroupsPanel::HandleNameCommitted(const FText& InText, ETextCommit::Type CommitMethod)
{
	// Escape reverts the text and must not write; an unchanged or empty name writes nothing either (read back on rebuild).
	AModularPlayerController* Controller = GetStageController();
	if (CommitMethod == ETextCommit::OnCleared || !Controller || !CurrentGroupId.IsValid())
	{
		return;
	}
	if (!Controller->RequestRenameGroup(CurrentGroupId, InText))
	{
		RequestRebuild();
	}
}

#undef LOCTEXT_NAMESPACE
