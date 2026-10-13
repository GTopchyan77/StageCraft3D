// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/StageOutlinerPanel.h"

#include "Actors/ModularBaseActor.h"
#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
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
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Data/BaseItemData.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Player/ModularPlayerController.h"
#include "Subsystems/StageSessionSubsystem.h"
#include "TimerManager.h"
#include "UI/StageCraftUITheme.h"
#include "UI/StageCraftWidgetStyle.h"
#include "UI/StageToolButton.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StageOutlinerPanel)

#define LOCTEXT_NAMESPACE "StageOutlinerPanel"

namespace StageOutlinerPanel
{
	constexpr float IconSize = 12.f;
	constexpr float ItemIndent = 14.f;
	/** Partially set group flags (some items hidden / locked) draw their icon at this opacity. */
	constexpr float MixedOpacity = 0.5f;
	/** Unlocked items show a faint open lock, so the column reads as a toggle without shouting. */
	constexpr float UnsetLockOpacity = 0.3f;

	UTextBlock* MakeText(UWidgetTree& Tree, int32 Size, FName Typeface, const FLinearColor& Color)
	{
		UTextBlock* Block = Tree.ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Block->SetFont(StageCraftWidgetStyle::MakeFont(Size, Typeface));
		Block->SetColorAndOpacity(FSlateColor(Color));
		return Block;
	}

	/** Ctrl (either key), read from Slate like the viewport's Ctrl+Click, so clicks mean the same in the panel and the viewport. */
	EStageSelectionClick GetClickFromKeyboard()
	{
		return FSlateApplication::IsInitialized() && FSlateApplication::Get().GetModifierKeys().IsControlDown()
			? EStageSelectionClick::Toggle
			: EStageSelectionClick::Replace;
	}

	FName GetVisibilityBrush(EStageOutlinerFlagState Hidden)
	{
		return Hidden == EStageOutlinerFlagState::All ? FName(TEXT("Icons.Hidden")) : FName(TEXT("Icons.Visible"));
	}

	float GetVisibilityOpacity(EStageOutlinerFlagState Hidden)
	{
		return Hidden == EStageOutlinerFlagState::Some ? MixedOpacity : 1.f;
	}

	FName GetLockBrush(EStageOutlinerFlagState Locked)
	{
		return Locked == EStageOutlinerFlagState::None ? FName(TEXT("Icons.Unlock")) : FName(TEXT("Icons.Lock"));
	}

	float GetLockOpacity(EStageOutlinerFlagState Locked)
	{
		return Locked == EStageOutlinerFlagState::All ? 1.f : (Locked == EStageOutlinerFlagState::Some ? MixedOpacity : UnsetLockOpacity);
	}

	EStageOutlinerFlagState ToFlagState(bool bSet)
	{
		return bSet ? EStageOutlinerFlagState::All : EStageOutlinerFlagState::None;
	}
}

// --- Row ---

void UStageOutlinerRow::BuildTree(bool bWithChevron, float Indent)
{
	using namespace StageCraftWidgetStyle;
	check(Theme);

	RowBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("RowBorder"));
	RowBorder->SetPadding(FMargin(4.f, 0.f));
	WidgetTree->RootWidget = RowBorder;

	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Line"));
	RowBorder->SetContent(Line);

	USpacer* IndentSpacer = WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass());
	IndentSpacer->SetSize(FVector2D(Indent, 1.f));
	Line->AddChildToHorizontalBox(IndentSpacer);

	const auto AddFixed = [Line](UWidget* Widget)
	{
		if (UHorizontalBoxSlot* FixedSlot = Line->AddChildToHorizontalBox(Widget))
		{
			FixedSlot->SetVerticalAlignment(VAlign_Center);
		}
	};

	if (bWithChevron)
	{
		ExpandButton = MakeIconButton(TEXT("Icons.ChevronDown"), LOCTEXT("ExpandTip", "Show or hide the items of this group."), 1.f, ExpandImage);
		ExpandButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleExpandClicked);
		AddFixed(ExpandButton);
	}

	VisibilityButton = MakeIconButton(TEXT("Icons.Visible"), FText::GetEmpty(), 1.f, VisibilityImage);
	VisibilityButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleVisibilityClicked);
	AddFixed(VisibilityButton);

	LockButton = MakeIconButton(TEXT("Icons.Unlock"), FText::GetEmpty(), 1.f, LockImage);
	LockButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleLockClicked);
	AddFixed(LockButton);

	RowButton = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass(), TEXT("RowButton"));
	RowButton->SetStyle(MakeFlatButton(FLinearColor::Transparent, Theme->RowHover, Theme->Divider));
	RowButton->OnClicked.AddUniqueDynamic(this, &ThisClass::HandleRowClicked);
	if (UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(RowButton))
	{
		ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ButtonSlot->SetPadding(FMargin(2.f, 0.f, 0.f, 0.f));
	}

	UHorizontalBox* Label = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("Label"));
	if (UButtonSlot* LabelSlot = Cast<UButtonSlot>(RowButton->AddChild(Label)))
	{
		LabelSlot->SetPadding(FMargin(4.f, 3.f));
		LabelSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	NameText = StageOutlinerPanel::MakeText(*WidgetTree, 8, TEXT("Regular"), Theme->TextPrimary);
	NameText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	NameText->SetClipping(EWidgetClipping::ClipToBounds);
	if (UHorizontalBoxSlot* NameSlot = Label->AddChildToHorizontalBox(NameText))
	{
		NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		NameSlot->SetVerticalAlignment(VAlign_Center);
	}

	DetailText = StageOutlinerPanel::MakeText(*WidgetTree, 7, TEXT("Regular"), Theme->TextSecondary);
	DetailText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	DetailText->SetClipping(EWidgetClipping::ClipToBounds);
	if (UHorizontalBoxSlot* DetailSlot = Label->AddChildToHorizontalBox(DetailText))
	{
		// Both texts share the width and truncate; an auto-sized detail would overlap a long name in a narrow panel.
		FSlateChildSize DetailSize(ESlateSizeRule::Fill);
		DetailSize.Value = 0.6f;
		DetailSlot->SetSize(DetailSize);
		DetailSlot->SetHorizontalAlignment(HAlign_Right);
		DetailSlot->SetVerticalAlignment(VAlign_Center);
		DetailSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}
}

UButton* UStageOutlinerRow::MakeIconButton(FName BrushName, const FText& ToolTip, float Opacity, TObjectPtr<UImage>& OutImage)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>(UStageToolButton::StaticClass());
	Button->SetStyle(StageCraftWidgetStyle::MakeFlatButton(FLinearColor::Transparent, Theme->RowHover, Theme->Divider));
	Button->SetToolTipText(ToolTip);

	OutImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	OutImage->SetDesiredSizeOverride(FVector2D(StageOutlinerPanel::IconSize));
	SetIcon(OutImage, BrushName, Opacity);
	if (UButtonSlot* IconSlot = Cast<UButtonSlot>(Button->AddChild(OutImage)))
	{
		IconSlot->SetPadding(FMargin(3.f));
	}
	return Button;
}

void UStageOutlinerRow::SetIcon(UImage* Image, FName BrushName, float Opacity) const
{
	if (Image)
	{
		Image->SetBrush(*StageCraftWidgetStyle::AppBrush(BrushName));
		Image->SetColorAndOpacity(StageCraftWidgetStyle::WithAlpha(Theme->TextPrimary, Opacity));
	}
}

void UStageOutlinerRow::InitializeGroup(const FStageOutlinerGroup& Group, bool bExpanded, const UStageCraftUITheme& InTheme)
{
	using namespace StageOutlinerPanel;
	if (!ensureMsgf(!WidgetTree->RootWidget, TEXT("%s: an outliner row is initialized once."), *GetName()))
	{
		return;
	}

	Theme = &InTheme;
	bGroupRow = true;
	GroupCategory = Group.Category;
	BuildTree(/*bWithChevron*/ true, 0.f);

	SetIcon(ExpandImage, bExpanded ? TEXT("Icons.ChevronDown") : TEXT("Icons.ChevronRight"), 1.f);
	SetIcon(VisibilityImage, GetVisibilityBrush(Group.Hidden), GetVisibilityOpacity(Group.Hidden));
	SetIcon(LockImage, GetLockBrush(Group.Locked), GetLockOpacity(Group.Locked));
	VisibilityButton->SetToolTipText(FText::Format(LOCTEXT("GroupVisibilityTip", "Show or hide every {0} item."), Group.Label));
	LockButton->SetToolTipText(FText::Format(LOCTEXT("GroupLockTip", "Lock or unlock every {0} item against moving and resizing."), Group.Label));
	RowButton->SetToolTipText(FText::Format(LOCTEXT("GroupSelectTip", "Select every {0} item listed (Ctrl: add to or remove from the selection)."), Group.Label));

	NameText->SetText(Group.Label.ToUpper());
	NameText->SetFont(StageCraftWidgetStyle::MakeFont(8, TEXT("Bold")));
	NameText->SetColorAndOpacity(FSlateColor(InTheme.TextSecondary));
	DetailText->SetText(FText::AsNumber(Group.Items.Num()));
	ApplyRowStyle();
}

void UStageOutlinerRow::InitializeItem(const FStageOutlinerItem& Item, const UStageCraftUITheme& InTheme)
{
	using namespace StageOutlinerPanel;
	if (!ensureMsgf(!WidgetTree->RootWidget, TEXT("%s: an outliner row is initialized once."), *GetName()))
	{
		return;
	}

	Theme = &InTheme;
	bGroupRow = false;
	InstanceId = Item.InstanceId;
	BuildTree(/*bWithChevron*/ false, ItemIndent + IconSize + 6.f);

	SetIcon(VisibilityImage, GetVisibilityBrush(ToFlagState(Item.bHidden)), 1.f);
	SetIcon(LockImage, GetLockBrush(ToFlagState(Item.bLocked)), GetLockOpacity(ToFlagState(Item.bLocked)));
	VisibilityButton->SetToolTipText(Item.bHidden ? LOCTEXT("ShowTip", "Hidden: click to show it on stage.") : LOCTEXT("HideTip", "Visible: click to hide it on stage."));
	LockButton->SetToolTipText(Item.bLocked ? LOCTEXT("UnlockTip", "Locked: click to allow moving and resizing.") : LOCTEXT("LockTip", "Click to lock it against moving and resizing."));
	RowButton->SetToolTipText(LOCTEXT("ItemSelectTip", "Select it (Ctrl: add to or remove from the selection)."));

	NameText->SetText(Item.Label);
	NameText->SetColorAndOpacity(FSlateColor(Item.bHidden ? InTheme.TextDisabled : InTheme.TextPrimary));
	DetailText->SetText(Item.Quantity > 1
		? FText::Format(LOCTEXT("ItemDetailQuantity", "{0} · {1} {1}|plural(one=segment,other=segments)"), Item.ModelName, Item.Quantity)
		: Item.ModelName);
	ApplyRowStyle();
}

void UStageOutlinerRow::SetSelected(bool bInSelected)
{
	if (bSelected != bInSelected)
	{
		bSelected = bInSelected;
		ApplyRowStyle();
	}
}

void UStageOutlinerRow::ApplyRowStyle()
{
	if (!RowBorder || !Theme)
	{
		return;
	}
	const FLinearColor Fill = bSelected ? StageCraftWidgetStyle::WithAlpha(Theme->SelectionColor, 0.35f)
		: (bGroupRow ? Theme->PanelHeader : FLinearColor::Transparent);
	RowBorder->SetBrush(StageCraftWidgetStyle::MakeBox(Fill));
}

void UStageOutlinerRow::HandleRowClicked()
{
	OnClicked.ExecuteIfBound(this);
}

void UStageOutlinerRow::HandleVisibilityClicked()
{
	OnVisibilityClicked.ExecuteIfBound(this);
}

void UStageOutlinerRow::HandleLockClicked()
{
	OnLockClicked.ExecuteIfBound(this);
}

void UStageOutlinerRow::HandleExpandClicked()
{
	OnExpandClicked.ExecuteIfBound(this);
}

// --- Panel ---

const UStageCraftUITheme& UStageOutlinerPanel::GetTheme() const
{
	return StageCraftWidgetStyle::ResolveTheme(Theme);
}

AModularPlayerController* UStageOutlinerPanel::GetStageController() const
{
	return Cast<AModularPlayerController>(GetOwningPlayer());
}

void UStageOutlinerPanel::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	if (FilterBox)
	{
		FilterBox->OnTextChanged.AddUniqueDynamic(this, &ThisClass::HandleFilterChanged);
	}
}

void UStageOutlinerPanel::BuildTree()
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
	Column->AddChildToVerticalBox(BuildFilterRow());

	EmptyText = StageOutlinerPanel::MakeText(*WidgetTree, 8, TEXT("Italic"), Palette.TextSecondary);
	EmptyText->SetAutoWrapText(true);
	EmptyText->SetJustification(ETextJustify::Center);
	if (UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(EmptyText))
	{
		EmptySlot->SetPadding(FMargin(12.f, 16.f));
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("Scroll"));
	Scroll->SetWidgetBarStyle(MakeScrollBar(Palette));
	Scroll->SetScrollbarThickness(FVector2D(6.f, 6.f));
	Scroll->SetScrollbarPadding(FMargin(2.f));
	if (UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	RowList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RowList"));
	Scroll->AddChild(RowList);

	UImage* Divider = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("FooterDivider"));
	Divider->SetBrush(MakeBox(Palette.Divider));
	Divider->SetDesiredSizeOverride(FVector2D(1.f, 1.f));
	Column->AddChildToVerticalBox(Divider);

	FooterText = StageOutlinerPanel::MakeText(*WidgetTree, 7, TEXT("Regular"), Palette.TextSecondary);
	if (UVerticalBoxSlot* FooterSlot = Column->AddChildToVerticalBox(FooterText))
	{
		FooterSlot->SetPadding(FMargin(10.f, 5.f));
	}
}

UWidget* UStageOutlinerPanel::BuildHeader()
{
	using namespace StageCraftWidgetStyle;
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* Header = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Header"));
	Header->SetBrush(MakeBox(Palette.PanelHeader));
	Header->SetPadding(FMargin(10.f, 8.f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("HeaderRow"));
	Header->SetContent(Row);

	UTextBlock* Title = StageOutlinerPanel::MakeText(*WidgetTree, 9, TEXT("Bold"), Palette.TextPrimary);
	Title->SetText(LOCTEXT("Title", "OUTLINER"));
	Title->SetToolTipText(LOCTEXT("TitleTip", "Every item on the stage, by discipline. Eye: show / hide. Lock: protect against moving and resizing."));
	if (UHorizontalBoxSlot* TitleSlot = Row->AddChildToHorizontalBox(Title))
	{
		TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}

	CountText = StageOutlinerPanel::MakeText(*WidgetTree, 8, TEXT("Regular"), Palette.TextSecondary);
	if (UHorizontalBoxSlot* CountSlot = Row->AddChildToHorizontalBox(CountText))
	{
		CountSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Header;
}

UWidget* UStageOutlinerPanel::BuildFilterRow()
{
	const UStageCraftUITheme& Palette = GetTheme();

	UBorder* FilterRow = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("FilterRow"));
	FilterRow->SetBrush(StageCraftWidgetStyle::MakeBox(Palette.PanelBackground));
	FilterRow->SetPadding(FMargin(8.f, 8.f, 8.f, 4.f));

	FilterBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("FilterBox"));
	FilterBox->SetWidgetStyle(StageCraftWidgetStyle::MakeInputField(Palette));
	FilterBox->SetHintText(LOCTEXT("FilterHint", "Filter by name, model or discipline"));
	FilterBox->SetRevertTextOnEscape(true);
	FilterBox->SetClearKeyboardFocusOnCommit(true);
	FilterRow->SetContent(FilterBox);
	return FilterRow;
}

void UStageOutlinerPanel::NativeConstruct()
{
	Super::NativeConstruct();

	UStageSessionSubsystem* SessionSubsystem = UWorld::GetSubsystem<UStageSessionSubsystem>(GetWorld());
	Session = SessionSubsystem;
	if (SessionSubsystem)
	{
		SessionSubsystem->OnItemListChanged.AddUniqueDynamic(this, &ThisClass::HandleItemListChanged);
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

void UStageOutlinerPanel::NativeDestruct()
{
	if (UStageSessionSubsystem* SessionSubsystem = Session.Get())
	{
		SessionSubsystem->OnItemListChanged.RemoveDynamic(this, &ThisClass::HandleItemListChanged);
	}
	if (USelectionComponent* SelectionComponent = Selection.Get())
	{
		SelectionComponent->OnSelectionSetChanged.RemoveDynamic(this, &ThisClass::HandleSelectionSetChanged);
	}
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RebuildTimer);
	}
	Session.Reset();
	Selection.Reset();
	ClearRows();
	Super::NativeDestruct();
}

void UStageOutlinerPanel::RequestRebuild()
{
	// One rebuild per frame however many items changed (a group toggle, a scene load), and never inside the callback that changed them.
	UWorld* World = GetWorld();
	if (!World || RebuildTimer.IsValid())
	{
		return;
	}
	RebuildTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ThisClass::Rebuild));
}

void UStageOutlinerPanel::ClearRows()
{
	if (RowList)
	{
		RowList->ClearChildren();
	}
	Rows.Reset();
}

void UStageOutlinerPanel::AddRow(TFunctionRef<void(UStageOutlinerRow&)> Initialize)
{
	UStageOutlinerRow* Row = CreateWidget<UStageOutlinerRow>(this, UStageOutlinerRow::StaticClass());
	Row->OnClicked.BindUObject(this, &ThisClass::HandleRowClicked);
	Row->OnVisibilityClicked.BindUObject(this, &ThisClass::HandleVisibilityClicked);
	Row->OnLockClicked.BindUObject(this, &ThisClass::HandleLockClicked);
	Row->OnExpandClicked.BindUObject(this, &ThisClass::HandleExpandClicked);

	// Built before it joins the list: adding a row to a live panel creates its Slate widget from the tree it has at that moment.
	Initialize(*Row);
	RowList->AddChildToVerticalBox(Row);
	Rows.Add(Row);
}

void UStageOutlinerPanel::Rebuild()
{
	RebuildTimer.Invalidate();
	ClearRows();
	ItemsById.Reset();
	if (!RowList)
	{
		return;
	}

	TArray<FStageOutlinerItem> Items;
	if (const UStageSessionSubsystem* SessionSubsystem = Session.Get())
	{
		for (AModularBaseActor* Actor : SessionSubsystem->GetPlacedItems())
		{
			if (Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			const UBaseItemData* Data = Actor->GetItemData();
			FStageOutlinerItem& Item = Items.AddDefaulted_GetRef();
			Item.InstanceId = Actor->GetInstanceId();
			Item.Label = Actor->GetInstanceLabel();
			Item.ModelName = Data ? Data->DisplayName : FText::GetEmpty();
			Item.Category = Data ? Data->CategoryTag : FGameplayTag();
			Item.Quantity = Actor->GetQuantity();
			Item.bHidden = Actor->IsHiddenOnStage();
			Item.bLocked = Actor->IsTransformLocked();
			ItemsById.Add(Item.InstanceId, Actor);
		}
	}
	const int32 TotalItems = Items.Num();
	Groups = StageOutliner::BuildGroups(MoveTemp(Items));

	const UStageCraftUITheme& Palette = GetTheme();
	const bool bFiltering = !FilterQuery.TrimStartAndEnd().IsEmpty();
	int32 Listed = 0;
	int32 Hidden = 0;
	int32 Locked = 0;
	for (const FStageOutlinerGroup& Group : Groups)
	{
		TArray<const FStageOutlinerItem*> Matching;
		for (const FStageOutlinerItem& Item : Group.Items)
		{
			Hidden += Item.bHidden ? 1 : 0;
			Locked += Item.bLocked ? 1 : 0;
			if (StageOutliner::MatchesFilter(Item, Group.Label, FilterQuery))
			{
				Matching.Add(&Item);
			}
		}
		if (Matching.IsEmpty())
		{
			continue;
		}

		// While filtering every matching group is open, as in the Library's search.
		const bool bExpanded = bFiltering || !CollapsedGroups.Contains(Group.Category);
		AddRow([&Group, bExpanded, &Palette](UStageOutlinerRow& Row) { Row.InitializeGroup(Group, bExpanded, Palette); });
		Listed += Matching.Num();
		if (bExpanded)
		{
			for (const FStageOutlinerItem* Item : Matching)
			{
				AddRow([Item, &Palette](UStageOutlinerRow& Row) { Row.InitializeItem(*Item, Palette); });
			}
		}
	}

	if (CountText)
	{
		CountText->SetText(FText::Format(LOCTEXT("Count", "{0} {0}|plural(one=item,other=items)"), TotalItems));
	}
	if (FooterText)
	{
		FooterText->SetText(FText::Format(LOCTEXT("Footer", "{0} listed   ·   {1} hidden   ·   {2} locked"), Listed, Hidden, Locked));
	}
	if (EmptyText)
	{
		const bool bEmpty = Listed == 0;
		EmptyText->SetVisibility(bEmpty ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		EmptyText->SetText(TotalItems == 0 ? LOCTEXT("EmptyStage", "The stage is empty. Place items from the Library.") : LOCTEXT("NoMatch", "No items match the filter."));
	}
	RefreshSelection();
}

void UStageOutlinerPanel::RefreshSelection()
{
	const USelectionComponent* SelectionComponent = Selection.Get();
	for (UStageOutlinerRow* Row : Rows)
	{
		if (!Row)
		{
			continue;
		}
		if (!SelectionComponent)
		{
			Row->SetSelected(false);
			continue;
		}
		if (!Row->IsGroupRow())
		{
			Row->SetSelected(SelectionComponent->IsActorSelected(ResolveItem(Row->GetInstanceId())));
			continue;
		}
		const TArray<AModularBaseActor*> GroupItems = ResolveGroupItems(Row->GetGroupCategory());
		Row->SetSelected(!GroupItems.IsEmpty() && Algo::AllOf(GroupItems, [SelectionComponent](const AModularBaseActor* Item) { return SelectionComponent->IsActorSelected(Item); }));
	}
}

AModularBaseActor* UStageOutlinerPanel::ResolveItem(const FGuid& InstanceId) const
{
	const TWeakObjectPtr<AModularBaseActor>* Found = ItemsById.Find(InstanceId);
	AModularBaseActor* Item = Found ? Found->Get() : nullptr;
	return Item && !Item->IsActorBeingDestroyed() ? Item : nullptr;
}

const FStageOutlinerGroup* UStageOutlinerPanel::FindGroup(const FGameplayTag& GroupCategory) const
{
	return Groups.FindByPredicate([&GroupCategory](const FStageOutlinerGroup& Group) { return Group.Category == GroupCategory; });
}

TArray<AModularBaseActor*> UStageOutlinerPanel::ResolveGroupItems(const FGameplayTag& GroupCategory) const
{
	TArray<AModularBaseActor*> Result;
	if (const FStageOutlinerGroup* Group = FindGroup(GroupCategory))
	{
		for (const FStageOutlinerItem& Item : Group->Items)
		{
			AModularBaseActor* Actor = StageOutliner::MatchesFilter(Item, Group->Label, FilterQuery) ? ResolveItem(Item.InstanceId) : nullptr;
			if (Actor)
			{
				Result.Add(Actor);
			}
		}
	}
	return Result;
}

void UStageOutlinerPanel::SetFlag(const TArray<AModularBaseActor*>& Items, const FGameplayTag& ParameterId, bool bValue)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller)
	{
		return;
	}
	for (AModularBaseActor* Item : Items)
	{
		const bool bCurrent = ParameterId == StageCraftTags::Param_Info_Visible ? !Item->IsHiddenOnStage() : Item->IsTransformLocked();
		if (bCurrent != bValue)
		{
			// The request bridge validates and reports; the rows update from the session's change notification (read-back).
			Controller->RequestParameterChange(Item, ParameterId, FStageParameterValue::MakeBool(bValue));
		}
	}
}

void UStageOutlinerPanel::HandleRowClicked(UStageOutlinerRow* Row)
{
	AModularPlayerController* Controller = GetStageController();
	if (!Controller || !Row)
	{
		return;
	}

	TArray<AModularBaseActor*> Targets;
	if (Row->IsGroupRow())
	{
		Targets = ResolveGroupItems(Row->GetGroupCategory());
	}
	else if (AModularBaseActor* Item = ResolveItem(Row->GetInstanceId()))
	{
		Targets.Add(Item);
	}
	if (!Targets.IsEmpty())
	{
		Controller->RequestSelectItems(Targets, StageOutlinerPanel::GetClickFromKeyboard());
	}
}

void UStageOutlinerPanel::HandleVisibilityClicked(UStageOutlinerRow* Row)
{
	if (!Row)
	{
		return;
	}
	if (!Row->IsGroupRow())
	{
		if (AModularBaseActor* Item = ResolveItem(Row->GetInstanceId()))
		{
			SetFlag({ Item }, StageCraftTags::Param_Info_Visible, Item->IsHiddenOnStage());
		}
		return;
	}

	// As in the Unreal Editor's outliner: a group with anything visible hides everything; an all-hidden group shows everything.
	const TArray<AModularBaseActor*> Items = ResolveGroupItems(Row->GetGroupCategory());
	const bool bAnyVisible = Algo::AnyOf(Items, [](const AModularBaseActor* Item) { return !Item->IsHiddenOnStage(); });
	SetFlag(Items, StageCraftTags::Param_Info_Visible, !bAnyVisible);
}

void UStageOutlinerPanel::HandleLockClicked(UStageOutlinerRow* Row)
{
	if (!Row)
	{
		return;
	}
	if (!Row->IsGroupRow())
	{
		if (AModularBaseActor* Item = ResolveItem(Row->GetInstanceId()))
		{
			SetFlag({ Item }, StageCraftTags::Param_Info_Locked, !Item->IsTransformLocked());
		}
		return;
	}

	// Locking errs on the safe side: a partly locked group locks completely; only a fully locked group unlocks.
	const TArray<AModularBaseActor*> Items = ResolveGroupItems(Row->GetGroupCategory());
	const bool bAllLocked = !Items.IsEmpty() && Algo::AllOf(Items, [](const AModularBaseActor* Item) { return Item->IsTransformLocked(); });
	SetFlag(Items, StageCraftTags::Param_Info_Locked, !bAllLocked);
}

void UStageOutlinerPanel::HandleExpandClicked(UStageOutlinerRow* Row)
{
	if (!Row || !Row->IsGroupRow())
	{
		return;
	}
	const FGameplayTag Category = Row->GetGroupCategory();
	if (CollapsedGroups.Remove(Category) == 0)
	{
		CollapsedGroups.Add(Category);
	}
	RequestRebuild();
}

void UStageOutlinerPanel::HandleItemListChanged()
{
	RequestRebuild();
}

void UStageOutlinerPanel::HandleSelectionSetChanged(int32 Count)
{
	RefreshSelection();
}

void UStageOutlinerPanel::HandleFilterChanged(const FText& Text)
{
	FilterQuery = Text.ToString();
	RequestRebuild();
}

#undef LOCTEXT_NAMESPACE
